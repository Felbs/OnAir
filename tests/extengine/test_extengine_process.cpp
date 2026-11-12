// External engine: the process's life.  An engine that exits by itself, a write to an engine that is gone, a stop that must not wait
// for what the engine left behind, and a second start after each of them.  The "engines" here are small shell commands.
#include "dect2/extengine/extengine.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#ifndef _WIN32
#include <csignal>
#include <fstream>
#include <unistd.h>
#endif

static int fails = 0;
#define CHECK(c, fmt, ...) do { if (!(c)) { std::printf("FAIL line %d: " fmt "\n", __LINE__ __VA_OPT__(,) __VA_ARGS__); fails++; } } while (0)   // Flawfinder: ignore (the format is a literal)

#ifdef _WIN32
int main() { std::printf("test_extengine_process: the shell engines of this test are POSIX only, skipped\n"); return 0; }
#else
using dect2::extengine::Engine;
using Clock = std::chrono::steady_clock;

static double since(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }
template <class F> static bool within(double seconds, F f) {
    const auto t0 = Clock::now();
    while (since(t0) < seconds) { if (f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    return f();
}
static const char* kHello = R"(echo '{"ev":"hello","tcp":1,"channels":{}}')";

int main() {
    const auto sink = [](const uint8_t*, size_t) {};

    {   // 1. the engine exits by itself: running() turns false, the state says so, and a second start works
        Engine e;
        e.start("exit 3", "127.0.0.1", 1, sink);
        CHECK(within(5, [&] { return !e.running() && !e.closing(); }), "an engine that exited must stop counting as running");
        CHECK(e.status().state == "error", "state after the engine exited: %s", e.status().state.c_str());
        e.tune(34);   // must neither crash nor end this program (SIGPIPE)
        e.start(std::string(kHello) + "; read line; exit 0", "127.0.0.1", 1, sink);
        CHECK(e.running(), "the second start after an exit must run");
        CHECK(within(5, [&] { return e.status().state == "idle"; }), "the second engine's hello was not read: %s", e.status().state.c_str());
        // 2. a write to an engine that is gone, before the reader has noticed: no SIGPIPE
        e.stop();
        e.wait();
        CHECK(!e.running() && e.status().state == "stopped", "after stop and wait: running %d state %s", (int)e.running(), e.status().state.c_str());
    }

    {   // 3. the engine obeys quit but leaves a child behind that holds its stdout (a receiver that did not stop): stop() returns
        //    at once, wait() comes back although the pipe never reaches its end, and the child is stopped with the engine's group
        const std::string pidFile = std::string(P_tmpdir) + "/onair_test_extengine_child.pid";
        std::remove(pidFile.c_str());
        Engine e;
        e.start("sleep 300 & echo $! > " + pidFile + "; " + kHello + "; read line; exit 0", "127.0.0.1", 1, sink);
        CHECK(within(5, [&] { return e.status().state == "idle"; }), "hello not read: %s", e.status().state.c_str());
        long child = 0;
        within(5, [&] { std::ifstream f(pidFile); return (bool)(f >> child) && child > 0; });
        CHECK(child > 0 && ::kill((pid_t)child, 0) == 0, "the left-behind child should be running (pid %ld)", child);
        auto t0 = Clock::now();
        e.stop();
        CHECK(since(t0) < 0.5, "stop() must return at once, took %.2f s", since(t0));
        CHECK(e.closing() || e.status().state == "stopped", "after stop() the engine is closing");
        e.start(kHello, "127.0.0.1", 1, sink);   // refused while the old one closes
        t0 = Clock::now();
        e.wait();
        CHECK(since(t0) < 15, "wait() hung on the child's pipe: %.1f s", since(t0));
        CHECK(e.status().state == "stopped", "state after wait: %s", e.status().state.c_str());
        CHECK(child > 0 && within(3, [&] { return ::kill((pid_t)child, 0) != 0; }), "the child the engine left behind still runs");
        std::remove(pidFile.c_str());
    }

    {   // 4. the screen was left (alive() false): the engine is asked to quit and running() turns false, so the next play starts it again
        Engine e;
        std::atomic<bool> here{true};
        e.start(std::string(kHello) + "; read line; exit 0", "127.0.0.1", 1, sink, [&] { return here.load(); });
        CHECK(within(5, [&] { return e.status().state == "idle"; }), "hello not read");
        here = false;
        CHECK(within(5, [&] { return !e.running() && !e.closing(); }), "leaving the screen must end the engine");
        CHECK(e.status().state == "stopped", "state after the screen was left: %s", e.status().state.c_str());
        here = true;
        e.start(std::string(kHello) + "; read line; exit 0", "127.0.0.1", 1, sink, [&] { return here.load(); });
        CHECK(e.running(), "the engine must start again after the screen was left");
    }   // the destructor stops the last one

    std::printf("%s: %d failure(s)\n", fails ? "test_extengine_process FAILED" : "test_extengine_process OK", fails);
    return fails ? 1 : 0;
}
#endif
