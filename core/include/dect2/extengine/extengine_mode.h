// External engine mode entry for the engine's tables: family 241, engine standard 243. See dect2/extengine/extengine.h and docs/EXTERNAL-ENGINE.md.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/engine/mode_synth.h"
#include <memory>
namespace dect2 {
ModeTuning extengineTuning();
std::unique_ptr<ModeSynth> makeExtengineSynth(const SynthConfig& cfg, double sampleRate);   // silence: the engine owns the radio
}
