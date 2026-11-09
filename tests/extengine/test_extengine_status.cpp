// External engine: the status lines an engine sends fold into the Status the panels read.  Lines recorded from tvTuna on 2026-11-09.
#include "dect2/extengine/extengine.h"
#include <cmath>
#include <cstdio>
#include <string>

static int fails = 0;
#define CHECK(c, fmt, ...) do { if (!(c)) { std::printf("FAIL line %d: " fmt "\n", __LINE__ __VA_OPT__(,) __VA_ARGS__); fails++; } } while (0)   // Flawfinder: ignore (the format is a literal)

int main() {
    using dect2::extengine::Status;
    using dect2::extengine::applyLine;
    Status s;
    s.state = "starting";
    CHECK(!applyLine("not json at all", s), "a non-JSON line must be ignored, not applied");
    CHECK(applyLine(R"({"ev": "hello", "version": "0.1.0", "tcp": 8555, "channels": {"34": {"atsc1_hold": {"Antenna A": {"held": {"gain:RFGR": 4, "gain:IFGR": 39}}}}, "33": {"standard": "atsc3", "atsc3_gains": {}}}, "radio": null})", s), "hello");
    CHECK(s.tcpPort == 8555, "tcp %d", s.tcpPort);
    CHECK(s.channels.size() == 2, "channels %zu", s.channels.size());
    CHECK(s.channels.size() == 2 && s.channelLabels[0].find("ATSC 1.0") != std::string::npos, "RF 34 is ATSC 1.0 by its hold: '%s'", s.channelLabels.empty() ? "" : s.channelLabels[0].c_str());
    CHECK(s.channels.size() == 2 && s.channelLabels[1].find("ATSC 3.0") != std::string::npos, "RF 33 is ATSC 3.0: '%s'", s.channelLabels.size() < 2 ? "" : s.channelLabels[1].c_str());
    CHECK(s.state == "idle", "after hello the engine is idle, got %s", s.state.c_str());

    CHECK(applyLine(R"({"ev": "up", "rf": 34, "standard": "atsc1", "ts": "C:/x/live.ts", "tcp": 8555})", s), "up");
    CHECK(s.up && s.state == "up" && s.rf == 34 && s.standard == "atsc1", "up folded: up=%d state=%s rf=%d std=%s", (int)s.up, s.state.c_str(), s.rf, s.standard.c_str());

    CHECK(applyLine(R"({"ev": "status", "rf": 34, "standard": "atsc1", "up": true, "radio": {"owner": "tvtuna", "purpose": "live TV", "priority": 60}, "last": {"standard": "atsc1", "state": "holding", "setting": {"gain:RFGR": 4, "gain:IFGR": 39}, "mer_db": 18.29, "video": true, "pictures_per_s": 4.5, "packets": 302028, "tei": 0, "cc_errors": 2609, "nulls": 15012, "seq_headers": 103, "t": 1794253726.6}, "error": null, "t": 1794253726.7})", s), "status atsc1");
    CHECK(s.radioOwner == "tvtuna", "radio owner '%s'", s.radioOwner.c_str());
    CHECK(s.servoState == "holding", "servo '%s'", s.servoState.c_str());
    CHECK(s.setting == "RFGR 4  IFGR 39", "setting '%s'", s.setting.c_str());
    CHECK(std::fabs(s.merDb - 18.29) < 1e-6, "mer %.2f", s.merDb);
    CHECK(s.video && std::fabs(s.picturesPerS - 4.5) < 1e-9, "video %d %.1f/s", (int)s.video, s.picturesPerS);
    CHECK(s.packets == 302028 && s.ccErrors == 2609 && s.nulls == 15012 && s.tei == 0, "counters %lld %lld %lld %lld", s.packets, s.ccErrors, s.nulls, s.tei);
    CHECK(s.frames == -1 && s.fecPct < 0, "ATSC 1.0 status carries no frames / FEC");

    CHECK(applyLine(R"({"ev": "status", "rf": 33, "standard": "atsc3", "up": true, "radio": {"owner": "atsc3_watch", "purpose": "ATSC 3.0 live watch RF33"}, "last": {"standard": "atsc3", "t": 1794254963.0, "sink_bytes": 27616448, "frames": 359, "fec_pct": 100.0, "bytes": 31517826, "lanes": {"video_pid12": 27942355}}, "error": null, "t": 1794254963.1})", s), "status atsc3");
    CHECK(s.rf == 33 && s.standard == "atsc3", "rf %d std %s", s.rf, s.standard.c_str());
    CHECK(s.frames == 359 && std::fabs(s.fecPct - 100.0) < 1e-9, "frames %lld fec %.1f", s.frames, s.fecPct);
    CHECK(s.sinkBytes == 27616448, "sink bytes %lld", s.sinkBytes);
    CHECK(s.radioOwner == "atsc3_watch", "radio owner '%s'", s.radioOwner.c_str());

    CHECK(applyLine(R"({"ev": "status", "rf": 33, "up": true, "radio": null, "last": {"standard": "atsc3", "frames": 400, "fec_pct": null}})", s), "status with a null FEC");
    CHECK(s.fecPct < 0, "a null FEC reads unknown, got %.1f", s.fecPct);
    CHECK(s.radioOwner.empty(), "a null radio reads free, got '%s'", s.radioOwner.c_str());

    CHECK(applyLine(R"({"ev": "error", "rf": 36, "msg": "chain did not start"})", s), "error");
    CHECK(s.state == "error" && !s.up && s.message == "chain did not start", "error folded: %s / %s", s.state.c_str(), s.message.c_str());
    CHECK(applyLine(R"({"ev": "released", "rf": 36})", s), "released");
    CHECK(!s.up && s.state == "idle" && s.merDb < 0 && s.setting.empty(), "released clears the decoder truth");

    std::printf("%s: %d failure(s)\n", fails ? "test_extengine_status FAILED" : "test_extengine_status OK", fails);
    return fails ? 1 : 0;
}
