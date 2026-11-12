// External engine screen: a receiver that runs in another program (the first is tvTuna: ATSC 1.0 and 3.0 on their own receivers
// behind a gain servo judged on decoder truth and a site radio warden).  OnAir starts the engine, tunes it, shows what it reports
// (the held gain setting, the equaliser MER against the data cliff, pictures per second, frames and FEC, who holds the radio) and
// plays its transport stream with the same player and controls as the antenna TV.  Modelled on the Internet TV screen (iptv_ui.cpp).
#include "app.h"
#include "dect2/common/u8file.h"
#include "dect2/extengine/extengine.h"
#include "dect2/tv/ts.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

using namespace dect2;

namespace {

struct State {
    bool loaded = false;
    char cmdBuf[1024] = "python -m tvtuna engine --tcp 8555";
    char hostBuf[128] = "127.0.0.1";
    int port = 8555;
    int rf = 0;
    extengine::Engine eng;
    std::mutex pmu;
    TsDemux demux;
    TsSnapshot snap;
    double snapT = 0;
    bool selected = false;
    double pmtT = 0;              // when the first PMT of this stream was seen (0: none yet)
    bool havePlaying = false;
    std::atomic<double> lastTick{0};
    std::string msg;
    // histories for the meters
    double lastMer = -1;
};
State S;

std::string dir() {
    std::error_code ec;
    const std::string d = plat::dataDir() + "/extengine";
    std::filesystem::create_directories(u8file(d), ec);
    return d;
}
std::string readAll(const std::string& p) { std::ifstream f(u8file(p), std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
void writeAll(const std::string& p, const std::string& d) { std::ofstream f(u8file(p), std::ios::binary); f << d; }

void saveState() {
    std::string o = std::string("cmd\t") + S.cmdBuf + "\nhost\t" + S.hostBuf + "\nport\t" + std::to_string(S.port) + "\nrf\t" + std::to_string(S.rf) + "\n";
    writeAll(dir() + "/state.tsv", o);
}
void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    std::istringstream in(readAll(dir() + "/state.tsv"));
    std::string line;
    while (std::getline(in, line)) {
        const size_t t = line.find('\t'); if (t == std::string::npos) continue;
        const std::string k = line.substr(0, t), v = line.substr(t + 1);
        if (k == "cmd") snprintf(S.cmdBuf, sizeof S.cmdBuf, "%s", v.c_str());
        else if (k == "host") snprintf(S.hostBuf, sizeof S.hostBuf, "%s", v.c_str());
        else if (k == "port") S.port = atoi(v.c_str());
        else if (k == "rf") S.rf = atoi(v.c_str());
    }
}

// ---- the player side (the same shape as Internet TV) ----
void sink(const uint8_t* p, size_t n, App* a) {
    std::lock_guard<std::mutex> lk(S.pmu);
    // nothing for the player after Stop (the engine closes in the background), nor once the screen is no longer drawn: the player
    // may already belong to another screen
    if (!S.havePlaying || glfwGetTime() - S.lastTick.load() > 0.5) return;
    for (size_t i = 0; i < n; i++) S.demux.feed(p + i * 188);
    a->engine.feedPlaybackSubtitles(p, n);
    const double t = glfwGetTime();
    bool haveSvc = false;
    for (auto& sv : S.snap.services) if (sv.havePmt) haveSvc = true;
    if (!haveSvc || !S.selected || t - S.snapT > 1.0) { S.snap = S.demux.snapshot(); S.snapT = t; }
    Player& pl = a->engine.player();
    if (!S.selected) {
        // the main programme, not whichever PMT arrived first (found live: one run opened the HD programme, the next a subchannel):
        // the lowest programme number, once every programme of the PAT has its PMT, or a second after the first PMT
        int pick = -1; bool all = !S.snap.services.empty();
        for (auto& sv : S.snap.services) { if (!sv.havePmt) { all = false; continue; } if (pick < 0 || sv.id < pick) pick = sv.id; }
        if (pick >= 0 && S.pmtT == 0) S.pmtT = t;
        if (pick >= 0 && (all || t - S.pmtT > 1.0)) { pl.select(pick); pl.setVolume(a->volume); S.selected = true; }
    }
    if (pl.selected() >= 0) pl.push(p, n, S.snap);
}

void stopPlaying(App& a) {
    S.eng.stop();
    std::lock_guard<std::mutex> lk(S.pmu);
    a.engine.player().select(-1);
    a.engine.setPlayerExternal(false);
    a.engine.teletext().reset();
    S.selected = false; S.pmtT = 0;
    S.havePlaying = false;
}

void play(App& a) {
    if (S.rf <= 0) { S.msg = "Give the RF channel number the engine should tune."; return; }
    if (S.eng.closing()) { S.msg = "The engine is still closing its receiver. Try again in a moment."; return; }
    { std::lock_guard<std::mutex> lk(S.pmu); S.demux.reset(); S.snap = TsSnapshot(); S.snapT = 0; S.selected = false; S.pmtT = 0; S.havePlaying = true; }
    a.engine.player().select(-1);
    a.engine.setPlayerExternal(true);
    a.engine.teletext().reset();
    App* ap = &a;
    auto alive = [ap] {
        if (glfwGetTime() - S.lastTick.load() < 2.0) return true;
        ap->engine.player().select(-1);   // the screen was left: give the player back to the antenna
        ap->engine.setPlayerExternal(false);
        return false;
    };
    S.msg.clear();
    saveState();
    if (!S.eng.running()) S.eng.start(S.cmdBuf, S.hostBuf, S.port, [ap](const uint8_t* p, size_t n) { sink(p, n, ap); }, alive);
    S.eng.tune(S.rf);
}

// one line above the picture: which channel, and whether pictures are coming
void drawState(App& a, const extengine::Status& st) {
    ImGui::AlignTextToFramePadding();
    const std::string title = st.rf > 0 ? "RF " + std::to_string(st.rf) + (st.standard == "atsc1" ? "  ATSC 1.0" : st.standard == "atsc3" ? "  ATSC 3.0" : "") : std::string("External engine");
    ImGui::TextColored(pal::heading(), "%s", title.c_str());
    const bool good = st.state == "up" && (st.video || st.fecPct > 50);
    ImVec4 col = good ? ImVec4(0.45f, 0.85f, 0.5f, 1) : st.state == "error" ? ImVec4(0.95f, 0.45f, 0.4f, 1) : ImVec4(0.95f, 0.8f, 0.35f, 1);
    const char* label = st.state == "up" ? (good ? "decoding" : "tuned, waiting for pictures") : st.state == "tuning" ? "tuning (the engine searches the gain on decoder truth)"
                      : st.state == "starting" ? "starting the engine" : st.state == "error" ? "cannot play" : st.state == "idle" ? "engine idle"
                      : st.state == "stopping" ? "the engine is closing its receiver" : "stopped";
    ImGui::SameLine();
    ImGui::TextColored(col, "%s", label);
    if (!st.message.empty()) { ImGui::SameLine(); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(st.message.c_str()); ImGui::PopTextWrapPos(); }
    if (st.state == "error") { ImGui::SameLine(); if (ImGui::Button("Try again##extretry")) play(a); }
}

// The row under the tabs, where the TV modes have their constellations: the decoder truth, what the engine measured and not what a
// meter felt. It lives here so that the tab above keeps its height for the picture.
void panels(App& a) {
    if (!S.havePlaying) { ImGui::TextDisabled("%s", tr("The engine's measurements appear here once a channel is tuned.")); return; }
    const extengine::Status st = S.eng.status();
    const PlayerStats ps = a.engine.player().stats();
    ImGui::TextColored(pal::heading(), "%s", tr("Engine measurements"));
    std::string line;
    if (!st.servoState.empty()) line += "servo " + st.servoState;
    if (!st.setting.empty()) line += (line.empty() ? "" : "  |  ") + st.setting;
    if (st.merDb >= 0) { char b[64]; snprintf(b, sizeof b, "  |  MER %.1f dB (cliff 15.2)", st.merDb); line += b; }
    if (st.standard == "atsc1") { char b[64]; snprintf(b, sizeof b, "  |  %s %.1f pictures/s", st.video ? "video" : "NO video", st.picturesPerS); line += b; }
    if (st.frames >= 0) { char b[64]; snprintf(b, sizeof b, "  |  frames %lld", st.frames); line += b; }
    if (st.fecPct >= 0) { char b[64]; snprintf(b, sizeof b, "  |  FEC %.0f %%%s", st.fecPct, st.fecPct < 20 ? " (fade: the air, not the code)" : ""); line += b; }
    if (!line.empty()) ImGui::TextUnformatted(line.c_str());
    std::string l2;
    if (st.packets >= 0) l2 += std::to_string(st.packets) + " packets";
    if (st.ccErrors >= 0) l2 += (l2.empty() ? "" : "  |  ") + std::to_string(st.ccErrors) + " continuity errors";
    if (st.tei >= 0) l2 += "  |  " + std::to_string(st.tei) + " TEI";
    if (ps.width > 0) l2 += (l2.empty() ? "" : "  |  ") + std::to_string(ps.width) + "x" + std::to_string(ps.height) + (ps.fps > 0 ? " " + std::to_string((int)(ps.fps + 0.5)) + " fps" : "");
    if (!ps.videoCodec.empty()) l2 += "  |  " + ps.videoCodec + (ps.audioCodec.empty() ? "" : " + " + ps.audioCodec);
    if (!l2.empty()) ImGui::TextUnformatted(l2.c_str());
    ImGui::Text("radio: %s", st.radioOwner.empty() ? "free" : (st.radioOwner + (st.radioPurpose.empty() ? "" : " (" + st.radioPurpose + ")")).c_str());
}

void tab(App& a) {
    loadState();
    const extengine::Status st = S.eng.status();
    // one row of controls, then the picture: the tab's height belongs to the picture (found on the first screenshots: nine rows of
    // settings and status above it left the picture 60 pixels)
    ImGui::SetNextItemWidth(70 * gUi);
    ImGui::InputInt("RF channel##extrf", &S.rf, 0, 0);
    ImGui::SameLine();
    if (ImGui::Button(S.havePlaying ? "Tune##exttune" : "Start and tune##exttune")) play(a);
    if (S.havePlaying) { ImGui::SameLine(); if (ImGui::Button("Stop##extstop")) stopPlaying(a); }
    if (!st.channels.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", tr("engine's table:"));
        for (size_t i = 0; i < st.channels.size(); i++) {
            ImGui::SameLine();
            char b[48]; snprintf(b, sizeof b, "%d##extch%zu", st.channels[i], i);
            if (ImGui::SmallButton(b)) { S.rf = st.channels[i]; play(a); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", st.channelLabels[i].c_str());
        }
    }
    if (!S.msg.empty()) { ImGui::SameLine(); ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.35f, 1), "%s", S.msg.c_str()); }
    // the engine's own settings: open until a channel plays, folded away while one does
    static int shownFor = -1;
    if (shownFor != (int)S.havePlaying) { ImGui::SetNextItemOpen(!S.havePlaying); shownFor = (int)S.havePlaying; }
    if (ImGui::CollapsingHeader("Engine##exteng")) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", tr("A receiver in another program: OnAir starts it, tunes it, and plays its transport stream. The first engine is tvTuna (ATSC 1.0 and ATSC 3.0)."));
        ImGui::PopTextWrapPos();
        ImGui::SetNextItemWidth(std::min(600.f * gUi, ImGui::GetContentRegionAvail().x - 8));
        if (ImGui::InputText("Engine command##extcmd", S.cmdBuf, sizeof S.cmdBuf)) saveState();
        ImGui::SetNextItemWidth(160 * gUi);
        if (ImGui::InputText("TS host##exthost", S.hostBuf, sizeof S.hostBuf)) saveState();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90 * gUi);
        if (ImGui::InputInt("port##extport", &S.port, 0, 0)) saveState();
    }
    if (S.havePlaying) drawState(a, st);
    playerTab(a);
}

void status(App& a) {
    const extengine::Status st = S.eng.status();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("External engine"); ImGui::SameLine(0, 8 * gUi);
    ImGui::PushFont(a.mono, 0);
    std::string s = S.havePlaying ? st.state : "idle";
    if (S.havePlaying && st.merDb >= 0) { char b[32]; snprintf(b, sizeof b, "  MER %.1f dB", st.merDb); s += b; }
    if (S.havePlaying && st.fecPct >= 0) { char b[32]; snprintf(b, sizeof b, "  FEC %.0f%%", st.fecPct); s += b; }
    ImGui::TextUnformatted(s.c_str());
    ImGui::PopFont();
}

void summary(const App&, std::string& l1, std::string& l2) {
    const extengine::Status st = S.eng.status();
    l1 = "External engine";
    l2 = S.havePlaying && st.rf > 0 ? "RF " + std::to_string(st.rf) + (st.standard == "atsc1" ? " ATSC 1.0" : st.standard == "atsc3" ? " ATSC 3.0" : "") : "no channel";
}

void meters(const App&, std::vector<ModeMeter>& out) {
    const extengine::Status st = S.eng.status();
    if (st.merDb >= 0) out.push_back({"MER", "%.1f dB", st.merDb, 5, 30, st.merDb < 15.2 ? 3 : st.merDb < 18.2 ? 2 : 1});
    if (st.standard == "atsc1") out.push_back({"pictures/s", "%.1f", st.picturesPerS, 0, 6, st.video ? 1 : 3});
    if (st.fecPct >= 0) out.push_back({"FEC", "%.0f %%", st.fecPct, 0, 100, st.fecPct < 20 ? 3 : st.fecPct < 90 ? 2 : 1});
}

void tick(App& a) {
    loadState();
    {   // dev/test only: DECT2_EXTENGINE_TUNE=<rf> starts the engine on that channel once (hidden --shot runs), like DECT2_IPTV_PLAY
        static bool done = false;
        if (!done) { done = true; if (const char* rf = std::getenv("DECT2_EXTENGINE_TUNE")) { S.rf = atoi(rf); if (S.rf > 0) play(a); } }
    }
    S.lastTick = glfwGetTime();
    a.engine.player().setVolume(a.volume);
}

}  // namespace

void extengineShutdown() {
    S.eng.stop();
    S.eng.wait();   // the program's end: the engine must have closed its radio before OnAir is gone
}

extern const ModeUi kExtEngineUi;
const ModeUi kExtEngineUi = {
    .sideTitle = "CHANNELS",
    .tabName = "External engine",
    .tabIcon = Ic::Tv,
    .tab = tab,
    .panels = panels,
    .status = status,
    .summary = summary,
    .tick = tick,
    .meters = meters,
};
