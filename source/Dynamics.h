#pragma once

#include <juce_dsp/juce_dsp.h>

//==============================================================================
// Dynamics in five bands: analysis (level distribution per band) and matching.
//
// The level of each band is followed by a detector (attack 5 ms, release by band).
// Its readings (100 per second) form a histogram per band, from which follow the
// centre level (average in dB; unlike the median it does not jump between loud and
// quiet sections) and the dynamic range (95th minus 10th percentile). Readings more
// than 20 dB below the average of the band are ignored: a band that is missing for a
// while (bass pause, break) does not pull the centre down.
//
// Matching: the level is mapped around the centre - where the input is more dynamic
// than the reference, the part above it is pushed down and the part below it raised
// (upward compression, guarded against pauses and decays); where it is flatter, the
// part below it is pushed down (expansion). Both channels get the same gain.
//==============================================================================
namespace Dynamics
{
    constexpr int numBands = 5;
    inline constexpr float crossovers[numBands - 1] { 120.0f, 600.0f, 1200.0f, 9000.0f };
    inline constexpr const char* bandNames[numBands]  { "SUB", "LOW MID", "MID", "HIGH MID", "AIR" };
    inline constexpr const char* bandRanges[numBands] { "0 - 120 Hz", "120 - 600 Hz", "600 Hz - 1.2 kHz",
                                                        "1.2 - 9 kHz", "9 - 22 kHz" };
    using Values = std::array<float, numBands>;

    constexpr float attackMs = 5.0f;
    inline constexpr float releaseMs[numBands] { 160.0f, 110.0f, 80.0f, 60.0f, 50.0f };
    constexpr double readingsPerSecond = 100.0;

    // Histogram of the band levels: 0.5 dB steps from -100 dBFS
    constexpr float histogramMin = -100.0f, histogramStep = 0.5f;
    constexpr int histogramSize = 212;
    using Histogram = std::array<float, histogramSize>;

    inline float binLevel (float bin)   { return histogramMin + (bin + 0.5f) * histogramStep; }

    struct Profile
    {
        std::array<Histogram, numBands> histogram {};   // weights (any scale)
        Values centre {}, range {};                     // dB: average level, 95th - 10th percentile
        Values upper {}, lower {};                      // dB: 95th - centre, centre - 10th percentile
        std::array<bool, numBands> active {};           // band has enough signal
        bool valid = false;

        // Median and range from the histograms
        void evaluate()
        {
            valid = false;
            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                const auto& h = histogram[b];
                double total = 0.0, energy = 0.0;
                for (int i = 0; i < histogramSize; ++i)
                {
                    total  += h[(size_t) i];
                    energy += h[(size_t) i] * std::pow (10.0, binLevel ((float) i) * 0.1);
                }

                active[b] = false;
                centre[b] = range[b] = upper[b] = lower[b] = 0.0f;
                if (total <= 0.0)
                    continue;

                // Gate: 20 dB below the energy average (and above -80 dBFS)
                const float gate = juce::jmax (-80.0f, (float) (10.0 * std::log10 (energy / total)) - 20.0f);
                const int first = juce::jlimit (0, histogramSize - 1, (int) ((gate - histogramMin) / histogramStep));

                double gated = 0.0, levelSum = 0.0;
                for (int i = first; i < histogramSize; ++i)
                {
                    gated    += h[(size_t) i];
                    levelSum += h[(size_t) i] * binLevel ((float) i);
                }
                if (gated <= total * 0.05)
                    continue;

                // Percentile with linear interpolation inside the bin
                const auto percentile = [&] (double p)
                {
                    const double target = p * gated;
                    double sum = 0.0;
                    for (int i = first; i < histogramSize; ++i)
                    {
                        const double w = h[(size_t) i];
                        if (sum + w >= target && w > 0.0)
                            return histogramMin + ((float) i + (float) ((target - sum) / w)) * histogramStep;
                        sum += w;
                    }
                    return binLevel ((float) (histogramSize - 1));
                };

                centre[b] = (float) (levelSum / gated);
                upper[b]  = juce::jmax (0.0f, percentile (0.95) - centre[b]);
                lower[b]  = juce::jmax (0.0f, centre[b] - percentile (0.10));
                range[b]  = upper[b] + lower[b];
                active[b] = true;
                valid = true;
            }
        }
    };

    //==========================================================================
    // Five bands from Linkwitz-Riley crossovers (4th order). With 'compensate' the lower
    // bands also pass the allpasses of the higher crossovers, so all bands are in phase
    // and add up to a flat response.
    class Splitter
    {
    public:
        void prepare (double sampleRate, bool shouldCompensate)
        {
            compensate = shouldCompensate;
            const juce::dsp::ProcessSpec spec { sampleRate, 1, 2 };
            for (int i = 0; i < numBands - 1; ++i)
            {
                split[(size_t) i].setCutoffFrequency (crossovers[i]);
                split[(size_t) i].prepare (spec);
            }

            // Band b gets the allpasses of crossovers b+1 ... (band 0: 3, band 1: 2, band 2: 1)
            size_t n = 0;
            for (int b = 0; b < numBands - 2; ++b)
                for (int c = b + 1; c < numBands - 1; ++c, ++n)
                {
                    allpass[n].setType (juce::dsp::LinkwitzRileyFilterType::allpass);
                    allpass[n].setCutoffFrequency (crossovers[c]);
                    allpass[n].prepare (spec);
                }
        }

        void reset()
        {
            for (auto& f : split)   f.reset();
            for (auto& f : allpass) f.reset();
        }

        void process (float l, float r, Values& bandL, Values& bandR) noexcept
        {
            float restL = l, restR = r;
            for (size_t c = 0; c < (size_t) numBands - 1; ++c)
            {
                float highL, highR;
                split[c].processSample (0, restL, bandL[c], highL);
                split[c].processSample (1, restR, bandR[c], highR);
                restL = highL;
                restR = highR;
            }
            bandL[(size_t) numBands - 1] = restL;
            bandR[(size_t) numBands - 1] = restR;

            if (compensate)
            {
                size_t n = 0;
                for (size_t b = 0; b < (size_t) numBands - 2; ++b)
                    for (size_t c = b + 1; c < (size_t) numBands - 1; ++c, ++n)
                    {
                        bandL[b] = allpass[n].processSample (0, bandL[b]);
                        bandR[b] = allpass[n].processSample (1, bandR[b]);
                    }
            }
        }

    private:
        std::array<juce::dsp::LinkwitzRileyFilter<float>, numBands - 1> split;
        std::array<juce::dsp::LinkwitzRileyFilter<float>, 6> allpass;
        bool compensate = false;
    };

    //==========================================================================
    // Level per band (power of both channels, attack/release)
    class Detector
    {
    public:
        void prepare (double sampleRate)
        {
            const auto coefficient = [sampleRate] (float ms) { return 1.0f - std::exp (-1.0f / (float) (sampleRate * ms * 0.001)); };
            attack = coefficient (attackMs);
            for (size_t b = 0; b < (size_t) numBands; ++b)
                release[b] = coefficient (releaseMs[b]);
            reset();
        }

        void reset()   { power.fill (0.0f); }

        void process (const Values& l, const Values& r) noexcept
        {
            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                const float p = 0.5f * (l[b] * l[b] + r[b] * r[b]);
                power[b] += (p > power[b] ? attack : release[b]) * (p - power[b]);
            }
        }

        float levelDb (size_t band) const noexcept   { return 10.0f * std::log10 (power[band] + 1.0e-12f); }
        float getPower (size_t band) const noexcept  { return power[band]; }

    private:
        float attack = 0.0f;
        Values release {}, power {};
    };

    //==========================================================================
    // Level distribution of a signal (reference, learned or live input, output)
    class Analyser
    {
    public:
        void prepare (double sampleRate)
        {
            splitter.prepare (sampleRate, false);
            detector.prepare (sampleRate);
            interval = juce::jmax (1, juce::roundToInt (sampleRate / readingsPerSecond));
            reset();
        }

        void reset()
        {
            splitter.reset();
            detector.reset();
            for (auto& h : histogram)
                h.fill (0.0);
            weight = 0.0;
            counter = 0;
        }

        // decay per reading: 1 = all readings count equally, < 1 = the newest count more
        void process (const float* l, const float* r, int numSamples, double decay = 1.0) noexcept
        {
            Values bl, br;
            for (int i = 0; i < numSamples; ++i)
            {
                splitter.process (l[i], r[i], bl, br);
                detector.process (bl, br);
                if (++counter >= interval)
                {
                    counter = 0;
                    Values levels;
                    for (size_t b = 0; b < (size_t) numBands; ++b)
                        levels[b] = detector.levelDb (b);
                    addLevels (levels, decay);
                }
            }
        }

        // At least this many seconds of signal so far
        bool hasData (double seconds = 1.0) const noexcept   { return weight >= seconds * readingsPerSecond * 0.5; }

        Profile getProfile() const
        {
            Profile p;
            for (size_t b = 0; b < (size_t) numBands; ++b)
                for (size_t i = 0; i < (size_t) histogramSize; ++i)
                    p.histogram[b][i] = (float) histogram[b][i];
            p.evaluate();
            return p;
        }

        // One reading (dB per band), e.g. from the detector of a Matcher
        void addLevels (const Values& levels, double decay = 1.0) noexcept
        {
            float loudest = -200.0f;
            for (auto v : levels)
                loudest = juce::jmax (loudest, v);

            if (loudest < -90.0f)   // silence (pause, stopped transport) is not counted
                return;

            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                auto& h = histogram[b];
                if (decay < 1.0)
                    for (auto& v : h)
                        v *= decay;

                const int bin = (int) ((levels[b] - histogramMin) / histogramStep);
                if (bin >= 0 && bin < histogramSize)
                    h[(size_t) bin] += 1.0;
            }
            weight = weight * decay + 1.0;
        }

    private:
        Splitter splitter;
        Detector detector;
        std::array<std::array<double, histogramSize>, numBands> histogram {};
        double weight = 0.0;
        int interval = 480, counter = 0;
    };

    //==========================================================================
    // The multiband dynamics stage in the audio path. The detector gets the input
    // before the lookahead delay, the bands are taken from the delayed signal, so
    // the gain is in place when a transient arrives.
    //
    // Gain per band (dB) = downward part (fast detector, at most -12 dB) + upward part
    // (level smoothed over 100 ms, at most +4 dB, rising at most 15 dB/s, falling 60 dB/s):
    //  - slope < 1 (input more dynamic, compression): down above the centre, up below it -
    //    but only for a band that is there (full down to 12 dB below the centre, none
    //    from 20 dB below: a missing band is never raised) and not while it is being cut
    //    off (its level falling faster than 12 ... 25 dB/s, e.g. the start of a pause),
    //  - slope > 1 (input flatter, expansion): up above the centre, down below it (fading
    //    out from 24 to 36 dB below).
    // The detector readings (100 per second) are passed on to the engine thread, which
    // builds the live level distribution of the signal at this point from them.
    class Matcher
    {
    public:
        void prepare (double sampleRate)
        {
            splitter.prepare (sampleRate, true);
            detectorSplitter.prepare (sampleRate, false);
            detector.prepare (sampleRate);
            smoothing = 1.0f - std::exp (-1.0f / (float) (sampleRate * 0.05));   // targets glide over 50 ms
            fadeStep  = 1.0f / (float) (sampleRate * 0.02);                     // in / out of the path: 20 ms
            readingInterval = juce::jmax (1, juce::roundToInt (sampleRate / readingsPerSecond));
            updatesPerSecond = (float) sampleRate / (float) gainInterval;
            smoothCoefficient = 1.0f - std::exp (-1.0f / (updatesPerSecond * 0.1f));
            upRiseStep = 15.0f / updatesPerSecond;
            upFallStep = 60.0f / updatesPerSecond;
            reset();
        }

        void reset()
        {
            splitter.reset();
            detectorSplitter.reset();
            detector.reset();
            slope.fill (1.0f);
            pivot.fill (-30.0f);
            gain.fill (1.0f);
            gainStep.fill (0.0f);
            smoothPower.fill (0.0f);
            previous.fill (-120.0f);
            fallRate.fill (0.0f);
            up.fill (0.0f);
            fade = 0.0f;
            counter = readingCounter = 0;
            readings.reset();
            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                targetSlope[b].store (1.0f, std::memory_order_relaxed);
                targetPivot[b].store (-30.0f, std::memory_order_relaxed);
                gainsDb[b].store (0.0f, std::memory_order_relaxed);
            }
        }

        // Engine thread: new targets (slope 1 everywhere = stage off)
        void setTargets (const Values& newSlope, const Values& newPivot) noexcept
        {
            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                targetSlope[b].store (newSlope[b], std::memory_order_relaxed);
                targetPivot[b].store (newPivot[b], std::memory_order_relaxed);
            }
        }

        // Audio thread: the input sample (lookahead) and the delayed sample to process
        void process (float inL, float inR, float& l, float& r) noexcept
        {
            Values bl, br;
            detectorSplitter.process (inL, inR, bl, br);
            detector.process (bl, br);

            if (--counter <= 0)
                updateGains();

            if (--readingCounter <= 0)
            {
                readingCounter = readingInterval;
                const auto scope = readings.write (1);   // engine thread too slow: reading dropped
                if (scope.blockSize1 > 0)
                    for (size_t b = 0; b < (size_t) numBands; ++b)
                        levelBuffer[(size_t) scope.startIndex1][b] = detector.levelDb (b);
            }

            splitter.process (l, r, bl, br);
            float wetL = 0.0f, wetR = 0.0f;
            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                gain[b] += gainStep[b];
                wetL += gain[b] * bl[b];
                wetR += gain[b] * br[b];
            }

            // Not needed (all slopes 1): the dry signal, without the crossover phase shift
            fade = juce::jlimit (0.0f, 1.0f, fade + (needed ? fadeStep : -fadeStep));
            l += fade * (wetL - l);
            r += fade * (wetR - r);
        }

        float getGainDb (size_t band) const noexcept   { return gainsDb[band].load (std::memory_order_relaxed); }

        // Engine thread: hands over the detector readings since the last call
        template <typename Fn>
        void readLevels (Fn&& use)
        {
            const auto scope = readings.read (readings.getNumReady());
            for (int i = 0; i < scope.blockSize1; ++i) use (levelBuffer[(size_t) (scope.startIndex1 + i)]);
            for (int i = 0; i < scope.blockSize2; ++i) use (levelBuffer[(size_t) (scope.startIndex2 + i)]);
        }

    private:
        static constexpr int gainInterval = 8;   // gains are calculated every 8 samples and ramped
        static constexpr float maxCutDb = 12.0f, maxBoostDb = 4.0f;
        static constexpr int numReadings = 1024;

        void updateGains() noexcept
        {
            counter = gainInterval;
            needed = false;
            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                const float targetS = targetSlope[b].load (std::memory_order_relaxed);
                const float glide = 1.0f - std::pow (1.0f - smoothing, (float) gainInterval);
                slope[b] += glide * (targetS - slope[b]);
                pivot[b] += glide * (targetPivot[b].load (std::memory_order_relaxed) - pivot[b]);
                needed = needed || std::abs (targetS - 1.0f) > 1.0e-3f || std::abs (slope[b] - 1.0f) > 1.0e-3f;

                const float level = detector.levelDb (b);
                const float d = juce::jlimit (-36.0f, 24.0f, level - pivot[b]);
                const float down = slope[b] < 1.0f ? (slope[b] - 1.0f) * juce::jmax (0.0f, d)
                                                   : (slope[b] - 1.0f) * juce::jmin (0.0f, d) * juce::jlimit (0.0f, 1.0f, (d + 36.0f) / 12.0f);

                smoothPower[b] += smoothCoefficient * (detector.getPower (b) - smoothPower[b]);
                const float smoothDb = 10.0f * std::log10 (smoothPower[b] + 1.0e-12f);
                fallRate[b] += smoothCoefficient * (juce::jmax (0.0f, previous[b] - smoothDb) * updatesPerSecond - fallRate[b]);
                previous[b] = smoothDb;

                const float ds = juce::jlimit (-36.0f, 24.0f, smoothDb - pivot[b]);
                const float upTarget = juce::jmin (maxBoostDb,
                    slope[b] < 1.0f ? (1.0f - slope[b]) * juce::jlimit (0.0f, 12.0f, -ds)
                                        * juce::jlimit (0.0f, 1.0f, (ds + 20.0f) / 8.0f)              // band is there
                                        * juce::jlimit (0.0f, 1.0f, (25.0f - fallRate[b]) / 13.0f)    // not cut off
                                    : (slope[b] - 1.0f) * juce::jmax (0.0f, ds));
                up[b] += juce::jlimit (-upFallStep, upRiseStep, upTarget - up[b]);

                const float gainDb = juce::jmax (-maxCutDb, down) + up[b];

                gainStep[b] = (juce::Decibels::decibelsToGain (gainDb) - gain[b]) / (float) gainInterval;
                gainsDb[b].store (gainDb, std::memory_order_relaxed);
            }
        }

        Splitter splitter, detectorSplitter;
        Detector detector;
        Values slope {}, pivot {}, gain {}, gainStep {}, smoothPower {}, previous {}, fallRate {}, up {};
        float updatesPerSecond = 6000.0f, smoothCoefficient = 0.0f, upRiseStep = 0.0f, upFallStep = 0.0f;
        std::array<std::atomic<float>, numBands> targetSlope, targetPivot, gainsDb;
        float smoothing = 0.0f, fade = 0.0f, fadeStep = 0.0f;
        bool needed = false;
        int counter = 0, readingCounter = 0, readingInterval = 480;
        juce::AbstractFifo readings { numReadings };
        std::array<Values, numReadings> levelBuffer {};
    };
}
