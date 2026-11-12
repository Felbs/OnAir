// External engine: a receiver that lives in another program.  OnAir starts it, tells it the channel over a line protocol on its stdin,
// reads its status as JSON lines on its stdout, and takes the picture as MPEG-TS from a local TCP socket into the Player.  The first
// engine is tvTuna (the Software TV Tuner's 8-VSB chain and the atsc3 receiver behind gr-rxtune's gain servo and a site radio warden);
// any program that speaks the same lines works.  The protocol, one JSON object per line:
//   to the engine    {"cmd":"tune","rf":N}  {"cmd":"status"}  {"cmd":"stop"}  {"cmd":"quit"}
//   from the engine  {"ev":"hello","tcp":PORT,"channels":{...}}  {"ev":"up","rf":N,"standard":"atsc1"|"atsc3","tcp":PORT}
//                    {"ev":"status","rf":N,"up":true,"radio":{"owner":...},"last":{...decoder truth...}}  {"ev":"error","msg":...}  {"ev":"released","rf":N}
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2::extengine {

// What the engine last said, flattened for the panels.  Fields it did not send keep their "unknown" value.
struct Status {
    std::string state = "stopped";   // stopped, starting, tuning, up, error, stopping (the engine is closing its receiver)
    std::string message;             // the engine's own words (an error, a note)
    int rf = 0;
    std::string standard;            // "atsc1", "atsc3", or empty
    int tcpPort = 0;
    bool up = false;
    std::string radioOwner;          // who holds the site radio warden ("" = free or unknown)
    std::string radioPurpose;
    // decoder truth, from the engine's "last" object
    std::string servoState;          // searching / holding / probing (ATSC 1.0 gain servo)
    std::string setting;             // "RFGR 4  IFGR 39"
    double merDb = -1;               // ATSC 1.0 equaliser MER; -1 unknown
    bool video = false;              // pictures arriving
    double picturesPerS = 0;
    long long packets = -1, ccErrors = -1, nulls = -1, tei = -1;
    long long frames = -1;           // ATSC 3.0 chain frames; -1 unknown
    double fecPct = -1;              // ATSC 3.0 interval FEC; -1 unknown
    long long sinkBytes = -1;
    double t = 0;                    // engine time of the line (seconds since 1970)
    std::vector<int> channels;       // from "hello": the engine's channel table (RF numbers)
    std::vector<std::string> channelLabels;   // same order: "RF 34  ATSC 1.0  18 dB"
};

// One JSON line from the engine folded into `s`.  Returns false when the line was not JSON (it is ignored).  Public for the tests.
bool applyLine(const std::string& line, Status& s);
// The decoder truth back to "unknown": a new channel shows nothing of the last one.
void clearTruth(Status& s);

// Starts the engine process, tunes, streams.  All methods but wait() may be called from the interface thread and return at once; the
// work runs on threads of its own (the status reader on the process's stdout, the TS reader on the socket, and at the end one that
// waits for the engine to close its receiver).
class Engine {
public:
    using Sink = std::function<void(const uint8_t* pkts, size_t nPackets)>;   // 188-byte aligned transport-stream packets
    Engine();
    ~Engine();
    // command: the engine's command line (program and arguments, as a shell would get it); host/port: where its TS socket listens.
    // alive: polled by the TS thread about twice a second; false stops everything (the screen was left).  May be empty.
    // Does nothing while an engine runs or closing() is true (the radio is not free yet).
    void start(const std::string& command, const std::string& host, int tcpPort, Sink sink, std::function<bool()> alive = {});
    void tune(int rf);                // {"cmd":"tune"} to the running engine
    // {"cmd":"quit"} and returns.  In the background the engine gets up to 90 s to close its receiver and exit; then it, and whatever
    // it started and left behind, is stopped.
    void stop();
    void wait();                      // blocks until a stop has finished (the program's end)
    bool running() const { return run_; }   // false as soon as the engine exited, the screen was left or stop() was called
    bool closing() const;             // the engine of before is still closing its receiver
    Status status() const;
    uint64_t tsBytes() const { return tsBytes_; }
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
    std::atomic<bool> run_{false};
    std::atomic<uint64_t> tsBytes_{0};
};

}  // namespace dect2::extengine
