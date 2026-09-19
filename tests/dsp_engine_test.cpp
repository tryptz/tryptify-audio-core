// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for DspEngine's empty-graph contract: what the engine does to a
// signal before any plug-in is added. This is the path a new consumer (the
// StepMania port) hits first, so it is pinned here rather than discovered
// by ear.

#include <cmath>
#include <cstdio>
#include <vector>

#include "dsp_engine.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

constexpr int kRate = 48000;
constexpr int kBlock = 256;

// Runs [blocks] blocks of a constant value through the engine and returns the
// last sample of the last block, once the gain smoothers have settled.
float settle(DspEngine& e, float value, int blocks) {
    std::vector<float> l(kBlock), r(kBlock);
    for (int b = 0; b < blocks; ++b) {
        std::fill(l.begin(), l.end(), value);
        std::fill(r.begin(), r.end(), value);
        e.process(l.data(), r.data(), kBlock);
    }
    return l.back();
}

bool allFinite(const std::vector<float>& v) {
    for (float x : v) if (!std::isfinite(x)) return false;
    return true;
}

}  // namespace

int main() {
    {
        DspEngine e(kRate, kBlock);
        std::vector<float> l(kBlock, 0.0f), r(kBlock, 0.0f);
        bool silent = true;
        for (int b = 0; b < 200; ++b) {
            e.process(l.data(), r.data(), kBlock);
            for (int i = 0; i < kBlock; ++i)
                if (l[i] != 0.0f || r[i] != 0.0f) silent = false;
        }
        check(silent, "silence in gives exact silence out");
    }
    {
        DspEngine e(kRate, kBlock);
        std::vector<float> l(kBlock, 0.0f), r(kBlock, 0.0f);
        l[0] = r[0] = 1.0f;
        e.process(l.data(), r.data(), kBlock);
        check(allFinite(l) && allFinite(r), "impulse output is finite");
    }
    {
        DspEngine e(kRate, kBlock);
        float out = settle(e, 0.5f, 400);  // ~2 s, smoothers fully settled
        float gainDb = 20.0f * std::log10(out / 0.5f);
        std::printf("info  empty-graph steady-state gain: %.2f dB\n", gainDb);
        check(std::isfinite(gainDb), "empty-graph gain is measurable");
    }
    {
        DspEngine e(kRate, kBlock);
        settle(e, 0.25f, 50);
        e.reconfigure(44100, kBlock);
        std::vector<float> l(kBlock, 0.25f), r(kBlock, 0.25f);
        e.process(l.data(), r.data(), kBlock);
        check(allFinite(l) && allFinite(r), "reconfigure mid-stream stays finite");
    }

    std::printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
