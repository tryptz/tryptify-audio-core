// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for the PEQ fitter: that its response model is the filter the
// engine actually runs, that it can find bands it is given back, and that
// what it returns is something a device with fixed limits would accept.

#include <cmath>
#include <cstdio>
#include <vector>

#include "biquad.h"
#include "peq_fitter.h"

using namespace tryptify::fit;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

constexpr double kRate = 48000.0;

// Level of a sine after the float Biquad, in dB, once the filter has settled.
double measuredDb(BiquadType type, double f0, double q, double gain, double freq) {
    Biquad bq;
    bq.configure(type, kRate, f0, q, gain);
    const int settle = 48000, measure = 48000;
    double in2 = 0.0, out2 = 0.0;
    for (int i = 0; i < settle + measure; i++) {
        float x = static_cast<float>(std::sin(2.0 * M_PI * freq * i / kRate));
        float y = bq.process(x);
        if (i >= settle) {
            in2 += double(x) * x;
            out2 += double(y) * y;
        }
    }
    return 10.0 * std::log10(out2 / in2);
}

bool isMultiple(double v, double step) {
    double k = v / step;
    return std::fabs(k - std::round(k)) < 1e-6;
}

void printBands(const PeqFit& fit) {
    for (const PeqBand& b : fit.bands)
        std::printf("        type %d  %8.1f Hz  %+6.2f dB  Q %.2f\n",
                    static_cast<int>(b.type), b.freq, b.gainDb, b.q);
}

}  // namespace

int main() {
    {
        // The fitter's whole job rests on this: if the model differs from
        // the running filter, it optimises the wrong thing.
        double worst = 0.0;
        for (double f : {100.0, 700.0, 1000.0, 1500.0, 8000.0}) {
            double model = Biquad::design(BiquadType::Peaking, kRate, 1000.0, 2.0, 6.0).magnitudeDb(f, kRate);
            worst = std::max(worst, std::fabs(model - measuredDb(BiquadType::Peaking, 1000.0, 2.0, 6.0, f)));
        }
        for (double f : {50.0, 105.0, 400.0}) {
            double model = Biquad::design(BiquadType::LowShelf, kRate, 105.0, 0.7, -4.0).magnitudeDb(f, kRate);
            worst = std::max(worst, std::fabs(model - measuredDb(BiquadType::LowShelf, 105.0, 0.7, -4.0, f)));
        }
        std::printf("      model vs running filter: worst %.4f dB\n", worst);
        check(worst < 0.01, "magnitudeDb matches the filter configure() builds");
    }
    {
        std::vector<PeqBand> known = {
            {BiquadType::Peaking, 60.0, 4.0, 0.8},   {BiquadType::Peaking, 180.0, -3.0, 1.2},
            {BiquadType::Peaking, 450.0, 2.0, 2.0},  {BiquadType::Peaking, 1200.0, -4.5, 1.5},
            {BiquadType::Peaking, 2800.0, 5.0, 3.0}, {BiquadType::Peaking, 4500.0, -2.5, 4.0},
            {BiquadType::Peaking, 7000.0, 3.0, 2.5}, {BiquadType::Peaking, 11000.0, -5.0, 1.0},
        };
        PeqLimits lim;
        PeqFit fit = fitPeq(renderCurve(known, kRate), lim);
        std::printf("      8 known bands back: rms %.3f dB, max %.3f dB\n", fit.rmsErrorDb, fit.maxErrorDb);
        check(fit.bands.size() == 8, "returns the device's band count");
        check(fit.rmsErrorDb < 0.1, "recovers a curve made of 8 bands it could have chosen");
    }
    {
        // An AutoEQ-shaped 10-band set (shelves at the ends, peaks between)
        // squeezed onto 8 bands, with the shelves kept as shelves.
        std::vector<PeqBand> tenBand = {
            {BiquadType::LowShelf, 105.0, 5.5, 0.7},   {BiquadType::Peaking, 60.0, -1.5, 1.4},
            {BiquadType::Peaking, 230.0, -2.8, 0.9},   {BiquadType::Peaking, 900.0, 1.6, 1.1},
            {BiquadType::Peaking, 1900.0, -2.2, 2.6},  {BiquadType::Peaking, 3200.0, 3.4, 2.1},
            {BiquadType::Peaking, 5200.0, -4.1, 3.5},  {BiquadType::Peaking, 6800.0, 2.0, 5.0},
            {BiquadType::Peaking, 8900.0, -3.0, 2.8},  {BiquadType::HighShelf, 10000.0, -1.5, 0.7},
        };
        PeqLimits lim;
        lim.types = {BiquadType::LowShelf, BiquadType::Peaking, BiquadType::Peaking, BiquadType::Peaking,
                     BiquadType::Peaking,  BiquadType::Peaking, BiquadType::Peaking, BiquadType::HighShelf};
        PeqFit fit = fitPeq(renderCurve(tenBand, kRate), lim);
        std::printf("      10 bands onto 8: rms %.3f dB, max %.3f dB\n", fit.rmsErrorDb, fit.maxErrorDb);
        printBands(fit);
        check(fit.bands.front().type == BiquadType::LowShelf && fit.bands.back().type == BiquadType::HighShelf,
              "keeps the band types the device requires");
        check(fit.rmsErrorDb < 0.5, "10 bands onto 8 within 0.5 dB rms");
    }
    {
        // A device that stores whole Hz, 0.1 dB and 0.01 Q, and only allows
        // ±6 dB per band, given a target that asks for more than that. The
        // fitter may stack bands to get there; each one must still be legal.
        std::vector<PeqBand> wants = {{BiquadType::Peaking, 300.0, 10.0, 1.0},
                                      {BiquadType::Peaking, 3000.0, -9.0, 2.0}};
        PeqLimits lim;
        lim.minGainDb = -6.0;
        lim.maxGainDb = 6.0;
        lim.minFreq = 25.0;
        lim.maxFreq = 16000.0;
        lim.minQ = 0.3;
        lim.maxQ = 6.0;
        lim.freqStep = 1.0;
        lim.gainStep = 0.1;
        lim.qStep = 0.01;
        PeqFit fit = fitPeq(renderCurve(wants, kRate), lim);
        bool inside = true, onSteps = true;
        for (const PeqBand& b : fit.bands) {
            inside = inside && b.freq >= lim.minFreq && b.freq <= lim.maxFreq && b.gainDb >= lim.minGainDb &&
                     b.gainDb <= lim.maxGainDb && b.q >= lim.minQ && b.q <= lim.maxQ;
            onSteps = onSteps && isMultiple(b.freq, lim.freqStep) && isMultiple(b.gainDb, lim.gainStep) &&
                      isMultiple(b.q, lim.qStep);
        }
        std::printf("      clamped target: rms %.3f dB, peak %+.2f dB\n", fit.rmsErrorDb, fit.peakGainDb);
        check(inside, "every value stays inside the device's limits");
        check(onSteps, "every value lands on the device's resolution");
    }
    {
        PeqFit fit = fitPeq({{20.0, 0.0}, {20000.0, 0.0}}, PeqLimits{});
        double most = 0.0;
        for (const PeqBand& b : fit.bands) most = std::max(most, std::fabs(b.gainDb));
        check(fit.rmsErrorDb < 1e-3 && most < 0.05, "a flat target gives flat bands");
    }

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
