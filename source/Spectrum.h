#pragma once

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <numeric>
#include <vector>

//==============================================================================
// Long-term spectrum of a stereo signal in 1/12-octave bands (20 Hz ... 20 kHz).
// Used for the reference song and for the live signal, so both are measured in
// exactly the same way.
//==============================================================================
namespace Spectrum
{
    constexpr int   numBands      = 121;      // 20 Hz * 2^(120/12) = 20.48 kHz
    constexpr float minFrequency  = 20.0f;
    constexpr float bandsPerOctave = 12.0f;

    using Curve = std::array<float, numBands>;

    inline float bandFrequency (float band)       { return minFrequency * std::exp2 (band / bandsPerOctave); }
    inline float bandOfFrequency (float hz)       { return bandsPerOctave * std::log2 (juce::jmax (hz, 1.0f) / minFrequency); }
    inline float toDb (float power)               { return 10.0f * std::log10 (juce::jmax (power, 1.0e-20f)); }

    // Value of a curve at a fractional band position (linear interpolation, clamped)
    inline float interpolate (const Curve& c, float band)
    {
        band = juce::jlimit (0.0f, (float) (numBands - 1), band);
        const int   i = juce::jmin ((int) band, numBands - 2);
        const float f = band - (float) i;
        return c[(size_t) i] + f * (c[(size_t) i + 1] - c[(size_t) i]);
    }

    //==========================================================================
    // Result of an analysis: power per band (linear) and the correlation of L and R
    // per band (+1 = mono / in phase, 0 = unrelated, -1 = out of phase)
    struct Profile
    {
        Curve left {}, right {}, mid {}, side {}, correlation {};
        bool valid = false;

        // Average level of the mid signal (dB) in the range that matters for the tonal
        // balance; profiles are compared relative to it, so loudness does not matter
        float referenceLevelDb() const
        {
            float sum = 0.0f;
            int count = 0;
            for (int b = 0; b < numBands; ++b)
                if (const auto hz = bandFrequency ((float) b); hz >= 40.0f && hz <= 16000.0f)
                {
                    sum += toDb (mid[(size_t) b]);
                    ++count;
                }
            return sum / (float) juce::jmax (1, count);
        }
    };

    //==========================================================================
    // Short-time FFT analyser (8192 points, Hann window, 75 % overlap). Frames are added one by one;
    // with decay = 1 all frames count equally (reference song), with decay < 1 older
    // frames fade out exponentially (live signal).
    class Analyser final
    {
    public:
        static constexpr int order = 13;
        static constexpr int size  = 1 << order;   // samples per frame
        static constexpr int hop   = size / 4;     // 75 % overlap: a new frame every 43 ms (48 kHz)

        Analyser()
        {
            window.resize ((size_t) size);
            juce::dsp::WindowingFunction<float>::fillWindowingTables (window.data(), (size_t) size,
                                                                      juce::dsp::WindowingFunction<float>::hann, false);
            for (auto* v : { &fftL, &fftR })
                v->resize ((size_t) size * 2);
            for (auto* v : { &powL, &powR, &powM, &powS, &cross })
                v->resize ((size_t) size / 2 + 1);
            reset();
        }

        void reset()
        {
            for (auto* v : { &powL, &powR, &powM, &powS, &cross })
                std::fill (v->begin(), v->end(), 0.0);
            weight = 0.0;
        }

        bool hasData() const  { return weight > 0.0; }

        // l and r point to 'size' samples each
        void addFrame (const float* l, const float* r, double decay = 1.0)
        {
            for (int i = 0; i < size; ++i)
            {
                fftL[(size_t) i] = l[i] * window[(size_t) i];
                fftR[(size_t) i] = r[i] * window[(size_t) i];
            }
            fft.performRealOnlyForwardTransform (fftL.data(), true);
            fft.performRealOnlyForwardTransform (fftR.data(), true);

            // Mid and side are calculated from the complex spectra (no extra FFTs)
            for (size_t k = 0; k <= (size_t) size / 2; ++k)
            {
                const double lr = fftL[2 * k], li = fftL[2 * k + 1];
                const double rr = fftR[2 * k], ri = fftR[2 * k + 1];
                const double mr = 0.5 * (lr + rr), mi = 0.5 * (li + ri);
                const double sr = 0.5 * (lr - rr), si = 0.5 * (li - ri);

                powL[k]  = powL[k]  * decay + lr * lr + li * li;
                powR[k]  = powR[k]  * decay + rr * rr + ri * ri;
                powM[k]  = powM[k]  * decay + mr * mr + mi * mi;
                powS[k]  = powS[k]  * decay + sr * sr + si * si;
                cross[k] = cross[k] * decay + lr * rr + li * ri;   // Re (L * conj (R))
            }
            weight = weight * decay + 1.0;
        }

        // Band profile of the frames added so far
        Profile getProfile (double sampleRate) const
        {
            Profile p;
            if (weight <= 0.0)
                return p;

            // Scaling: a full-scale sine gives about 0 dB in its bin
            const double windowSum = std::accumulate (window.begin(), window.end(), 0.0);
            const double scale     = 4.0 / (windowSum * windowSum) / weight;
            const double binHz     = sampleRate / size;
            const auto   lastBin   = (double) (size / 2);

            for (int b = 0; b < numBands; ++b)
            {
                // Average of the bins inside the band; narrow low bands without a bin
                // centre are interpolated between the neighbouring bins
                const double lo = bandFrequency ((float) b - 0.5f) / binHz;
                const double hi = bandFrequency ((float) b + 0.5f) / binHz;
                const double centre = juce::jmin ((double) bandFrequency ((float) b) / binHz, lastBin);
                const auto first = (size_t) std::ceil (lo), last = (size_t) juce::jmin (std::floor (hi), lastBin);

                const auto bandValue = [&] (const std::vector<double>& v)
                {
                    if (first <= last && lo <= lastBin)
                        return std::accumulate (v.begin() + (long) first, v.begin() + (long) last + 1, 0.0) / (double) (last - first + 1);

                    const auto k0 = (size_t) centre;
                    const auto k1 = juce::jmin (k0 + 1, (size_t) lastBin);
                    const double f = centre - (double) k0;
                    return v[k0] + f * (v[k1] - v[k0]);
                };

                const double l = bandValue (powL), r = bandValue (powR);
                p.left[(size_t) b]  = (float) (l * scale);
                p.right[(size_t) b] = (float) (r * scale);
                p.mid[(size_t) b]   = (float) (bandValue (powM) * scale);
                p.side[(size_t) b]  = (float) (bandValue (powS) * scale);
                p.correlation[(size_t) b] = l * r > 1.0e-30 ? (float) juce::jlimit (-1.0, 1.0, bandValue (cross) / std::sqrt (l * r))
                                                            : 0.0f;
            }

            p.valid = true;
            return p;
        }

    private:
        juce::dsp::FFT fft { order };
        std::vector<float> window, fftL, fftR;
        std::vector<double> powL, powR, powM, powS, cross;   // per bin, summed over the frames
        double weight = 0.0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Analyser)
    };
}
