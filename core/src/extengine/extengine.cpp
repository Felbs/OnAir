// External engine (dect2/extengine/extengine.h): the process, its two pipes, the status reader and the TS reader.
// The engine owns the radio and the decoding; this file only carries bytes and words between it and OnAir.
#include "dect2/extengine/extengine.h"
#include "dect2/common/thread_name.h"
#include "dect2/mcp/mcp.h"
#include "dect2/tv/iptv.h"
#include "../net/netcompat.h"
#include "../net/tcp_client.h"
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace dect2::extengine {

namespace {
std::string fmtSetting(const mcp::Json& j) {
    // {"gain:RFGR": 4, "gain:IFGR": 39} -> "RFGR 4  IFGR 39"
    std::string s;
    for (auto& kv : j.obj) {
        std::string k = kv.first;
        const size_t c = k.find(':');
        if (c != std::string::npos) k = k.substr(c + 1);
        if (!s.empty()) s += "  ";
        s += k + " " + (kv.second.isNum() ? std::to_string((int)kv.second.num) : kv.second.str);
    }
    return s;
}
}  // namespace

bool applyLine(const std::string& line, Status& s) {
    mcp::Json j;
    std::string err;
    if (!mcp::parseJson(line, j, err) || !j.isObj()) return false;
    const std::string ev = j.strOr("ev");
    if (ev == "hello") {
        s.tcpPort = (int)j.numOr("tcp", s.tcpPort);
        s.channels.clear(); s.channelLabels.clear();
        if (const mcp::Json* ch = j.get("channels")) {
            for (auto& kv : ch->obj) {
                const int rf = atoi(kv.first.c_str());
                if (rf <= 0) continue;
                const mcp::Json& c = kv.second;
                std::string std_ = c.strOr("standard");
                if (std_.empty()) std_ = c.has("atsc1_hold") ? "atsc1" : c.has("atsc3_gains") ? "atsc3" : "";
                std::string lab = "RF " + std::to_string(rf);
                if (std_ == "atsc1") lab += "  ATSC 1.0"; else if (std_ == "atsc3") lab += "  ATSC 3.0";
                s.channels.push_back(rf);
                s.channelLabels.push_back(lab);
            }
        }
        if (const mcp::Json* r = j.get("radio")) { if (r->isObj()) { s.radioOwner = r->strOr("owner"); s.radioPurpose = r->strOr("purpose"); } else s.radioOwner.clear(); }
        if (s.state == "starting") s.state = "idle";
        return true;
    }
    if (ev == "up") {
        s.rf = (int)j.numOr("rf", s.rf);
        s.standard = j.strOr("standard", s.standard);
        s.tcpPort = (int)j.numOr("tcp", s.tcpPort);
        s.up = true; s.state = "up"; s.message.clear();
        return true;
    }
    if (ev == "error") {
        s.state = "error"; s.up = false;
        s.message = j.strOr("msg", "error");
        if (j.has("rf")) s.rf = (int)j.numOr("rf", s.rf);
        return true;
    }
    if (ev == "released") {
        s.up = false; s.state = "idle";
        s.servoState.clear(); s.setting.clear(); s.merDb = -1; s.video = false; s.picturesPerS = 0;
        return true;
    }
    if (ev == "status") {
        if (j.has("rf") && j.get("rf")->isNum()) s.rf = (int)j.numOr("rf", s.rf);
        s.standard = j.strOr("standard", s.standard);
        s.up = j.boolOr("up", s.up);
        if (s.up && s.state != "error") s.state = "up";
        s.t = j.numOr("t", s.t);
        if (const mcp::Json* r = j.get("radio")) { if (r->isObj()) { s.radioOwner = r->strOr("owner"); s.radioPurpose = r->strOr("purpose"); } else { s.radioOwner.clear(); s.radioPurpose.clear(); } }
        const std::string e = j.strOr("error");
        if (!e.empty()) s.message = e;
        if (const mcp::Json* l = j.get("last")) {
            if (l->isObj()) {
                s.servoState = l->strOr("state", s.servoState);
                if (const mcp::Json* st = l->get("setting")) if (st->isObj()) s.setting = fmtSetting(*st);
                if (const mcp::Json* m = l->get("mer_db")) s.merDb = m->isNum() ? m->num : -1;
                s.video = l->boolOr("video", s.video);
                s.picturesPerS = l->numOr("pictures_per_s", s.picturesPerS);
                if (l->has("packets")) s.packets = (long long)l->numOr("packets", 0);
                if (l->has("cc_errors")) s.ccErrors = (long long)l->numOr("cc_errors", 0);
                if (l->has("nulls")) s.nulls = (long long)l->numOr("nulls", 0);
                if (l->has("tei")) s.tei = (long long)l->numOr("tei", 0);
                if (const mcp::Json* f = l->get("frames")) s.frames = f->isNum() ? (long long)f->num : -1;
                if (const mcp::Json* f = l->get("fec_pct")) s.fecPct = f->isNum() ? f->num : -1;
                if (l->has("sink_bytes")) s.sinkBytes = (long long)l->numOr("sink_bytes", 0);
            }
        }
        return true;
    }
    return true;   // some other event: known JSON, nothing to fold
}

// ---- the process ----
struct Engine::Impl {
    std::string command, host;
    int port = 0;
    Sink sink;
    std::function<bool()> alive;
    mutable std::mutex mu;
    Status st;
    std::thread reader, tsThread;
    std::atomic<bool> stop{false};
    std::atomic<bool> exited{false};
    std::mutex wmu;
#ifdef _WIN32
    HANDLE proc = nullptr, hin = nullptr, hout = nullptr;
#else
    pid_t pid = -1;
    int fin = -1, fout = -1;
#endif

    bool spawn(std::string& err) {
#ifdef _WIN32
        SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof sa; sa.bInheritHandle = TRUE;
        HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr;
        if (!CreatePipe(&inR, &inW, &sa, 1 << 16) || !CreatePipe(&outR, &outW, &sa, 1 << 20)) { err = "cannot create the pipes"; return false; }
        SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
        HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        STARTUPINFOW si{}; si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = inR; si.hStdOutput = outW; si.hStdError = nul;
        PROCESS_INFORMATION pi{};
        const int n = MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, nullptr, 0);
        std::vector<wchar_t> line((size_t)std::max(n, 1));
        MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, line.data(), n);
        const BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi);
        CloseHandle(inR); CloseHandle(outW); if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        if (!ok) { err = "cannot start: " + command; CloseHandle(inW); CloseHandle(outR); return false; }
        CloseHandle(pi.hThread);
        proc = pi.hProcess; hin = inW; hout = outR;
        return true;
#else
        int in[2], out[2];
        if (pipe(in) != 0 || pipe(out) != 0) { err = "cannot create the pipes"; return false; }
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, in[0], 0);
        posix_spawn_file_actions_adddup2(&fa, out[1], 1);
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
        for (int fd : {in[0], in[1], out[0], out[1]}) posix_spawn_file_actions_addclose(&fa, fd);
        std::vector<char*> av = {(char*)"/bin/sh", (char*)"-c", (char*)command.c_str(), nullptr};
        const int r = posix_spawn(&pid, "/bin/sh", &fa, nullptr, av.data(), environ);
        posix_spawn_file_actions_destroy(&fa);
        close(in[0]); close(out[1]);
        if (r != 0) { err = "cannot start: " + command; close(in[1]); close(out[0]); pid = -1; return false; }
        fin = in[1]; fout = out[0];
        return true;
#endif
    }

    bool write(const std::string& s) {
        std::lock_guard<std::mutex> lk(wmu);
#ifdef _WIN32
        if (!hin) return false;
        DWORD n = 0;
        return WriteFile(hin, s.data(), (DWORD)s.size(), &n, nullptr) && n == s.size();
#else
        if (fin < 0) return false;
        return ::write(fin, s.data(), s.size()) == (ssize_t)s.size();
#endif
    }

    long readSome(char* b, size_t n) {
#ifdef _WIN32
        DWORD got = 0;
        if (!hout || !ReadFile(hout, b, (DWORD)n, &got, nullptr)) return -1;
        return (long)got;
#else
        return (long)::read(fout, b, n);
#endif
    }

    bool processAlive() {
#ifdef _WIN32
        return proc && WaitForSingleObject(proc, 0) == WAIT_TIMEOUT;
#else
        if (pid <= 0) return false;
        int status = 0;
        return waitpid(pid, &status, WNOHANG) == 0;
#endif
    }

    void kill() {
#ifdef _WIN32
        if (proc) { TerminateProcess(proc, 1); }
#else
        if (pid > 0) ::kill(pid, SIGKILL);
#endif
    }

    void closeHandles() {
#ifdef _WIN32
        if (hin) CloseHandle(hin); if (hout) CloseHandle(hout); if (proc) CloseHandle(proc);
        hin = hout = proc = nullptr;
#else
        if (fin >= 0) close(fin); if (fout >= 0) close(fout);
        if (pid > 0) { int s = 0; waitpid(pid, &s, 0); }
        fin = fout = -1; pid = -1;
#endif
    }

    void readerLoop() {
        setThreadName("onair-extengine");
        std::string acc;
        char b[8192];
        while (!stop) {
            const long n = readSome(b, sizeof b);
            if (n <= 0) break;
            acc.append(b, (size_t)n);
            size_t nl;
            while ((nl = acc.find('\n')) != std::string::npos) {
                std::string line = acc.substr(0, nl);
                acc.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                std::lock_guard<std::mutex> lk(mu);
                applyLine(line, st);
            }
        }
        exited = true;
        std::lock_guard<std::mutex> lk(mu);
        if (!stop) { st.state = "error"; st.up = false; if (st.message.empty()) st.message = "the engine exited"; }
    }

    void tsLoop(std::atomic<uint64_t>& bytes) {
        setThreadName("onair-extengine-ts");
        iptv::TsAligner al([this](const uint8_t* p, size_t n) { if (sink) sink(p, n / 188); });
        sock_t s = kBadSock;
        std::vector<char> buf(1 << 16);
        auto lastAlive = std::chrono::steady_clock::now();
        while (!stop) {
            if (alive && std::chrono::steady_clock::now() - lastAlive > std::chrono::milliseconds(500)) {
                lastAlive = std::chrono::steady_clock::now();
                if (!alive()) { stop = true; break; }
            }
            bool up; int port;
            { std::lock_guard<std::mutex> lk(mu); up = st.up; port = st.tcpPort ? st.tcpPort : this->port; }
            if (!up) {
                if (s != kBadSock) { sockClose(s); s = kBadSock; al.reset(); }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }
            if (s == kBadSock) {
                std::string err;
                s = tcpConnect(host, port, 3000, err);
                if (s == kBadSock) { std::lock_guard<std::mutex> lk(mu); st.message = err; std::this_thread::sleep_for(std::chrono::seconds(1)); continue; }
                sockRecvTimeout(s, 1000);
            }
            const long long n = sockRecv(s, buf.data(), buf.size());
            if (n > 0) { bytes += (uint64_t)n; al.feed((const uint8_t*)buf.data(), (size_t)n); }
            else if (n == 0) { sockClose(s); s = kBadSock; al.reset(); }
            // n < 0: the receive timeout (nothing for a second); the engine sends null packets while quiet, so this is rare
        }
        if (s != kBadSock) sockClose(s);
    }
};

Engine::Engine() : p_(new Impl) {}
Engine::~Engine() { stop(); }

Status Engine::status() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->st; }

void Engine::start(const std::string& command, const std::string& host, int tcpPort, Sink sink, std::function<bool()> alive) {
    stop();
    Impl& I = *p_;
    I.command = command; I.host = host; I.port = tcpPort; I.sink = std::move(sink); I.alive = std::move(alive);
    I.stop = false; I.exited = false;
    { std::lock_guard<std::mutex> lk(I.mu); I.st = Status(); I.st.state = "starting"; I.st.tcpPort = tcpPort; }
    std::string err;
    if (!I.spawn(err)) { std::lock_guard<std::mutex> lk(I.mu); I.st.state = "error"; I.st.message = err; return; }
    run_ = true;
    tsBytes_ = 0;
    I.reader = std::thread([this] { p_->readerLoop(); });
    I.tsThread = std::thread([this] { p_->tsLoop(tsBytes_); });
}

void Engine::tune(int rf) {
    if (!run_) return;
    { std::lock_guard<std::mutex> lk(p_->mu); p_->st.rf = rf; p_->st.state = "tuning"; p_->st.up = false; p_->st.message.clear(); }
    p_->write("{\"cmd\":\"tune\",\"rf\":" + std::to_string(rf) + "}\n");
}

void Engine::stop() {
    Impl& I = *p_;
    if (!run_) return;
    I.write("{\"cmd\":\"quit\"}\n");
    // the engine stops its receiver gracefully (it must close the radio itself); give it time, then kill what is left
    for (int i = 0; i < 900 && I.processAlive(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    I.stop = true;
    if (I.processAlive()) I.kill();
    I.closeHandles();
    if (I.reader.joinable()) I.reader.join();
    if (I.tsThread.joinable()) I.tsThread.join();
    run_ = false;
    std::lock_guard<std::mutex> lk(I.mu);
    I.st.up = false; I.st.state = "stopped";
}

}  // namespace dect2::extengine
