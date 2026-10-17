// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "../util/biquad.h"
#include <algorithm>
#include <cmath>
#include <vector>

// Fits a fixed number of parametric EQ bands to a target curve, inside a
// device's limits. Written for earbuds that run their own PEQ (the Nothing
// Ear's 8-band "advanced EQ"), where the app can only hand over band
// parameters and the device decides everything else: band count, ranges, and
// the resolution each value is stored at.
//
// Method: greedy placement (each band goes where the residual is largest),
// Levenberg-Marquardt refinement of all bands jointly in continuous space,
// then snapping to the device's steps and a ±1-step search to recover what
// snapping cost. Error is measured in dB on a log-spaced grid, so every
// octave counts the same.
//
// The response model is RBJ biquads at limits.sampleRate. That matches what
// the device runs only if the device uses RBJ filters at that rate. Shelf Q
// in particular is RBJ's Q, not the shelf slope S some firmwares use; confirm
// against a captured curve before trusting shelf bands.
namespace tryptify::fit {

struct PeqBand {
    BiquadType type = BiquadType::Peaking;
    double freq = 1000.0;
    double gainDb = 0.0;
    double q = 1.0;
};

struct CurvePoint {
    double freq;
    double db;
};

struct PeqLimits {
    int numBands = 8;
    double sampleRate = 48000.0;
    double minFreq = 20.0, maxFreq = 20000.0;
    double minGainDb = -12.0, maxGainDb = 12.0;
    double minQ = 0.1, maxQ = 10.0;
    // Resolution the device stores each value at. 0 means continuous.
    double freqStep = 0.0, gainStep = 0.0, qStep = 0.0;
    // One type per band. Empty, or shorter than numBands, pads with Peaking.
    std::vector<BiquadType> types;
};

struct FitOptions {
    double fitMinHz = 20.0;
    double fitMaxHz = 16000.0;  // above this, measurements are rarely trustworthy
    int pointsPerOctave = 24;
    int iterations = 200;       // LM iterations for the final joint pass
};

struct PeqFit {
    std::vector<PeqBand> bands;
    double rmsErrorDb = 0.0;  // over the fit range, after snapping
    double maxErrorDb = 0.0;
    double peakGainDb = 0.0;  // highest point of the fitted response: headroom needed
};

inline double responseDb(const std::vector<PeqBand>& bands, double freq, double sampleRate) {
    double db = 0.0;
    for (const PeqBand& b : bands)
        db += Biquad::design(b.type, sampleRate, b.freq, b.q, b.gainDb).magnitudeDb(freq, sampleRate);
    return db;
}

// Linear in log-frequency, held flat past either end. curve must be sorted
// by ascending frequency.
inline double interpolateDb(const std::vector<CurvePoint>& curve, double freq) {
    if (curve.empty()) return 0.0;
    if (freq <= curve.front().freq) return curve.front().db;
    if (freq >= curve.back().freq) return curve.back().db;
    auto hi = std::lower_bound(curve.begin(), curve.end(), freq,
                               [](const CurvePoint& p, double f) { return p.freq < f; });
    auto lo = hi - 1;
    double t = std::log(freq / lo->freq) / std::log(hi->freq / lo->freq);
    return lo->db + t * (hi->db - lo->db);
}

namespace detail {

// Per band: log2(freq), gain, log2(Q). Logs make a step in freq or Q mean
// the same thing anywhere in the range, which is what LM's damping assumes.
constexpr int kParams = 3;

class Fitter {
public:
    Fitter(const std::vector<CurvePoint>& target, const PeqLimits& limits, const FitOptions& opt)
        : lim_(limits) {
        double lo = std::max(opt.fitMinHz, 1.0);
        double hi = std::min(opt.fitMaxHz, limits.sampleRate * 0.49);
        int n = std::max(2, static_cast<int>(std::ceil(std::log2(hi / lo) * opt.pointsPerOctave)) + 1);
        for (int i = 0; i < n; i++) {
            double f = lo * std::pow(hi / lo, static_cast<double>(i) / (n - 1));
            freqs_.push_back(f);
            target_.push_back(interpolateDb(target, f));
        }
    }

    PeqFit run(int iterations) {
        std::vector<PeqBand> bands;
        int n = std::max(0, lim_.numBands);
        std::vector<BiquadType> types(n, BiquadType::Peaking);
        for (int i = 0; i < n && i < static_cast<int>(lim_.types.size()); i++) types[i] = lim_.types[i];

        // Shelves first: they set the broad tilt that peaks then correct.
        // That order is only for fitting; slot[k] remembers which device
        // band bands[k] is, so each comes back in the slot its type was for.
        std::vector<int> slot;
        for (int pass = 0; pass < 2; pass++) {
            for (int i = 0; i < n; i++) {
                bool shelf = types[i] != BiquadType::Peaking;
                if (shelf != (pass == 0)) continue;
                bands.push_back(initialBand(types[i], bands));
                slot.push_back(i);
                refine(bands, 30);
            }
        }
        refine(bands, iterations);
        snap(bands);

        PeqFit out;
        out.bands.resize(bands.size());
        for (size_t k = 0; k < bands.size(); k++) out.bands[slot[k]] = bands[k];
        double sum = 0.0, peak = -1e9;
        for (size_t i = 0; i < freqs_.size(); i++) {
            double m = responseDb(bands, freqs_[i], lim_.sampleRate);
            double e = target_[i] - m;
            sum += e * e;
            out.maxErrorDb = std::max(out.maxErrorDb, std::fabs(e));
            peak = std::max(peak, m);
        }
        out.rmsErrorDb = freqs_.empty() ? 0.0 : std::sqrt(sum / freqs_.size());
        out.peakGainDb = freqs_.empty() ? 0.0 : peak;
        return out;
    }

private:
    PeqLimits lim_;
    std::vector<double> freqs_, target_;

    double cost(const std::vector<PeqBand>& bands) const {
        double sum = 0.0;
        for (size_t i = 0; i < freqs_.size(); i++) {
            double e = target_[i] - responseDb(bands, freqs_[i], lim_.sampleRate);
            sum += e * e;
        }
        return sum;
    }

    std::vector<double> residual(const std::vector<PeqBand>& bands) const {
        std::vector<double> r(freqs_.size());
        for (size_t i = 0; i < freqs_.size(); i++)
            r[i] = target_[i] - responseDb(bands, freqs_[i], lim_.sampleRate);
        return r;
    }

    PeqBand clampBand(PeqBand b) const {
        b.freq = std::clamp(b.freq, lim_.minFreq, lim_.maxFreq);
        b.gainDb = std::clamp(b.gainDb, lim_.minGainDb, lim_.maxGainDb);
        b.q = std::clamp(b.q, lim_.minQ, lim_.maxQ);
        return b;
    }

    PeqBand initialBand(BiquadType type, const std::vector<PeqBand>& placed) const {
        std::vector<double> r = residual(placed);
        PeqBand b;
        b.type = type;

        if (type == BiquadType::LowShelf || type == BiquadType::HighShelf) {
            // Corner where the bass/treble region conventionally starts; the
            // gain is the mean residual on the shelf's side of it.
            bool low = type == BiquadType::LowShelf;
            b.freq = low ? 105.0 : 10000.0;
            b.q = 0.7;
            double sum = 0.0;
            int count = 0;
            for (size_t i = 0; i < freqs_.size(); i++) {
                if (low ? freqs_[i] <= b.freq : freqs_[i] >= b.freq) {
                    sum += r[i];
                    count++;
                }
            }
            b.gainDb = count ? sum / count : 0.0;
            return clampBand(b);
        }

        // Peak: the biggest residual the band is allowed to reach, with Q
        // from the width of that bump at half its height.
        int best = -1;
        for (size_t i = 0; i < freqs_.size(); i++) {
            if (freqs_[i] < lim_.minFreq || freqs_[i] > lim_.maxFreq) continue;
            if (best < 0 || std::fabs(r[i]) > std::fabs(r[best])) best = static_cast<int>(i);
        }
        if (best < 0) return clampBand(b);
        double peak = r[best];
        int left = best, right = best;
        auto stillIn = [&](int i) { return r[i] * peak > 0.0 && std::fabs(r[i]) >= std::fabs(peak) * 0.5; };
        while (left > 0 && stillIn(left - 1)) left--;
        while (right + 1 < static_cast<int>(freqs_.size()) && stillIn(right + 1)) right++;
        b.freq = freqs_[best];
        b.gainDb = peak;
        double width = freqs_[right] - freqs_[left];
        b.q = width > 0.0 ? b.freq / width : lim_.maxQ;
        return clampBand(b);
    }

    void toParams(const std::vector<PeqBand>& bands, std::vector<double>& x) const {
        x.resize(bands.size() * kParams);
        for (size_t b = 0; b < bands.size(); b++) {
            x[b * kParams + 0] = std::log2(bands[b].freq);
            x[b * kParams + 1] = bands[b].gainDb;
            x[b * kParams + 2] = std::log2(bands[b].q);
        }
    }

    void fromParams(const std::vector<double>& x, std::vector<PeqBand>& bands) const {
        for (size_t b = 0; b < bands.size(); b++) {
            bands[b].freq = std::exp2(x[b * kParams + 0]);
            bands[b].gainDb = x[b * kParams + 1];
            bands[b].q = std::exp2(x[b * kParams + 2]);
            bands[b] = clampBand(bands[b]);
        }
    }

    // Levenberg-Marquardt, with bounds enforced by clamping each trial step.
    void refine(std::vector<PeqBand>& bands, int iterations) const {
        const int p = static_cast<int>(bands.size()) * kParams;
        const int m = static_cast<int>(freqs_.size());
        if (p == 0 || m == 0) return;

        std::vector<double> x, trialX, jac(static_cast<size_t>(m) * p), model(m), probe(m);
        std::vector<double> jtj(static_cast<size_t>(p) * p), jtr(p), a(static_cast<size_t>(p) * p), delta(p);
        std::vector<PeqBand> trial = bands;
        toParams(bands, x);
        double current = cost(bands);
        double lambda = 1e-2;

        for (int it = 0; it < iterations; it++) {
            for (int i = 0; i < m; i++) model[i] = responseDb(bands, freqs_[i], lim_.sampleRate);

            // Forward-difference Jacobian. A column moves one parameter of one
            // band, so only that band's response is recomputed. The moved
            // band is deliberately not clamped: at a bound the derivative is
            // still the derivative, and clamping would zero it.
            const double h = 1e-5;
            for (size_t b = 0; b < bands.size(); b++) {
                std::vector<PeqBand> one{bands[b]};
                for (int i = 0; i < m; i++) probe[i] = responseDb(one, freqs_[i], lim_.sampleRate);
                for (int k = 0; k < kParams; k++) {
                    PeqBand moved = bands[b];
                    if (k == 0) moved.freq = std::exp2(std::log2(moved.freq) + h);
                    if (k == 1) moved.gainDb += h;
                    if (k == 2) moved.q = std::exp2(std::log2(moved.q) + h);
                    one[0] = moved;
                    int j = static_cast<int>(b) * kParams + k;
                    for (int i = 0; i < m; i++)
                        jac[static_cast<size_t>(i) * p + j] =
                            (responseDb(one, freqs_[i], lim_.sampleRate) - probe[i]) / h;
                }
            }

            std::fill(jtj.begin(), jtj.end(), 0.0);
            std::fill(jtr.begin(), jtr.end(), 0.0);
            for (int i = 0; i < m; i++) {
                const double* row = &jac[static_cast<size_t>(i) * p];
                double r = target_[i] - model[i];
                for (int j = 0; j < p; j++) {
                    jtr[j] += row[j] * r;
                    for (int k = j; k < p; k++) jtj[static_cast<size_t>(j) * p + k] += row[j] * row[k];
                }
            }
            for (int j = 0; j < p; j++)
                for (int k = 0; k < j; k++) jtj[static_cast<size_t>(j) * p + k] = jtj[static_cast<size_t>(k) * p + j];

            bool improved = false;
            while (lambda < 1e10) {
                a = jtj;
                for (int j = 0; j < p; j++) {
                    // A zero-gain band has no freq/Q derivative at all; the
                    // floor keeps the system solvable instead of singular.
                    double d = std::max(jtj[static_cast<size_t>(j) * p + j], 1e-6);
                    a[static_cast<size_t>(j) * p + j] += lambda * d;
                }
                delta = jtr;
                if (!solve(a, delta, p)) {
                    lambda *= 4.0;
                    continue;
                }
                trialX = x;
                for (int j = 0; j < p; j++) trialX[j] += delta[j];
                fromParams(trialX, trial);
                double c = cost(trial);
                if (c < current) {
                    bands = trial;
                    toParams(bands, x);  // re-read so x reflects any clamping
                    lambda = std::max(lambda / 3.0, 1e-9);
                    improved = c < current * (1.0 - 1e-9);
                    current = c;
                    break;
                }
                lambda *= 4.0;
            }
            if (!improved) break;
        }
    }

    // Gaussian elimination with partial pivoting; b is overwritten with the
    // solution. False if the matrix is singular.
    static bool solve(std::vector<double>& a, std::vector<double>& b, int n) {
        for (int col = 0; col < n; col++) {
            int pivot = col;
            for (int r = col + 1; r < n; r++)
                if (std::fabs(a[static_cast<size_t>(r) * n + col]) > std::fabs(a[static_cast<size_t>(pivot) * n + col])) pivot = r;
            if (std::fabs(a[static_cast<size_t>(pivot) * n + col]) < 1e-300) return false;
            if (pivot != col) {
                for (int k = 0; k < n; k++) std::swap(a[static_cast<size_t>(col) * n + k], a[static_cast<size_t>(pivot) * n + k]);
                std::swap(b[col], b[pivot]);
            }
            for (int r = col + 1; r < n; r++) {
                double f = a[static_cast<size_t>(r) * n + col] / a[static_cast<size_t>(col) * n + col];
                if (f == 0.0) continue;
                for (int k = col; k < n; k++) a[static_cast<size_t>(r) * n + k] -= f * a[static_cast<size_t>(col) * n + k];
                b[r] -= f * b[col];
            }
        }
        for (int r = n - 1; r >= 0; r--) {
            double s = b[r];
            for (int k = r + 1; k < n; k++) s -= a[static_cast<size_t>(r) * n + k] * b[k];
            b[r] = s / a[static_cast<size_t>(r) * n + r];
        }
        return true;
    }

    // Nearest multiple of step that is still inside [lo, hi].
    static double quantize(double v, double step, double lo, double hi) {
        if (step <= 0.0) return std::clamp(v, lo, hi);
        double q = std::round(v / step) * step;
        if (q > hi) q -= step;
        if (q < lo) q += step;
        return std::clamp(q, lo, hi);
    }

    void snap(std::vector<PeqBand>& bands) const {
        for (PeqBand& b : bands) {
            b.freq = quantize(b.freq, lim_.freqStep, lim_.minFreq, lim_.maxFreq);
            b.gainDb = quantize(b.gainDb, lim_.gainStep, lim_.minGainDb, lim_.maxGainDb);
            b.q = quantize(b.q, lim_.qStep, lim_.minQ, lim_.maxQ);
        }
        // Rounding each value alone ignores how bands interact; nudging one
        // value a step at a time, keeping only what lowers the error, wins
        // most of that back.
        double current = cost(bands);
        for (int pass = 0; pass < 20; pass++) {
            bool changed = false;
            for (PeqBand& b : bands) {
                struct Axis { double* v; double step, lo, hi; };
                Axis axes[3] = {{&b.freq, lim_.freqStep, lim_.minFreq, lim_.maxFreq},
                                {&b.gainDb, lim_.gainStep, lim_.minGainDb, lim_.maxGainDb},
                                {&b.q, lim_.qStep, lim_.minQ, lim_.maxQ}};
                for (Axis& ax : axes) {
                    if (ax.step <= 0.0) continue;
                    for (double dir : {-1.0, 1.0}) {
                        double old = *ax.v;
                        double moved = old + dir * ax.step;
                        if (moved < ax.lo || moved > ax.hi) continue;
                        *ax.v = moved;
                        double c = cost(bands);
                        if (c < current) {
                            current = c;
                            changed = true;
                        } else {
                            *ax.v = old;
                        }
                    }
                }
            }
            if (!changed) break;
        }
    }
};

}  // namespace detail

inline PeqFit fitPeq(const std::vector<CurvePoint>& target, const PeqLimits& limits,
                     const FitOptions& options = {}) {
    return detail::Fitter(target, limits, options).run(options.iterations);
}

// Samples a band set as a curve: how a 10-band EQ from the app becomes the
// target for the earbuds' 8 bands.
inline std::vector<CurvePoint> renderCurve(const std::vector<PeqBand>& bands, double sampleRate,
                                           double minHz = 20.0, double maxHz = 20000.0,
                                           int pointsPerOctave = 48) {
    std::vector<CurvePoint> curve;
    int n = std::max(2, static_cast<int>(std::ceil(std::log2(maxHz / minHz) * pointsPerOctave)) + 1);
    for (int i = 0; i < n; i++) {
        double f = minHz * std::pow(maxHz / minHz, static_cast<double>(i) / (n - 1));
        curve.push_back({f, responseDb(bands, f, sampleRate)});
    }
    return curve;
}

}  // namespace tryptify::fit
