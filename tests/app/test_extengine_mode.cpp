// External engine mode: the engine tuning is registered under its number, the mode row and the family count agree with it,
// the screen is mapped, and the panel's strings are in the screen source.
#include <cstdlib>
#include "dect2/extengine/extengine_mode.h"
#include "dect2/engine/mode_tuning.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static int fails = 0;
#define CHECK(c, fmt, ...) do { if (!(c)) { std::printf("FAIL line %d: " fmt "\n", __LINE__ __VA_OPT__(,) __VA_ARGS__); fails++; } } while (0)   // Flawfinder: ignore (the format is a literal)

static std::string readFile(const std::string& rel) {
    std::ifstream in(std::string(DECT2_SOURCE_DIR) + "/" + rel);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int main() {
    const dect2::ModeTuning t = dect2::extengineTuning();
    CHECK(t.stdMode == 243, "stdMode %d, want 243", t.stdMode);
    const dect2::ModeTuning* reg = dect2::modeTuning(243);
    CHECK(reg != nullptr, "modeTuning(243) not registered");
    if (reg) CHECK(std::string(reg->name) == "External engine", "registered name '%s'", reg->name);

    const std::string table = readFile("app/ui/mode_table.cpp");
    CHECK(table.find("{241, \"External engine\", ") != std::string::npos, "mode row for family 241 missing");
    const std::string appH = readFile("app/app.h");
    { const size_t q = appH.find("constexpr int kNumFamilies = "); CHECK(q != std::string::npos && atoi(appH.c_str() + q + 29) >= 242, "kNumFamilies does not cover family 241"); }
    const std::string modeUi = readFile("app/panels/modeui.cpp");
    CHECK(modeUi.find("case 241: return &kExtEngineUi;") != std::string::npos, "family 241 not mapped to kExtEngineUi");
    const std::string eng = readFile("core/src/engine/engine.cpp");
    CHECK(eng.find("a == 242") != std::string::npos, "the feed switch has no branch for active standard 242 (it would fall through to DVB-T2)");
    const std::string tv = readFile("app/panels/tv.cpp");
    CHECK(tv.find("a.family == 241") != std::string::npos, "the TV tab does not know family 241 draws its own header");

    const std::string ui = readFile("app/extengine/extengine_ui.cpp");
    const char* strings[] = {"Engine command", "RF channel", "Start and tune", "decoder truth", "MER", "pictures/s", "FEC", "radio:"};
    for (const char* s : strings)
        CHECK(ui.find(s) != std::string::npos, "panel string missing: %s", s);

    std::printf("%s: %d failure(s)\n", fails ? "test_extengine_mode FAILED" : "test_extengine_mode OK", fails);
    return fails ? 1 : 0;
}
