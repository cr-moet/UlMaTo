#pragma once

#include <juce_dsp/juce_dsp.h>
#include <array>

//==============================================================================
// Lookahead brickwall limiter (5 ms), no overshoot:
//   1. required gain per sample = ceiling / peak (true peak: also the peaks between
//      the samples, from an 8x interpolation with 32 taps and a 0.3 dB margin),
//   2. its minimum over the lookahead window (the gain is down before the peak),
//   3. release: rises back exponentially,
//   4. averaged over the lookahead window: a smooth ramp that reaches the required gain
//      exactly at the peak.
// Link: 100 % = the same gain for both channels (no image shift), 0 % = independent.
// Off: the input, delayed by the latency (cross-faded over 20 ms).
//==============================================================================
namespace Limit
{
    struct Settings
    {
        bool on = true, truePeak = true;
        float inputDb = 0.0f, ceilingDb = -0.3f, releaseMs = 100.0f, link = 1.0f;
    };

    class Limiter
    {
    public:
        void prepare (double sampleRate)
        {
            rate = sampleRate;
            lookahead = juce::jmax (8, juce::roundToInt (sampleRate * 0.005));
            for (auto& c : channels)
            {
                c.history.fill (0.0f);
                c.minValues.assign ((size_t) lookahead + 1, 1.0f);   // the window holds up to lookahead + 1 entries
                c.minIndices.assign ((size_t) lookahead + 1, 0);
                c.box.assign ((size_t) lookahead, 1.0f);
                c.delay.assign ((size_t) lookahead - 1, 0.0f);       // the box average ends with the peak's sample
            }

            // 8x interpolation: windowed sinc, 32 taps per phase (phases 1 ... 7)
            for (int p = 1; p < phases; ++p)
                for (int k = 0; k < taps; ++k)
                {
                    const double t = (double) (k - taps / 2 + 1) - (double) p / phases;   // position relative to the centre sample
                    const double x = juce::MathConstants<double>::pi * t;
                    const double sinc = std::abs (t) < 1.0e-9 ? 1.0 : std::sin (x) / x;
                    const double w = 0.5 * (1.0 + std::cos (juce::MathConstants<double>::pi * t / (taps / 2 + 1)));
                    interpolation[(size_t) p - 1][(size_t) k] = (float) (sinc * w);
                }

            inputGain.reset (sampleRate, 0.05);
            reset();
        }

        void reset()
        {
            for (auto& c : channels)
            {
                c.history.fill (0.0f);
                std::fill (c.box.begin(), c.box.end(), 1.0f);
                std::fill (c.delay.begin(), c.delay.end(), 0.0f);
                c.minHead = c.minTail = 0;
                c.release = 1.0f;
                c.boxSum = (double) lookahead;
            }
            position = historyPosition = delayPosition = 0;
            sampleIndex = 0;
            fade = 1.0f;
        }

        // Centre of the interpolation window (taps / 2) + lookahead - 1 (end of the box average)
        int getLatencySamples() const noexcept   { return taps / 2 + lookahead - 1; }

        // Meters (any thread)
        float getGainReductionDb() const noexcept   { return reduction.load (std::memory_order_relaxed); }
        float getOutputPeakDb() const noexcept      { return outputPeak.load (std::memory_order_relaxed); }

        void process (float* left, float* right, int numSamples, const Settings& s) noexcept
        {
            inputGain.setTargetValue (s.on ? juce::Decibels::decibelsToGain (s.inputDb) : 1.0f);
            const float ceiling = juce::Decibels::decibelsToGain (s.ceilingDb);
            const float releaseCoefficient = 1.0f - std::exp (-1.0f / (float) (rate * juce::jmax (1.0f, s.releaseMs) * 0.001));
            const float fadeStep = 1.0f / (float) (rate * 0.02);
            float lowest = 1.0f, peak = 0.0f;

            float* io[] { left, right };

            // Off (and faded out): only the delay, so the latency stays the same
            if (! s.on && fade <= 0.0f)
            {
                for (int i = 0; i < numSamples; ++i)
                {
                    for (size_t c = 0; c < 2; ++c)
                    {
                        auto& ch = channels[c];
                        ch.history[(size_t) historyPosition] = io[c][i];
                        const float centre = ch.history[(size_t) ((historyPosition - taps / 2 + taps) % taps)];
                        io[c][i] = ch.delay[(size_t) delayPosition];
                        ch.delay[(size_t) delayPosition] = centre;
                        ch.release = 1.0f;
                    }
                    historyPosition = (historyPosition + 1) % taps;
                    delayPosition = (delayPosition + 1) % (lookahead - 1);
                    position = (position + 1) % lookahead;
                    ++sampleIndex;
                }
                inputGain.setCurrentAndTargetValue (1.0f);
                reduction.store (0.0f, std::memory_order_relaxed);
                return;
            }

            for (int i = 0; i < numSamples; ++i)
            {
                const float g = inputGain.getNextValue();
                float centre[2], need[2];
                for (size_t c = 0; c < 2; ++c)
                {
                    auto& ch = channels[c];
                    ch.history[(size_t) historyPosition] = io[c][i] * g;

                    // Sample in the middle of the interpolation window, its peak (and between samples)
                    const auto at = [&] (int k) { return ch.history[(size_t) ((historyPosition - (taps - 1) + k + taps) % taps)]; };
                    centre[c] = at (taps / 2 - 1);
                    float p = std::abs (centre[c]);
                    float window = 0.0f;   // more than 6 dB below the ceiling: no peak between the samples can reach it
                    if (s.truePeak)
                        for (int k = 0; k < taps; ++k)
                            window = juce::jmax (window, std::abs (at (k)));
                    if (s.truePeak && window > ceiling * 0.5f)
                        for (const auto& phase : interpolation)
                        {
                            float v = 0.0f;
                            for (int k = 0; k < taps; ++k)
                                v += phase[(size_t) k] * at (k);
                            p = juce::jmax (p, std::abs (v) * margin);
                        }
                    need[c] = p;
                }
                historyPosition = (historyPosition + 1) % taps;

                const float linked = juce::jmax (need[0], need[1]);
                for (size_t c = 0; c < 2; ++c)
                {
                    auto& ch = channels[c];
                    const float p = s.link * linked + (1.0f - s.link) * need[c];
                    const float required = p > ceiling ? ceiling / p : 1.0f;

                    // Minimum over the lookahead window (monotonic queue)
                    const auto size = (int) ch.minValues.size();
                    while (ch.minHead != ch.minTail && ch.minValues[(size_t) ((ch.minTail - 1 + size) % size)] >= required)
                        ch.minTail = (ch.minTail - 1 + size) % size;
                    ch.minValues[(size_t) ch.minTail]  = required;
                    ch.minIndices[(size_t) ch.minTail] = sampleIndex;
                    ch.minTail = (ch.minTail + 1) % size;
                    if (ch.minIndices[(size_t) ch.minHead] <= sampleIndex - lookahead)
                        ch.minHead = (ch.minHead + 1) % size;
                    const float held = ch.minValues[(size_t) ch.minHead];

                    // Release, then the average over the window (smooth attack)
                    ch.release = held < ch.release ? held : ch.release + releaseCoefficient * (held - ch.release);
                    ch.boxSum += (double) ch.release - (double) ch.box[(size_t) position];
                    ch.box[(size_t) position] = ch.release;
                    const float gain = juce::jmin (1.0f, (float) (ch.boxSum / (double) lookahead));

                    // Audio: delayed so that the gain arrives with its peak
                    const float delayed = ch.delay[(size_t) delayPosition];
                    ch.delay[(size_t) delayPosition] = centre[c];
                    const float limited = juce::jlimit (-ceiling, ceiling, delayed * gain);

                    fade = juce::jlimit (0.0f, 1.0f, fade + (s.on ? fadeStep : -fadeStep) * 0.5f);   // two channels per sample
                    io[c][i] = delayed + fade * (limited - delayed);
                    lowest = juce::jmin (lowest, gain);
                    peak = juce::jmax (peak, std::abs (io[c][i]));
                }
                position = (position + 1) % lookahead;
                delayPosition = (delayPosition + 1) % (lookahead - 1);
                ++sampleIndex;
            }

            reduction.store (s.on ? juce::Decibels::gainToDecibels (lowest, -60.0f) : 0.0f, std::memory_order_relaxed);
            outputPeak.store (juce::Decibels::gainToDecibels (peak, -100.0f), std::memory_order_relaxed);
        }

    private:
        static constexpr int taps = 32, phases = 8;
        static constexpr float margin = 1.0351f;   // +0.3 dB: what a 32 tap interpolation may miss

        struct Channel
        {
            std::array<float, taps> history {};
            std::vector<float> minValues, box, delay;
            std::vector<juce::int64> minIndices;
            int minHead = 0, minTail = 0;
            float release = 1.0f;
            double boxSum = 0.0;
        };

        double rate = 48000.0;
        int lookahead = 240, position = 0, historyPosition = 0, delayPosition = 0;
        juce::int64 sampleIndex = 0;
        std::array<Channel, 2> channels;
        std::array<std::array<float, taps>, phases - 1> interpolation {};
        juce::SmoothedValue<float> inputGain { 1.0f };
        float fade = 1.0f;
        std::atomic<float> reduction { 0.0f }, outputPeak { -100.0f };
    };
}
