// External engine as a mode row: no radio and no samples here (the engine process owns its radio), so the Engine only needs a
// tuning entry (engine standard 243, family 241) and a silent test signal, exactly like Internet TV (iptv_mode.cpp).
#include "dect2/extengine/extengine_mode.h"

namespace dect2 {
ModeTuning extengineTuning() {
    ModeTuning t;
    t.stdMode = 243; t.id = "extengine"; t.name = "External engine";
    t.minMhz = 1; t.maxMhz = 6000; t.defMhz = 100;
    t.sampleRate = 2e6;
    t.bandwidthMhz = 1;
    t.minSampleRate = 1e5;
    return t;
}
namespace {
class SilentSynth : public ModeSynth {
public:
    explicit SilentSynth(double r) : r_(r) {}
    double sampleRate() const override { return r_; }
    void generate(cf32* out, size_t n) override { for (size_t i = 0; i < n; i++) out[i] = cf32(0.f, 0.f); }
private:
    double r_;
};
}
std::unique_ptr<ModeSynth> makeExtengineSynth(const SynthConfig&, double sampleRate) { return std::make_unique<SilentSynth>(sampleRate); }
}
