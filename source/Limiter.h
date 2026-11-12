#pragma once

#include <juce_dsp/juce_dsp.h>
#include <array>

//==============================================================================
// Mastering limiter (lookahead, brickwall, no overshoot) with the functions of a modern
// mastering limiter:
//   GAIN / OUTPUT (ceiling), LOOKAHEAD 0.1 ... 5 ms, ATTACK (shape of the ramp in the
//   lookahead), RELEASE 0 ms ... 10 s or AUTO, CHANNEL LINK for transients and for the
//   release, OVERSAMPLING off / 2x / 4x / 8x, TRUE PEAK, DC filter, UNITY GAIN (listen at
//   the input level), DITHER 24 / 20 / 16 bit with noise shaping, meters (output true
//   peak and RMS, gain reduction, loudness after ITU BS.1770: momentary, short term,
//   integrated, loudness range) and a history for the scrolling display.
//
// Core, per sample (at the oversampled rate):
//   1. required gain = ceiling / peak (true peak: also between
//      the samples, 8x interpolation with 32 taps and a 0.3 dB margin),
//   2. linked over the channels (transients link), minimum over the lookahead window,
//   3. release (slightly longer while it limits for long, shorter after short peaks; AUTO:
//      a fast release for transients and a slow one for sustained limiting, the lower
//      gain of both wins), linked over the channels (release link),
//   4. averaged over the attack part of the window: a smooth ramp that reaches the
//      required gain exactly at the peak.
// The latency is constant (lookahead and oversampling only move internal delays).
// Off: the input, delayed by the latency (cross-faded over 20 ms).
//==============================================================================
namespace Limit
{
    struct Settings
    {
        bool on = true, truePeak = true, dcFilter = false, unityGain = false, autoRelease = false;
        float inputDb = 0.0f, ceilingDb = -0.3f, releaseMs = 100.0f, lookaheadMs = 5.0f, attack = 1.0f;
        float linkTransients = 1.0f, linkRelease = 1.0f;   // 0 ... 1
        int oversampling = 0;    // 0 off, 1 = 2x, 2 = 4x, 3 = 8x
        int ditherBits = 0;      // 0 off, else 24 / 20 / 16
        int noiseShaping = 0;    // 0 off, 1 basic, 2 optimized
    };

    // One column of the history display
    struct Column
    {
        float input = 0.0f, output = 0.0f, reduction = 0.0f;   // peak input / output (linear), reduction in dB (<= 0)
    };

    struct Meters
    {
        float truePeakL = -100.0f, truePeakR = -100.0f, rmsL = -100.0f, rmsR = -100.0f;  // output: true peak of the last block, RMS (300 ms)
        float reduction = 0.0f, truePeak = -100.0f;                                       // dB; true peak: maximum since reset
        float momentary = -100.0f, shortTerm = -100.0f, integrated = -100.0f, range = 0.0f;   // LUFS, LU
    };

    //==========================================================================
    // The last 'size' samples, always contiguous (each sample is written twice), oldest first
    template <int size>
    struct Window
    {
        const float* push (float x) noexcept
        {
            data[(size_t) position] = data[(size_t) (position + size)] = x;
            position = (position + 1) % size;
            return data.data() + position;   // oldest ... newest (data[position + size - 1] is x)
        }
        void clear() noexcept   { data.fill (0.0f); position = 0; }

        std::array<float, 2 * (size_t) size> data {};
        int position = 0;
    };

    // Dot product (n a multiple of 4); four sums, so the compiler can use SIMD
    inline float dot (const float* a, const float* b, int n) noexcept
    {
        float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
        for (int k = 0; k < n; k += 4)
        {
            s0 += a[k] * b[k];
            s1 += a[k + 1] * b[k + 1];
            s2 += a[k + 2] * b[k + 2];
            s3 += a[k + 3] * b[k + 3];
        }
        return (s0 + s1) + (s2 + s3);
    }

    //==========================================================================
    // Loudness after ITU BS.1770 / EBU R128: K-weighting, 100 ms parts; momentary 400 ms,
    // short term 3 s, integrated gated (-70 LUFS absolute, -10 LU relative), loudness
    // range from the short term values (-20 LU relative, 10 % ... 95 %)
    class Loudness
    {
    public:
        void prepare (double rate)
        {
            const auto shelf = [rate]
            {
                const double g = 3.999843853973347, q = 0.7071752369554196, fc = 1681.974450955533;
                const double a = std::pow (10.0, g / 40.0), w = juce::MathConstants<double>::twoPi * fc / rate;
                const double c = std::cos (w), alpha = std::sin (w) / (2.0 * q), sa = 2.0 * std::sqrt (a) * alpha;
                return std::array<double, 6> { a * ((a + 1) + (a - 1) * c + sa), -2 * a * ((a - 1) + (a + 1) * c), a * ((a + 1) + (a - 1) * c - sa),
                                               (a + 1) - (a - 1) * c + sa, 2 * ((a - 1) - (a + 1) * c), (a + 1) - (a - 1) * c - sa };
            }();
            const auto highpass = [rate]
            {
                const double q = 0.5003270373238773, fc = 38.13547087602444, w = juce::MathConstants<double>::twoPi * fc / rate;
                const double c = std::cos (w), alpha = std::sin (w) / (2.0 * q);
                return std::array<double, 6> { (1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + alpha, -2 * c, 1 - alpha };
            }();
            for (auto [stage, k] : { std::pair { 0, shelf }, std::pair { 1, highpass } })
                for (size_t i = 0; i < 5; ++i)
                    coefficients[(size_t) stage][i] = k[i < 3 ? i : i + 1] / k[3];
            partLength = juce::jmax (1, juce::roundToInt (rate * 0.1));
            reset();
        }

        void reset()
        {
            for (auto& s : state)
                s.fill (0.0);
            parts.fill (0.0);
            partCount = partSamples = 0;
            partSum = 0.0;
            integratedBlocks.fill ({});
            rangeBlocks.fill (0);
            momentary = shortTerm = integrated = -100.0f;
            range = 0.0f;
        }

        void add (float l, float r) noexcept
        {
            double sum = 0.0;
            for (size_t c = 0; c < 2; ++c)
            {
                double x = c == 0 ? l : r;
                for (size_t st = 0; st < 2; ++st)
                {
                    const auto& k = coefficients[st];
                    auto& z = state[c * 2 + st];
                    const double y = k[0] * x + z[0];
                    z[0] = k[1] * x - k[3] * y + z[1];
                    z[1] = k[2] * x - k[4] * y;
                    x = y;
                }
                sum += x * x;
            }
            partSum += sum;
            if (++partSamples >= partLength)
                finishPart();
        }

        float momentary = -100.0f, shortTerm = -100.0f, integrated = -100.0f, range = 0.0f;

    private:
        static constexpr int bins = 800;   // 0.1 LU from -70 to +10 LUFS
        static float lufs (double meanSquare)   { return meanSquare > 0.0 ? (float) (-0.691 + 10.0 * std::log10 (meanSquare)) : -100.0f; }
        static int binOf (float l)              { return juce::jlimit (0, bins - 1, (int) ((l + 70.0f) * 10.0f)); }

        void finishPart() noexcept
        {
            parts[(size_t) (partCount % 30)] = partSum / partLength;
            ++partCount;
            partSum = 0.0;
            partSamples = 0;

            const auto mean = [this] (int count)
            {
                const int n = (int) juce::jmin ((juce::int64) count, partCount);
                double s = 0.0;
                for (int i = 0; i < n; ++i)
                    s += parts[(size_t) ((partCount - 1 - i) % 30)];
                return n > 0 ? s / count : 0.0;
            };
            const double m = mean (4), st = mean (30);
            momentary = lufs (m);
            shortTerm = lufs (st);

            // Integrated: 400 ms blocks every 100 ms, gated
            if (partCount >= 4 && momentary > -70.0f)
            {
                auto& b = integratedBlocks[(size_t) binOf (momentary)];
                ++b.count;
                b.energy += m;
            }
            double all = 0.0;
            juce::int64 count = 0;
            for (const auto& b : integratedBlocks) { all += b.energy; count += b.count; }
            if (count > 0)
            {
                const float gate = lufs (all / (double) count) - 10.0f;
                double gated = 0.0;
                juce::int64 n = 0;
                for (int i = binOf (gate); i < bins; ++i) { gated += integratedBlocks[(size_t) i].energy; n += integratedBlocks[(size_t) i].count; }
                integrated = n > 0 ? lufs (gated / (double) n) : -100.0f;
            }

            // Loudness range: short term values (3 s windows) every 100 ms
            if (partCount >= 30 && shortTerm > -70.0f)
                ++rangeBlocks[(size_t) binOf (shortTerm)];
            double energy = 0.0;
            juce::int64 total = 0;
            for (int i = 0; i < bins; ++i)
                if (rangeBlocks[(size_t) i] > 0)
                {
                    energy += (double) rangeBlocks[(size_t) i] * std::pow (10.0, (-70.0 + (i + 0.5) * 0.1 + 0.691) / 10.0);
                    total += rangeBlocks[(size_t) i];
                }
            if (total > 0)
            {
                const int first = binOf (lufs (energy / (double) total) - 20.0f);
                juce::int64 n = 0;
                for (int i = first; i < bins; ++i) n += rangeBlocks[(size_t) i];
                const auto percentile = [&] (double p)
                {
                    juce::int64 seen = 0;
                    for (int i = first; i < bins; ++i)
                        if ((seen += rangeBlocks[(size_t) i]) >= (juce::int64) std::ceil (p * (double) n))
                            return -70.0f + ((float) i + 0.5f) * 0.1f;
                    return 10.0f;
                };
                range = n > 1 ? percentile (0.95) - percentile (0.10) : 0.0f;
            }
        }

        struct Bin { juce::int64 count = 0; double energy = 0.0; };
        std::array<std::array<double, 5>, 2> coefficients {};
        std::array<std::array<double, 2>, 4> state {};
        std::array<double, 30> parts {};
        std::array<Bin, bins> integratedBlocks {};
        std::array<juce::int64, bins> rangeBlocks {};
        juce::int64 partCount = 0;
        int partLength = 4800, partSamples = 0;
        double partSum = 0.0;
    };

    //==========================================================================
    // The limiting itself at one sample rate (the base rate or an oversampled one)
    class Core
    {
    public:
        struct Params
        {
            float ceiling = 1.0f, release = 0.001f, adaptive = 0.0f, linkTransients = 1.0f, linkRelease = 1.0f;
            float autoFast = 0.0f, autoAttack = 0.0f, autoSlow = 0.0f;   // AUTO release: coefficients per sample (0: off)
            int lookahead = 1, attack = 1;   // samples
            bool truePeak = true;
        };

        void prepare (double sampleRate, int maximumLookahead, bool interpolate)
        {
            rate = sampleRate;
            maxLookahead = juce::jmax (2, maximumLookahead);
            useInterpolation = interpolate;
            for (auto& c : channels)
            {
                c.minValues.assign ((size_t) maxLookahead + 1, 1.0f);
                c.minIndices.assign ((size_t) maxLookahead + 1, 0);
                c.sums.assign ((size_t) maxLookahead + 1, 0.0);
                c.gains.assign ((size_t) maxLookahead, 1.0f);
                c.delay.assign ((size_t) maxLookahead - 1, 0.0f);
            }
            for (int p = 1; p < phases; ++p)
                for (int k = 0; k < taps; ++k)
                {
                    const double t = (double) (k - taps / 2 + 1) - (double) p / phases;
                    const double x = juce::MathConstants<double>::pi * t;
                    const double sinc = std::abs (t) < 1.0e-9 ? 1.0 : std::sin (x) / x;
                    const double w = 0.5 * (1.0 + std::cos (juce::MathConstants<double>::pi * t / (taps / 2 + 1)));
                    interpolation[(size_t) p - 1][(size_t) k] = (float) (sinc * w);
                }
            sustainCoefficient = 1.0f - std::exp (-1.0f / (float) (rate * 0.3));
            reset();
        }

        void reset()
        {
            for (auto& c : channels)
            {
                c.history.clear();
                std::fill (c.sums.begin(), c.sums.end(), 0.0);
                std::fill (c.gains.begin(), c.gains.end(), 1.0f);
                std::fill (c.delay.begin(), c.delay.end(), 0.0f);
                c.minHead = c.minTail = 0;
                c.release = 1.0f;
                c.sum = 0.0;
                c.sustain = 0.0f;
                c.fast = c.slow = 1.0f;
            }
            index = 0;
        }

        // Interpolation centre + the full lookahead (constant, whatever the lookahead is)
        int getLatencySamples() const noexcept   { return taps / 2 + maxLookahead - 1; }
        int getMaxLookahead() const noexcept     { return maxLookahead; }

        // In place; returns the lowest gain of the two channels
        float process (float& left, float& right, const Params& p) noexcept
        {
            float* io[] { &left, &right };
            float centre[2], need[2];
            for (size_t c = 0; c < 2; ++c)
            {
                auto& ch = channels[c];
                const float* window = ch.history.push (*io[c]);   // the last 'taps' samples, oldest first
                centre[c] = window[taps / 2 - 1];
                float peak = std::abs (centre[c]);
                if (p.truePeak)
                {
                    if (useInterpolation)
                    {
                        const auto range = juce::FloatVectorOperations::findMinAndMax (window, taps);
                        if (juce::jmax (-range.getStart(), range.getEnd()) > p.ceiling * 0.5f)
                            for (const auto& phase : interpolation)
                                peak = juce::jmax (peak, std::abs (dot (phase.data(), window, taps)) * margin);
                    }
                    else
                        peak *= 1.0116f;   // oversampled 4x / 8x: 0.1 dB for the rest
                }
                need[c] = peak;
            }

            const float linked = juce::jmax (need[0], need[1]);
            const int size = maxLookahead + 1;
            float lowest = 1.0f;
            for (size_t c = 0; c < 2; ++c)
            {
                auto& ch = channels[c];
                const float peak = p.linkTransients * linked + (1.0f - p.linkTransients) * need[c];
                const float required = peak > p.ceiling ? p.ceiling / peak : 1.0f;

                // Minimum over the lookahead (monotonic queue)
                while (ch.minHead != ch.minTail && ch.minValues[(size_t) ((ch.minTail - 1 + size) % size)] >= required)
                    ch.minTail = (ch.minTail - 1 + size) % size;
                ch.minValues[(size_t) ch.minTail] = required;
                ch.minIndices[(size_t) ch.minTail] = index;
                ch.minTail = (ch.minTail + 1) % size;
                while (ch.minIndices[(size_t) ch.minHead] <= index - p.lookahead)
                    ch.minHead = (ch.minHead + 1) % size;
                const float held = ch.minValues[(size_t) ch.minHead];

                if (p.autoFast > 0.0f)
                {
                    // AUTO: fast stage (back quickly after a transient) and slow stage (follows
                    // sustained limiting slowly and lets go slowly); the lower one wins
                    ch.fast = held < ch.fast ? held : ch.fast + p.autoFast * (held - ch.fast);
                    ch.slow += (held < ch.slow ? p.autoAttack : p.autoSlow) * (held - ch.slow);
                    ch.release = juce::jmin (ch.fast, ch.slow);
                }
                else
                {
                    // Release: a little longer while it keeps limiting, shorter after short peaks
                    const float depth = -juce::Decibels::gainToDecibels (ch.release, -40.0f);
                    ch.sustain += sustainCoefficient * (depth - ch.sustain);
                    const float scale = 1.0f + p.adaptive * (juce::jlimit (0.0f, 1.0f, ch.sustain / 6.0f) * 2.5f - 0.6f);
                    ch.release = held < ch.release ? held : ch.release + p.release / scale * (held - ch.release);
                    ch.fast = ch.slow = ch.release;
                }
            }

            const float both = juce::jmin (channels[0].release, channels[1].release);
            const int slot = (int) (index % size);
            for (size_t c = 0; c < 2; ++c)
            {
                auto& ch = channels[c];
                const float released = p.linkRelease * both + (1.0f - p.linkRelease) * ch.release;

                // Attack: average over the last 'attack' samples (prefix sums), ends with the peak
                ch.sum += released;
                ch.sums[(size_t) slot] = ch.sum;
                const double before = ch.sums[(size_t) ((index - p.attack + size * 4) % size)];
                const float ramp = index >= p.attack ? (float) ((ch.sum - before) / p.attack) : released;

                // The ramp belongs to the sample lookahead - 1 back; the audio is delayed by the
                // full lookahead - 1, so the gain waits the difference
                const int gainSlot = (int) (index % maxLookahead);
                ch.gains[(size_t) gainSlot] = juce::jmin (1.0f, ramp);
                const float gain = ch.gains[(size_t) ((index - (maxLookahead - p.lookahead) + (juce::int64) maxLookahead * 4) % maxLookahead)];

                const int delaySlot = (int) (index % (maxLookahead - 1));
                const float delayed = ch.delay[(size_t) delaySlot];
                ch.delay[(size_t) delaySlot] = centre[c];
                const float limited = juce::jlimit (-p.ceiling, p.ceiling, delayed * gain);
                *io[c] = limited;
                lowest = juce::jmin (lowest, gain);
            }
            ++index;
            return lowest;
        }

    private:
        static constexpr int taps = 32, phases = 8;
        static constexpr float margin = 1.0351f;   // +0.3 dB: what a 32 tap interpolation may miss

        struct Channel
        {
            Window<taps> history;
            std::vector<float> minValues, gains, delay;
            std::vector<juce::int64> minIndices;
            std::vector<double> sums;
            int minHead = 0, minTail = 0;
            float release = 1.0f, sustain = 0.0f, fast = 1.0f, slow = 1.0f;
            double sum = 0.0;
        };

        double rate = 48000.0;
        int maxLookahead = 240;
        bool useInterpolation = true;
        juce::int64 index = 0;
        float sustainCoefficient = 0.0f;
        std::array<Channel, 2> channels;
        std::array<std::array<float, taps>, phases - 1> interpolation {};
    };

    //==========================================================================
    class Limiter
    {
    public:
        void prepare (double sampleRate, int maxBlockSize = 2048)
        {
            rate = sampleRate;
            maxBlock = juce::jlimit (1, 8192, maxBlockSize);

            // Core latency in base samples, the same for every oversampling factor
            const int lookahead = juce::jmax (8, juce::roundToInt (sampleRate * 0.005));
            coreLatency = 16 + lookahead - 1;
            double longest = 0.0;
            for (int k = 0; k < 4; ++k)
            {
                const int factor = 1 << k;
                cores[(size_t) k].prepare (sampleRate * factor, coreLatency * factor - 16 + 1, k < 2);
                double osLatency = 0.0;
                if (k > 0)
                {
                    auto& os = oversamplers[(size_t) k - 1];
                    os = std::make_unique<juce::dsp::Oversampling<float>> (2, (size_t) k, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true);
                    os->initProcessing ((size_t) maxBlock);
                    osLatency = (double) os->getLatencyInSamples();
                }
                pathLatency[(size_t) k] = coreLatency + osLatency;
                longest = juce::jmax (longest, pathLatency[(size_t) k]);
            }
            const int paths = (int) std::ceil (longest - 1.0e-6);
            for (int k = 0; k < 4; ++k)
                padding[(size_t) k] = juce::jmax (0, juce::roundToInt ((double) paths - pathLatency[(size_t) k]));

            // Safety stage at the base rate after the downsampling (its filter can raise the
            // peaks a little): a second limiter, 1 ms lookahead, fast release
            safety.prepare (sampleRate, juce::jmax (8, juce::roundToInt (sampleRate * 0.001)), true);
            latency = paths + safety.getLatencySamples();

            dryDelay.setSize (2, latency + maxBlock + 1);   // the whole chunk is written before it is read
            padDelay.setSize (2, paths + 1);
            work.setSize (2, maxBlock);
            inputGain.reset (sampleRate, 0.05);
            unityGain.reset (sampleRate, 0.05);
            fadeStep = 1.0f / (float) (sampleRate * 0.02);
            switchStep = 1.0f / (float) (sampleRate * 0.008);
            dcCoefficient = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * 5.0f / (float) sampleRate);
            columnLength = juce::jmax (1, juce::roundToInt (sampleRate / 150.0));

            // 4x interpolation for the true peak meter: 16 taps
            for (int p = 1; p < 4; ++p)
                for (int k = 0; k < 16; ++k)
                {
                    const double t = (double) (k - 7) - p / 4.0, x = juce::MathConstants<double>::pi * t;
                    meterKernel[(size_t) p - 1][(size_t) k] = (float) ((std::abs (t) < 1.0e-9 ? 1.0 : std::sin (x) / x) * 0.5 * (1.0 + std::cos (juce::MathConstants<double>::pi * t / 9.0)));
                }
            loudness.prepare (sampleRate);
            rmsCoefficient = 1.0f - std::exp (-1.0f / (float) (sampleRate * 0.3));
            reset();
        }

        void reset()
        {
            for (auto& c : cores)
                c.reset();
            safety.reset();
            for (auto& os : oversamplers)
                if (os != nullptr)
                    os->reset();
            dryDelay.clear();
            padDelay.clear();
            delayPosition = padPosition = 0;
            fade = 1.0f;
            switchGain = 1.0f;
            dcState = { 0.0f, 0.0f };
            ditherError = {};
            for (auto& h : meterHistory)
                h.clear();
            loudness.reset();
            truePeakMax = 0.0f;
            blockTruePeak = rmsState = {};
            column = {};
            columnSamples = 0;
        }

        int getLatencySamples() const noexcept   { return latency; }

        // Meters and history (any thread; the history has one reader: the display)
        float getGainReductionDb() const noexcept   { return reduction.load (std::memory_order_relaxed); }
        Meters getMeters() const noexcept
        {
            Meters m;
            m.reduction = reduction.load(); m.truePeak = meters[truePeakMeter].load();
            m.momentary = meters[momentaryMeter].load(); m.shortTerm = meters[shortTermMeter].load();
            m.integrated = meters[integratedMeter].load(); m.range = meters[rangeMeter].load();
            m.truePeakL = meters[truePeakLeft].load(); m.truePeakR = meters[truePeakLeft + 1].load();
            m.rmsL = meters[rmsLeft].load(); m.rmsR = meters[rmsLeft + 1].load();
            return m;
        }
        void resetLoudness() noexcept   { resetRequest.store (true); }

        int readColumns (Column* destination, int maxCount) noexcept
        {
            const auto scope = fifo.read (juce::jmin (maxCount, fifo.getNumReady()));
            for (int i = 0; i < scope.blockSize1; ++i) destination[i] = columns[(size_t) (scope.startIndex1 + i)];
            for (int i = 0; i < scope.blockSize2; ++i) destination[scope.blockSize1 + i] = columns[(size_t) (scope.startIndex2 + i)];
            return scope.blockSize1 + scope.blockSize2;
        }

        void process (float* left, float* right, int numSamples, const Settings& s) noexcept
        {
            if (resetRequest.exchange (false))
            {
                loudness.reset();
                truePeakMax = 0.0f;
            }
            for (int start = 0; start < numSamples; start += maxBlock)
                processChunk (left + start, right + start, juce::jmin (maxBlock, numSamples - start), s);
        }

    private:
        void processChunk (float* left, float* right, int n, const Settings& s) noexcept
        {
            // Oversampling changed: fade the path out, switch, fade in
            const int wanted = juce::jlimit (0, 3, s.oversampling);
            if (wanted != current && switchGain <= 0.0f)
            {
                current = wanted;
                cores[(size_t) current].reset();
                if (current > 0)
                    oversamplers[(size_t) current - 1]->reset();
                padDelay.clear();
            }
            const bool switching = wanted != current;

            const int factor = 1 << current;
            auto& core = cores[(size_t) current];
            Core::Params p;
            p.ceiling = juce::Decibels::decibelsToGain (s.ceilingDb);
            const auto perSample = [this, factor] (float ms) { return 1.0f / (float) (rate * factor * juce::jmax (0.05f, ms) * 0.001); };
            p.release = perSample (s.releaseMs);
            p.adaptive = 0.8f;
            if (s.autoRelease)
            {
                p.autoFast = perSample (40.0f);
                p.autoAttack = perSample (300.0f);
                p.autoSlow = perSample (1500.0f);
            }
            p.linkTransients = s.linkTransients;
            p.linkRelease = s.linkRelease;
            p.lookahead = juce::jlimit (1, core.getMaxLookahead(), juce::roundToInt (s.lookaheadMs * 0.001 * rate * factor));
            p.attack = juce::jlimit (1, p.lookahead, juce::roundToInt ((float) p.lookahead * juce::jlimit (0.02f, 1.0f, 0.1f + 0.9f * s.attack)));
            p.truePeak = s.truePeak;

            Core::Params safe;
            safe.ceiling = p.ceiling;
            safe.release = 1.0f / (float) (rate * 0.03);
            safe.lookahead = safe.attack = safety.getMaxLookahead();
            safe.truePeak = s.truePeak;

            inputGain.setTargetValue (s.on ? juce::Decibels::decibelsToGain (s.inputDb) : 1.0f);
            unityGain.setTargetValue (s.on && s.unityGain ? juce::Decibels::decibelsToGain (-s.inputDb) : 1.0f);
            const bool active = s.on || fade > 0.0f;
            if (active && ! wasActive)   // back on after a while: no stale state
            {
                cores[(size_t) current].reset();
                safety.reset();
                if (current > 0)
                    oversamplers[(size_t) current - 1]->reset();
            }
            wasActive = active;

            // Input: gain, DC filter, meters, dry delay (for off)
            float* wl = work.getWritePointer (0);
            float* wr = work.getWritePointer (1);
            float* dl = dryDelay.getWritePointer (0);
            float* dr = dryDelay.getWritePointer (1);
            const int length = dryDelay.getNumSamples();
            for (int i = 0; i < n; ++i)
            {
                const int slot = (delayPosition + i) % length;
                const float g = inputGain.getNextValue();
                float x[] { left[i] * g, right[i] * g };
                for (size_t c = 0; c < 2; ++c)
                {
                    if (s.dcFilter)
                    {
                        dcState[c] += dcCoefficient * (x[c] - dcState[c]);
                        x[c] -= dcState[c];
                    }
                }
                wl[i] = x[0];
                wr[i] = x[1];
                columnInput[(size_t) i] = juce::jmax (std::abs (x[0]), std::abs (x[1]));
                dl[slot] = left[i];   // written now, read 'latency' samples later
                dr[slot] = right[i];
            }

            // Limiting (at the oversampled rate)
            float lowest = 1.0f;
            if (active)
            {
                juce::dsp::AudioBlock<float> block (work.getArrayOfWritePointers(), 2, (size_t) n);
                if (current == 0)
                {
                    for (int i = 0; i < n; ++i)
                        columnGain[(size_t) i] = core.process (wl[i], wr[i], p);
                }
                else
                {
                    auto& os = *oversamplers[(size_t) current - 1];
                    auto up = os.processSamplesUp (block);
                    float* ul = up.getChannelPointer (0);
                    float* ur = up.getChannelPointer (1);
                    for (int i = 0; i < (int) up.getNumSamples(); ++i)
                    {
                        const float gr = core.process (ul[i], ur[i], p);
                        float& slot = columnGain[(size_t) (i / factor)];
                        slot = (i % factor == 0) ? gr : juce::jmin (slot, gr);
                    }
                    os.processSamplesDown (block);
                }
            }
            else
                std::fill (columnGain.begin(), columnGain.begin() + n, 1.0f);

            // Output: padding to the constant latency, safety clip, unity gain, dither, meters
            float* pl = padDelay.getWritePointer (0);
            float* pr = padDelay.getWritePointer (1);
            const int pad = padding[(size_t) current], padLength = padDelay.getNumSamples();
            const float ceiling = juce::Decibels::decibelsToGain (s.ceilingDb);
            for (int i = 0; i < n; ++i)
            {
                const int slot = (delayPosition + i) % length;
                const int padSlot = (padPosition + i) % padLength;
                pl[padSlot] = wl[i];
                pr[padSlot] = wr[i];
                const int padRead = (padPosition + i - pad + padLength * 2) % padLength;
                float wet[] { pl[padRead], pr[padRead] };
                if (active)
                    columnGain[(size_t) i] = juce::jmin (columnGain[(size_t) i], safety.process (wet[0], wet[1], safe));
                const int dryRead = (slot - latency + length * 2) % length;
                const float dry[] { dl[dryRead], dr[dryRead] };

                switchGain = juce::jlimit (0.0f, 1.0f, switchGain + (switching ? -switchStep : switchStep));
                fade = juce::jlimit (0.0f, 1.0f, fade + (s.on ? fadeStep : -fadeStep));
                const float unity = unityGain.getNextValue();
                float out[2];
                for (size_t c = 0; c < 2; ++c)
                {
                    float w = juce::jlimit (-ceiling, ceiling, wet[c]);
                    w *= switchGain * unity;
                    out[c] = dry[c] + fade * (w - dry[c]);
                    if (s.on && s.ditherBits > 0)
                        out[c] = dither (out[c], c, s.ditherBits, s.noiseShaping);
                }
                left[i] = out[0];
                right[i] = out[1];
                lowest = juce::jmin (lowest, columnGain[(size_t) i]);
                meterOutput (out[0], out[1], columnInput[(size_t) i], columnGain[(size_t) i]);
            }
            delayPosition = (delayPosition + n) % length;
            padPosition = (padPosition + n) % padLength;

            const auto db = [] (float v) { return juce::Decibels::gainToDecibels (v, -100.0f); };
            meters[truePeakMeter].store (db (truePeakMax));
            for (size_t c = 0; c < 2; ++c)
            {
                meters[truePeakLeft + c].store (db (blockTruePeak[c]));
                meters[rmsLeft + c].store (db (std::sqrt (rmsState[c])));
                blockTruePeak[c] = 0.0f;
            }
            meters[momentaryMeter].store (loudness.momentary); meters[shortTermMeter].store (loudness.shortTerm);
            meters[integratedMeter].store (loudness.integrated); meters[rangeMeter].store (loudness.range);
            reduction.store (s.on ? juce::Decibels::gainToDecibels (lowest, -60.0f) : 0.0f, std::memory_order_relaxed);
        }

        // TPDF dither to the given word length; noise shaping by error feedback
        float dither (float x, size_t c, int bits, int shaping) noexcept
        {
            const float q = (float) (1 << (bits - 1));
            auto& e = ditherError[c];
            float shaped = x;
            if (shaping == 1)
                shaped -= e[0];
            else if (shaping == 2)
                shaped -= 1.623f * e[0] - 0.982f * e[1] + 0.109f * e[2];
            const float noise = (random() + random()) * 0.5f;   // triangular, +-1 LSB
            const float y = std::round (shaped * q + noise) / q;
            e = { y - shaped, e[0], e[1] };
            return y;
        }

        float random() noexcept
        {
            seed = seed * 1664525u + 1013904223u;
            return (float) (seed >> 8) / (float) (1u << 23) - 1.0f;
        }

        void meterOutput (float l, float r, float input, float gain) noexcept
        {
            loudness.add (l, r);

            // True peak of the output: 4x interpolation
            float* x[] { &l, &r };
            for (size_t c = 0; c < 2; ++c)
            {
                const float* window = meterHistory[c].push (*x[c]);
                float peak = std::abs (*x[c]);
                for (const auto& phase : meterKernel)
                    peak = juce::jmax (peak, std::abs (dot (phase.data(), window, 16)));
                truePeakMax = juce::jmax (truePeakMax, peak);
                blockTruePeak[c] = juce::jmax (blockTruePeak[c], peak);
                rmsState[c] += rmsCoefficient * (*x[c] * *x[c] - rmsState[c]);
            }

            // History column
            column.input = juce::jmax (column.input, input);
            column.output = juce::jmax (column.output, juce::jmax (std::abs (l), std::abs (r)));
            column.reduction = juce::jmin (column.reduction, juce::Decibels::gainToDecibels (gain, -60.0f));
            if (++columnSamples >= columnLength)
            {
                const auto scope = fifo.write (1);
                if (scope.blockSize1 > 0)
                    columns[(size_t) scope.startIndex1] = column;
                column = {};
                columnSamples = 0;
            }
        }

        double rate = 48000.0;
        int maxBlock = 2048, latency = 0, coreLatency = 0, current = 0, delayPosition = 0, padPosition = 0;
        bool wasActive = true;
        std::array<Core, 4> cores;
        Core safety;
        std::array<std::unique_ptr<juce::dsp::Oversampling<float>>, 3> oversamplers;
        std::array<double, 4> pathLatency {};
        std::array<int, 4> padding {};
        juce::AudioBuffer<float> dryDelay, padDelay, work;
        std::array<float, 8192> columnGain {}, columnInput {};
        juce::SmoothedValue<float> inputGain { 1.0f }, unityGain { 1.0f };
        float fade = 1.0f, fadeStep = 0.001f, switchGain = 1.0f, switchStep = 0.003f, dcCoefficient = 0.0f;
        std::array<float, 2> dcState {};
        std::array<std::array<float, 3>, 2> ditherError {};
        juce::uint32 seed = 22222u;

        std::array<std::array<float, 16>, 3> meterKernel {};
        std::array<Window<16>, 2> meterHistory;
        float truePeakMax = 0.0f;
        Loudness loudness;
        std::atomic<bool> resetRequest { false };
        enum : size_t { truePeakMeter, momentaryMeter, shortTermMeter, integratedMeter, rangeMeter, truePeakLeft, rmsLeft = truePeakLeft + 2, numMeters = rmsLeft + 2 };
        std::array<std::atomic<float>, numMeters> meters { -100.0f, -100.0f, -100.0f, -100.0f, 0.0f, -100.0f, -100.0f, -100.0f, -100.0f };
        std::array<float, 2> blockTruePeak {}, rmsState {};
        float rmsCoefficient = 0.0f;
        std::atomic<float> reduction { 0.0f };

        static constexpr int numColumns = 2048;
        std::array<Column, numColumns> columns {};
        juce::AbstractFifo fifo { numColumns };
        Column column;
        int columnLength = 320, columnSamples = 0;
    };
}
