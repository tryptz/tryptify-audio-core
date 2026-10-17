#pragma once
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// RBJ Audio EQ Cookbook biquad filter.
// Transposed Direct Form II for numerical stability.
enum class BiquadType {
    LowPass,
    HighPass,
    BandPass,
    Notch,
    LowShelf,
    HighShelf,
    Peaking,
    AllPass
};

// Normalized (a0 = 1) coefficients in double precision. The filter itself
// runs in float; this form is for analysis, e.g. the PEQ fitter, which needs
// the response of exactly the filter configure() would build.
struct BiquadCoeffs {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;

    // Magnitude response in dB at freq.
    double magnitudeDb(double freq, double sampleRate) const {
        double w = 2.0 * M_PI * freq / sampleRate;
        double c1 = std::cos(w), s1 = std::sin(w);
        double c2 = std::cos(2.0 * w), s2 = std::sin(2.0 * w);
        double nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2);
        double dr = 1.0 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
        return 10.0 * std::log10((nr * nr + ni * ni) / (dr * dr + di * di));
    }
};

class Biquad {
public:
    void reset() { z1_ = z2_ = 0.0f; }

    void setCoefficients(float b0, float b1, float b2, float a1, float a2) {
        b0_ = b0; b1_ = b1; b2_ = b2; a1_ = a1; a2_ = a2;
    }

    void configure(BiquadType type, double sampleRate, double freq, double q, double gainDb = 0.0) {
        BiquadCoeffs c = design(type, sampleRate, freq, q, gainDb);
        b0_ = static_cast<float>(c.b0);
        b1_ = static_cast<float>(c.b1);
        b2_ = static_cast<float>(c.b2);
        a1_ = static_cast<float>(c.a1);
        a2_ = static_cast<float>(c.a2);
    }

    static BiquadCoeffs design(BiquadType type, double sampleRate, double freq, double q, double gainDb = 0.0) {
        double w0 = 2.0 * M_PI * freq / sampleRate;
        double cosw0 = std::cos(w0);
        double sinw0 = std::sin(w0);
        double alpha = sinw0 / (2.0 * q);

        // Identity until a case sets them; every type does.
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;

        switch (type) {
            case BiquadType::LowPass:
                b0 = (1.0 - cosw0) / 2.0;
                b1 = 1.0 - cosw0;
                b2 = (1.0 - cosw0) / 2.0;
                a0 = 1.0 + alpha;
                a1 = -2.0 * cosw0;
                a2 = 1.0 - alpha;
                break;
            case BiquadType::HighPass:
                b0 = (1.0 + cosw0) / 2.0;
                b1 = -(1.0 + cosw0);
                b2 = (1.0 + cosw0) / 2.0;
                a0 = 1.0 + alpha;
                a1 = -2.0 * cosw0;
                a2 = 1.0 - alpha;
                break;
            case BiquadType::BandPass:
                b0 = alpha;
                b1 = 0.0;
                b2 = -alpha;
                a0 = 1.0 + alpha;
                a1 = -2.0 * cosw0;
                a2 = 1.0 - alpha;
                break;
            case BiquadType::Notch:
                b0 = 1.0;
                b1 = -2.0 * cosw0;
                b2 = 1.0;
                a0 = 1.0 + alpha;
                a1 = -2.0 * cosw0;
                a2 = 1.0 - alpha;
                break;
            case BiquadType::LowShelf: {
                double A = std::pow(10.0, gainDb / 40.0);
                double sq = 2.0 * std::sqrt(A) * alpha;
                b0 = A * ((A + 1.0) - (A - 1.0) * cosw0 + sq);
                b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cosw0);
                b2 = A * ((A + 1.0) - (A - 1.0) * cosw0 - sq);
                a0 = (A + 1.0) + (A - 1.0) * cosw0 + sq;
                a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cosw0);
                a2 = (A + 1.0) + (A - 1.0) * cosw0 - sq;
                break;
            }
            case BiquadType::HighShelf: {
                double A = std::pow(10.0, gainDb / 40.0);
                double sq = 2.0 * std::sqrt(A) * alpha;
                b0 = A * ((A + 1.0) + (A - 1.0) * cosw0 + sq);
                b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosw0);
                b2 = A * ((A + 1.0) + (A - 1.0) * cosw0 - sq);
                a0 = (A + 1.0) - (A - 1.0) * cosw0 + sq;
                a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cosw0);
                a2 = (A + 1.0) - (A - 1.0) * cosw0 - sq;
                break;
            }
            case BiquadType::Peaking: {
                double A = std::pow(10.0, gainDb / 40.0);
                b0 = 1.0 + alpha * A;
                b1 = -2.0 * cosw0;
                b2 = 1.0 - alpha * A;
                a0 = 1.0 + alpha / A;
                a1 = -2.0 * cosw0;
                a2 = 1.0 - alpha / A;
                break;
            }
            case BiquadType::AllPass:
                // Unity magnitude everywhere; 360° of phase rotation around
                // freq, sharpness set by q (RBJ cookbook APF).
                b0 = 1.0 - alpha;
                b1 = -2.0 * cosw0;
                b2 = 1.0 + alpha;
                a0 = 1.0 + alpha;
                a1 = -2.0 * cosw0;
                a2 = 1.0 - alpha;
                break;
        }

        // Normalize
        return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
    }

    float process(float in) {
        float out = b0_ * in + z1_;
        z1_ = b1_ * in - a1_ * out + z2_;
        z2_ = b2_ * in - a2_ * out;
        return out;
    }

    void processBlock(float* data, int numFrames) {
        for (int i = 0; i < numFrames; i++) {
            data[i] = process(data[i]);
        }
    }

private:
    float b0_ = 1.0f, b1_ = 0.0f, b2_ = 0.0f;
    float a1_ = 0.0f, a2_ = 0.0f;
    float z1_ = 0.0f, z2_ = 0.0f;
};
