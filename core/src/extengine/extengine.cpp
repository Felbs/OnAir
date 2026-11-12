// External engine (dect2/extengine/extengine.h): the process, its two pipes, the status reader and the TS reader.
// The engine owns the radio and the decoding; this file only carries bytes and words between it and OnAir.
#include "dect2/extengine/extengine.h"
#include "dect2/common/thread_name.h"
#include "dect2/mcp/mcp.h"
#include "dect2/tv/iptv.h"
#include "../net/netcompat.h"
#include "../net/tcp_client.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
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
// a counter the engine sent: its number, or -1 when it said null (it does not know)
void counter(const mcp::Json& l, const char* key, long long& v) {
    if (const mcp::Json* f = l.get(key)) v = f->isNum() ? (long long)f->num : -1;
}
}  // namespace

void clearTruth(Status& s) {
    s.servoState.clear(); s.setting.clear();
    s.merDb = -1; s.video = false; s.picturesPerS = 0;
    s.packets = s.ccErrors = s.nulls = s.tei = s.frames = s.sinkBytes = -1;
    s.fecPct = -1;
}

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
        clearTruth(s);   // a new channel: nothing of the last one (an ATSC 1.0 status sends no frames or FEC, an ATSC 3.0 one no MER)
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
        clearTruth(s);
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
                counter(*l, "packets", s.packets);
                counter(*l, "cc_errors", s.ccErrors);
                counter(*l, "nulls", s.nulls);
                counter(*l, "tei", s.tei);
                counter(*l, "frames", s.frames);
                if (const mcp::Json* f = l->get("fec_pct")) s.fecPct = f->isNum() ? f->num : -1;
                counter(*l, "sink_bytes", s.sinkBytes);
            }
        }
        return true;
    }
    return true;   // some other event: known JSON, nothing to fold
}

// ---- the process ----
// Who ends it: stop() (the interface), the status reader when the engine exits by itself, the TS reader when the screen was left.
// Each of them only calls beginClose(); the waiting (the engine closes its radio in its own time) is done by a thread of its own,
// so that none of the three blocks and none of them joins itself.
struct Engine::Impl {
    static constexpr long kNothingYet = -2;   // readSome(): no bytes within its short wait
    std::string command, host;
    int port = 0;
    Sink sink;
    std::function<bool()> alive;
    mutable std::mutex mu;
    Status st;
    std::thread reader, tsThread, closer;
    std::mutex cmu;                        // guards `closer`
    std::atomic<bool> stop{false};         // the two reader threads end
    std::atomic<bool> spawned{false};      // a process and its threads exist (until the closer has taken them down)
    std::atomic<bool> closing{false};      // the closer is at work
    std::atomic<bool>* run = nullptr;      // Engine::run_
    std::mutex wmu;
#ifdef _WIN32
    HANDLE proc = nullptr, hin = nullptr, hout = nullptr;
#else
    pid_t pid = -1;                        // also its process group: the engine's own receivers are stopped with it
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
        signal(SIGPIPE, SIG_IGN);   // a write to an engine that has gone must fail (EPIPE), not end OnAir (as plugin/proc.cpp does for its helper)
        int in[2], out[2];
        if (pipe(in) != 0) { err = "cannot create the pipes"; return false; }
        if (pipe(out) != 0) { close(in[0]); close(in[1]); err = "cannot create the pipes"; return false; }
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, in[0], 0);
        posix_spawn_file_actions_adddup2(&fa, out[1], 1);
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
        for (int fd : {in[0], in[1], out[0], out[1]}) posix_spawn_file_actions_addclose(&fa, fd);
        posix_spawnattr_t at;
        posix_spawnattr_init(&at);
        posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);   // a process group of its own: what the engine starts can be stopped with it
        posix_spawnattr_setpgroup(&at, 0);
        std::vector<char*> av = {(char*)"/bin/sh", (char*)"-c", (char*)command.c_str(), nullptr};
        const int r = posix_spawn(&pid, "/bin/sh", &fa, &at, av.data(), environ);
        posix_spawnattr_destroy(&at);
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
        size_t done = 0;
        while (done < s.size()) {
            const ssize_t w = ::write(fin, s.data() + done, s.size() - done);
            if (w < 0) { if (errno == EINTR) continue; return false; }   // EPIPE: the engine is gone
            done += (size_t)w;
        }
        return true;
#endif
    }

    // Bytes of the engine's stdout: their count, 0 at its end, kNothingYet when none came within a short wait. It never blocks for
    // long, so the reader thread can always be stopped, whoever still holds the other end of the pipe.
    long readSome(char* b, size_t n) {
#ifdef _WIN32
        DWORD avail = 0;
        if (!hout || !PeekNamedPipe(hout, nullptr, 0, nullptr, &avail, nullptr)) return 0;
        if (!avail) { Sleep(50); return kNothingYet; }
        DWORD got = 0;
        if (!ReadFile(hout, b, (DWORD)std::min<size_t>(n, avail), &got, nullptr)) return 0;
        return got ? (long)got : kNothingYet;
#else
        struct pollfd p = {fout, POLLIN, 0};
        const int r = ::poll(&p, 1, 200);
        if (r == 0) return kNothingYet;
        if (r < 0) return errno == EINTR ? kNothingYet : 0;
        const ssize_t got = ::read(fout, b, n);
        if (got < 0) return errno == EINTR ? kNothingYet : 0;
        return (long)got;
#endif
    }

    bool processAlive() {
#ifdef _WIN32
        return proc && WaitForSingleObject(proc, 0) == WAIT_TIMEOUT;
#else
        if (pid <= 0) return false;
        siginfo_t si{};
        // WNOWAIT: it stays unreaped, so its pid (the group's id) cannot be given to another process before endProcess() is through
        return waitid(P_PID, (id_t)pid, &si, WEXITED | WNOHANG | WNOWAIT) == 0 && si.si_pid == 0;
#endif
    }

    // After the engine had its time to exit: stop it if it did not, and whatever it started and left behind (a receiver that still
    // holds the radio). Asked first (SIGTERM, five seconds), then killed.
    void endProcess() {
#ifdef _WIN32
        if (proc && WaitForSingleObject(proc, 0) == WAIT_TIMEOUT) TerminateProcess(proc, 1);
#else
        if (pid <= 0) return;
        const auto wait5 = [](const std::function<bool()>& gone) { for (int i = 0; i < 50 && !gone(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(100)); };
        if (processAlive()) {
            ::kill(-pid, SIGTERM);
            wait5([this] { return !processAlive(); });
            if (processAlive()) ::kill(-pid, SIGKILL);
        }
        int s = 0;
        waitpid(pid, &s, 0);
        if (::kill(-pid, SIGTERM) == 0) {   // members of its group are left
            wait5([this] { return ::kill(-pid, 0) != 0; });
            ::kill(-pid, SIGKILL);
        }
        pid = -1;
#endif
    }

    void closeHandles() {
#ifdef _WIN32
        if (hin) CloseHandle(hin); if (hout) CloseHandle(hout); if (proc) CloseHandle(proc);
        hin = hout = proc = nullptr;
#else
        std::lock_guard<std::mutex> lk(wmu);
        if (fin >= 0) close(fin); if (fout >= 0) close(fout);
        fin = fout = -1;
#endif
    }

    // Any thread: the engine is asked to quit and the closer takes over. Returns at once.
    void beginClose() {
        if (!spawned || closing.exchange(true)) return;
        if (run) *run = false;
        { std::lock_guard<std::mutex> lk(mu); st.up = false; if (st.state != "error") st.state = "stopping"; }
        write("{\"cmd\":\"quit\"}\n");
        std::lock_guard<std::mutex> lk(cmu);
        if (closer.joinable()) closer.join();   // an earlier one, long finished
        closer = std::thread([this] { closeDown(); });
    }

    void closeDown() {
        setThreadName("onair-extengine-close");
        // the engine stops its receiver gracefully (it must close the radio itself); give it time, then stop what is left
        for (int i = 0; i < 900 && processAlive(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        endProcess();
        stop = true;
        if (reader.joinable()) reader.join();
        if (tsThread.joinable()) tsThread.join();
        closeHandles();   // after the reader: nothing reads a handle that is being closed
        { std::lock_guard<std::mutex> lk(mu); st.up = false; if (st.state != "error") st.state = "stopped"; }   // an error stays on the screen
        spawned = false;
        closing = false;
    }

    void readerLoop() {
        setThreadName("onair-extengine");
        std::string acc;
        char b[8192];
        while (!stop) {
            const long n = readSome(b, sizeof b);
            if (n == kNothingYet) continue;
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
        if (stop || closing) return;
        // the engine exited by itself
        { std::lock_guard<std::mutex> lk(mu); if (st.state != "error") st.message = "the engine exited"; st.state = "error"; st.up = false; }   // its own last error stays
        beginClose();
    }

    void tsLoop(std::atomic<uint64_t>& bytes) {
        setThreadName("onair-extengine-ts");
        iptv::TsAligner al([this](const uint8_t* p, size_t n) { if (sink) sink(p, n / 188); });
        sock_t s = kBadSock;
        std::vector<char> buf(1 << 16);
        auto lastAlive = std::chrono::steady_clock::now();
        while (!stop && !closing) {
            if (alive && std::chrono::steady_clock::now() - lastAlive > std::chrono::milliseconds(500)) {
                lastAlive = std::chrono::steady_clock::now();
                if (!alive()) { beginClose(); break; }   // the screen was left: the engine closes its receiver and gives the radio back
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
                if (s == kBadSock) { { std::lock_guard<std::mutex> lk(mu); st.message = err; } std::this_thread::sleep_for(std::chrono::seconds(1)); continue; }
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

Engine::Engine() : p_(new Impl) { p_->run = &run_; }
Engine::~Engine() { stop(); wait(); }

Status Engine::status() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->st; }
bool Engine::closing() const { return p_->closing; }

void Engine::start(const std::string& command, const std::string& host, int tcpPort, Sink sink, std::function<bool()> alive) {
    Impl& I = *p_;
    if (I.spawned || I.closing) return;
    { std::lock_guard<std::mutex> lk(I.cmu); if (I.closer.joinable()) I.closer.join(); }
    I.command = command; I.host = host; I.port = tcpPort; I.sink = std::move(sink); I.alive = std::move(alive);
    I.stop = false;
    { std::lock_guard<std::mutex> lk(I.mu); I.st = Status(); I.st.state = "starting"; I.st.tcpPort = tcpPort; }
    std::string err;
    if (!I.spawn(err)) { std::lock_guard<std::mutex> lk(I.mu); I.st.state = "error"; I.st.message = err; return; }
    I.spawned = true;
    run_ = true;
    tsBytes_ = 0;
    I.reader = std::thread([this] { p_->readerLoop(); });
    I.tsThread = std::thread([this] { p_->tsLoop(tsBytes_); });
}

void Engine::tune(int rf) {
    if (!run_) return;
    { std::lock_guard<std::mutex> lk(p_->mu); p_->st.rf = rf; p_->st.state = "tuning"; p_->st.up = false; p_->st.message.clear(); clearTruth(p_->st); }
    if (!p_->write("{\"cmd\":\"tune\",\"rf\":" + std::to_string(rf) + "}\n")) {
        std::lock_guard<std::mutex> lk(p_->mu);
        p_->st.state = "error"; p_->st.message = "the engine is not running";
    }
}

void Engine::stop() { p_->beginClose(); }

void Engine::wait() {
    std::lock_guard<std::mutex> lk(p_->cmu);
    if (p_->closer.joinable()) p_->closer.join();
}

}  // namespace dect2::extengine
