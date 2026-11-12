// TS recording to a file when the receiver runs ahead of the clock: the paced queue is still behind when the recording is closed, and a
// few packets are still held back (less than the minimum span). Every packet must land in the file once, in the order it came.
#include "dect2/tv/tsout.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace dect2;

int main() {
    const std::string path = std::string(P_tmpdir) + "/onair_test_tsout_file.ts";
    OutputManager out;
    OutputConfig c;
    c.file = true;
    c.path = path;
    out.configure(c);
    if (!out.stats().fileOpen) { printf("FAIL: cannot open %s\n", path.c_str()); return 1; }

    uint32_t seq = 0;
    auto send = [&](int n) {
        for (int i = 0; i < n; i++, seq++) {
            uint8_t pkt[188];
            memset(pkt, 0, sizeof pkt);
            pkt[0] = 0x47; pkt[1] = 0x01; pkt[2] = 0x00; pkt[3] = 0x10 | (seq & 15);
            memcpy(pkt + 4, &seq, 4);
            out.packet(pkt, nullptr);
        }
    };
    // ten seconds of signal handed over at once: the file is paced to the signal's time, so most of it is still queued at close
    for (int b = 0; b < 10; b++) { send(500); out.burstDone(1.0); }
    // then a little more than the minimum span would schedule: it stays held back
    send(37);
    out.burstDone(0.001);
    out.close();

    std::vector<uint8_t> d;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        uint8_t buf[65536];
        size_t k;
        while ((k = fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + k);
        fclose(f);
    }
    remove(path.c_str());
    int fails = 0;
    size_t n = d.size() / 188, wrong = 0;
    if (n != seq) { printf("FAIL: %zu packets in the file, sent %u\n", n, seq); fails++; }
    for (size_t i = 0; i < n; i++) {
        uint32_t v;
        memcpy(&v, &d[i * 188 + 4], 4);
        if (v != i && wrong++ == 0) { printf("FAIL: packet %zu holds %u (out of order)\n", i, v); fails++; }
    }
    printf("tsout file: %u packets sent, %zu in the file, %zu out of order: %s\n", seq, n, wrong, fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
