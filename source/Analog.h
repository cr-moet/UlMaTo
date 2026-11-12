#pragma once

#include "Dynamics.h"
#include <complex>

//==============================================================================
// Analog heat: the colour of analog gear, in this order
//   0. DRIVE: the level into the whole chain; after the output stages it is taken back
//      again (automatic gain staging), so the drive only changes how hard everything works,
//   1. tape transport as described for the Abbey Road J37: wow is a slow movement of the
//      pitch (speed of the motor, a modulated delay), flutter a fast movement of the level
//      (the tape moving against the heads); each with depth and rate around the reference
//      of the machine,
//   2. vinyl crackle: clicks and the odd pop, each at its own place in the stereo field;
//      vinyl hum: mains hum (50 or 60 Hz) with the buzz of its harmonics,
//   3. (2x oversampled from here on)
//      tape saturation per band (the five bands of the dynamics), warm and asymmetric,
//      tube output stage: asymmetric (even harmonics),
//      transformer output stage: saturates mostly in the bass, like a real core.
// The saturation is stereo: the drive of each saturator is varied by a random value per
// sample, separately for left and right.
// Only the harmonics are added: from each saturated signal its share of the input (the
// fundamental, projected over a short window) is taken out again and the rest is added to
// the unchanged input. Automatic gain staging then keeps the overall level of the
// saturation section equal to its input (slowly, so it does not pump).
// All amounts 0: the input, delayed by the latency, unchanged (cross-faded over 20 ms).
//==============================================================================
namespace Analog
{
    struct Settings
    {
        Dynamics::Values tape {};   // 0 ... 1 per band
        float wowDepth = 0.0f, flutterDepth = 0.0f;           // 0 (off) ... 1 (full)
        float wowRate = 0.8f, flutterRate = 5.15f;            // Hz
        float crackle = 0.0f, hum = 0.0f, tube = 0.0f, transformer = 0.0f;
        float driveDb = 0.0f;
        bool hum60 = false;   // mains hum at 60 Hz (else 50 Hz)

        bool isActive() const noexcept
        {
            bool any = driveDb > 0.0f || wowDepth > 0.0f || flutterDepth > 0.0f || crackle > 0.0f || hum > 0.0f || tube > 0.0f || transformer > 0.0f;
            for (auto t : tape)
                any = any || t > 0.0f;
            return any;
        }
    };

    class Heat
    {
    public:
        void prepare (double newSampleRate, int maxBlockSize)
        {
            sampleRate = newSampleRate;
            oversampling.initProcessing ((size_t) maxBlockSize);
            splitter.prepare (sampleRate * 2.0, true);

            transportDelay = juce::jmax (64, juce::roundToInt (sampleRate * 0.02));   // 20 ms around which the wow moves
            latency = transportDelay + juce::roundToInt (oversampling.getLatencyInSamples());
            transport.setSize (2, juce::nextPowerOfTwo (transportDelay * 2 + 8));
            dry.setSize (2, latency + 1);
            input.setSize (2, maxBlockSize);

            fadeStep = 1.0f / (float) (sampleRate * 0.02);
            dcCoefficient = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * 5.0f / (float) (sampleRate * 2.0));
            coreCoefficient = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * 120.0f / (float) (sampleRate * 2.0));

            // Windows for the fundamental: longer in the bass (several periods)
            const auto window = [this] (float seconds) { return 1.0f - std::exp (-1.0f / (float) (sampleRate * 2.0 * seconds)); };
            static constexpr float bandSeconds[Dynamics::numBands] { 0.06f, 0.02f, 0.01f, 0.005f, 0.003f };
            for (size_t b = 0; b < (size_t) Dynamics::numBands; ++b)
                bandCoefficient[b] = window (bandSeconds[b]);
            stageCoefficient  = window (0.04f);
            levelCoefficient  = window (0.3f);
            makeUpCoefficient = window (0.5f);
            crackleHighpass = std::exp (-juce::MathConstants<float>::twoPi * 900.0f / (float) sampleRate);
            vuCoefficient = 1.0f - std::exp (-1.0f / (float) (sampleRate * 0.065));   // the needle adds the rest of the 300 ms
            driveGain.reset (sampleRate, 0.05);
            reset();
        }

        void reset()
        {
            oversampling.reset();
            splitter.reset();
            transport.clear();
            dry.clear();
            transportPosition = dryPosition = 0;
            wowPhase = flutterPhase = 0.0;
            drift = driftTarget = flutterNoise = rateJitter = 0.0f;
            wowAmount = flutterAmount = 0.0f;
            driftCounter = 0;
            crackleEnv = { 0.0f, 0.0f };
            crackleDecay = 0.0f;
            crackleState = { 0.0f, 0.0f };
            crackleLast = { 0.0f, 0.0f };
            dcIn = dcOut = core = powerIn = powerOut = { 0.0f, 0.0f };
            makeUp = { 1.0f, 1.0f };
            for (auto& channel : tapeHarmonics)
                channel.fill ({});
            tubeHarmonics.fill ({});
            transformerHarmonics.fill ({});
            fade = 0.0f;
            humPhase = 0.0;
            humAmount = humWander = 0.0f;
            vuState = {};
            driveGain.setCurrentAndTargetValue (driveGain.getTargetValue());
        }

        int getLatencySamples() const noexcept   { return latency; }

        // VU level of the output (average of the rectified signal, 300 ms), dBFS per channel
        float getVuDb (int channel) const noexcept   { return vu[(size_t) channel].load (std::memory_order_relaxed); }

        // In place, numSamples <= maxBlockSize
        void process (float* left, float* right, int numSamples, const Settings& s) noexcept
        {
            // Dry path: the input delayed by the latency
            float* dryL = dry.getWritePointer (0);
            float* dryR = dry.getWritePointer (1);
            const int dryLength = dry.getNumSamples();

            const bool active = s.isActive();
            const auto measure = [&]
            {
                for (size_t c = 0; c < 2; ++c)
                {
                    const float* x = c == 0 ? left : right;
                    for (int i = 0; i < numSamples; ++i)
                        vuState[c] += vuCoefficient * (std::abs (x[i]) - vuState[c]);
                    // the average of a rectified sine is 2 / pi of its peak: times pi / (2 sqrt 2) it reads as its RMS
                    vu[c].store (juce::Decibels::gainToDecibels (vuState[c] * 1.1107f, -100.0f), std::memory_order_relaxed);
                }
            };
            if (! active && fade <= 0.0f)
            {
                // Off: only the delay (the stages keep their state from silence)
                for (int i = 0; i < numSamples; ++i)
                {
                    const int read = (dryPosition + 1) % dryLength;
                    dryL[dryPosition] = left[i];
                    dryR[dryPosition] = right[i];
                    left[i]  = dryL[read];
                    right[i] = dryR[read];
                    dryPosition = read;
                }
                measure();
                return;
            }

            float* inL = input.getWritePointer (0);
            float* inR = input.getWritePointer (1);
            driveGain.setTargetValue (juce::Decibels::decibelsToGain (s.driveDb));
            for (int i = 0; i < numSamples; ++i)
            {
                inL[i] = left[i];
                inR[i] = right[i];
                driveNow[(size_t) i] = driveGain.getNextValue();
                left[i]  *= driveNow[(size_t) i];
                right[i] *= driveNow[(size_t) i];
                transportAndCrackle (left[i], right[i], s, driveNow[(size_t) i]);
            }

            // Saturation, 2x oversampled
            float* channels[] { left, right };
            juce::dsp::AudioBlock<float> block (channels, 2, (size_t) numSamples);
            auto up = oversampling.processSamplesUp (block);
            saturate (up.getChannelPointer (0), up.getChannelPointer (1), (int) up.getNumSamples(), s);
            oversampling.processSamplesDown (block);
            for (int i = 0; i < numSamples; ++i)   // gain staging: the drive taken back after the output stages
            {
                left[i]  /= driveNow[(size_t) i];
                right[i] /= driveNow[(size_t) i];
            }

            // Cross-fade with the dry signal (both have the same latency)
            for (int i = 0; i < numSamples; ++i)
            {
                fade = juce::jlimit (0.0f, 1.0f, fade + (active ? fadeStep : -fadeStep));
                dryL[dryPosition] = inL[i];
                dryR[dryPosition] = inR[i];
                const int read = (dryPosition + 1) % dryLength;
                left[i]  = dryL[read] + fade * (left[i]  - dryL[read]);
                right[i] = dryR[read] + fade * (right[i] - dryR[read]);
                dryPosition = read;
            }
            measure();
        }

    private:
        // Uniform random value -1 ... 1 (xorshift, cheap enough for every sample)
        float random() noexcept
        {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            return (float) (seed & 0xffffff) / (float) 0x7fffff - 1.0f;
        }

        // Wow / flutter (one transport for both channels) and vinyl crackle, at the base rate
        void transportAndCrackle (float& l, float& r, const Settings& s, float drive) noexcept
        {
            const int length = transport.getNumSamples();
            float* tl = transport.getWritePointer (0);
            float* tr = transport.getWritePointer (1);
            tl[transportPosition] = l;
            tr[transportPosition] = r;

            // Wow: the pitch moves at the wow rate (with its second harmonic, as from an
            // eccentric capstan, and a slow random drift); full depth swings the pitch by 8 %
            // (the reference machine, ~0.05 %, is near 10 %) - the delay swing that gives this
            // is fs * deviation / (2 pi rate)
            const double twoPi = juce::MathConstants<double>::twoPi;
            if (--driftCounter <= 0)
            {
                driftCounter = juce::roundToInt (sampleRate * 0.7);
                driftTarget = random();
            }
            drift += (driftTarget - drift) * (float) (2.0 / sampleRate);
            rateJitter += (random() - rateJitter) * (float) (3.0 / sampleRate);   // the transport never runs perfectly even
            flutterNoise += (random() - flutterNoise) * (float) (40.0 / sampleRate);
            wowPhase     = std::fmod (wowPhase + twoPi * s.wowRate * (1.0 + 0.03 * rateJitter) / sampleRate, twoPi);
            flutterPhase = std::fmod (flutterPhase + twoPi * s.flutterRate * (1.0 + 0.06 * rateJitter) / sampleRate, twoPi);

            const float smoothing = (float) (30.0 / sampleRate);   // depth changes glide (no clicks)
            wowAmount     += smoothing * (s.wowDepth - wowAmount);
            flutterAmount += smoothing * (s.flutterDepth - flutterAmount);

            const float swing = juce::jmin ((float) (0.08 * sampleRate / (twoPi * s.wowRate)) * wowAmount, (float) transportDelay - 3.0f);
            const float wow = (float) wowPhase, flutter = (float) flutterPhase;
            const float modulation = swing * (0.75f * std::sin (wow) + 0.12f * std::sin (2.0f * wow + 0.7f) + 0.2f * drift);

            // Flutter: the level moves at the flutter rate; full depth by about 17 dB peak to
            // peak (the reference machine, ~0.1 dB, is near 12 %)
            const float flutterGain = 1.0f + 0.75f * flutterAmount
                                    * (0.8f * std::sin (flutter) + 0.15f * std::sin (2.0f * flutter + 1.3f) + 0.25f * flutterNoise);
            const float delay = (float) transportDelay + modulation;

            // Cubic (Hermite) interpolation
            const int whole = (int) std::floor (delay);
            const float frac = delay - (float) whole;
            const auto at = [&] (const float* x, int back) { return x[(transportPosition - back + length) & (length - 1)]; };
            const auto hermite = [frac] (float xm1, float x0, float x1, float x2)
            {
                const float c1 = 0.5f * (x1 - xm1), c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
                return ((c3 * frac + c2) * frac + c1) * frac + x0;
            };
            l = flutterGain * hermite (at (tl, whole - 1), at (tl, whole), at (tl, whole + 1), at (tl, whole + 2));
            r = flutterGain * hermite (at (tr, whole - 1), at (tr, whole), at (tr, whole + 1), at (tr, whole + 2));
            transportPosition = (transportPosition + 1) & (length - 1);

            // Vinyl crackle: up to 30 small clicks and 1.5 pops per second, each a short,
            // bright noise burst at a random place in the stereo field
            if (s.crackle > 0.0f)
            {
                const float chance = (float) (s.crackle * 30.0 / sampleRate), popChance = (float) (s.crackle * 1.5 / sampleRate);
                const float dice = 0.5f * (random() + 1.0f);
                if (dice < chance + popChance)
                {
                    const bool pop = dice < popChance;
                    const float amplitude = s.crackle * (pop ? 0.045f : 0.008f) * (0.3f + 0.35f * (random() + 1.0f));
                    const float pan = 0.5f * (random() + 1.0f);
                    crackleEnv = { amplitude * std::sqrt (1.0f - pan), amplitude * std::sqrt (pan) };
                    crackleDecay = std::exp (-1.0f / (float) (sampleRate * (pop ? 0.0012 : 0.00025)));
                }
                for (size_t c = 0; c < 2; ++c)
                {
                    const float noise = crackleEnv[c] * random();
                    crackleEnv[c] *= crackleDecay;
                    crackleState[c] = crackleHighpass * (crackleState[c] + noise - crackleLast[c]);   // one-pole highpass
                    crackleLast[c] = noise;
                }
                l += crackleState[0] * drive;   // times the drive: after the gain staging it is as loud as set
                r += crackleState[1] * drive;
            }

            // Vinyl hum: the mains frequency with its harmonics (the buzz of the rectifier),
            // full: -30 dBFS for the fundamental; slowly wandering a little in level
            humAmount += (float) (20.0 / sampleRate) * (s.hum - humAmount);
            if (humAmount > 1.0e-5f)
            {
                humPhase = std::fmod (humPhase + juce::MathConstants<double>::twoPi * (s.hum60 ? 60.0 : 50.0) / sampleRate, juce::MathConstants<double>::twoPi);
                humWander += (random() - humWander) * (float) (0.5 / sampleRate);
                // harmonic k: sin ((k + 1) phase + 0.4 k) - the powers of e^(i phase) give all of them
                static constexpr float harmonics[] { 1.0f, 0.45f, 0.3f, 0.12f, 0.08f, 0.05f, 0.03f };
                static const auto offsets = []
                {
                    std::array<std::complex<float>, 7> o;
                    for (size_t k = 0; k < o.size(); ++k)
                        o[k] = std::polar (1.0f, 0.4f * (float) k);
                    return o;
                }();
                const std::complex<float> step = std::polar (1.0f, (float) humPhase);
                std::complex<float> power = step;
                float h = 0.0f;
                for (size_t k = 0; k < 7; ++k, power *= step)
                    h += harmonics[k] * (power * offsets[k]).imag();
                h *= 0.0316f * humAmount * humAmount * (1.0f + 0.15f * humWander) * drive;
                l += h;
                r += h;
            }
        }

        // Harmonics of one saturator: its output minus its share of the input (the fundamental,
        // projected with exponential averages of x*y and x*x over a short window)
        struct Harmonics
        {
            float xy = 0.0f, xx = 0.0f;

            float operator() (float x, float saturated, float coefficient) noexcept
            {
                xy += coefficient * (x * saturated - xy);
                xx += coefficient * (x * x - xx);
                const float share = xx > 1.0e-12f ? xy / xx : 1.0f;
                return saturated - share * x;
            }
        };

        // Tape per band, tube, transformer (oversampled); each adds only its harmonics
        void saturate (float* l, float* r, int numSamples, const Settings& s) noexcept
        {
            // Full scale of the knobs: SUB 1/10, LOW MID 1/2 of the other bands
            static constexpr float tapeScale[Dynamics::numBands] { 0.1f, 0.5f, 1.0f, 1.0f, 1.0f };
            Dynamics::Values tapeDrive {}, tapeSpread {};
            bool tapeOn = false;
            for (size_t b = 0; b < (size_t) Dynamics::numBands; ++b)
            {
                const float amount = s.tape[b] * tapeScale[b];
                tapeDrive[b]  = juce::Decibels::decibelsToGain (amount * 24.0f);   // up to +24 dB into the tape (a band only carries part of the level)
                tapeSpread[b] = 0.06f * amount;                                   // random L/R drive variation
                tapeOn = tapeOn || s.tape[b] > 0.0f;
            }
            const float tubeAmount = 0.024f * s.tube, transformerAmount = 0.024f * s.transformer;
            const float tubeDrive = 1.0f + 3.0f * tubeAmount, transformerDrive = 1.0f + 5.0f * transformerAmount;
            const bool any = tapeOn || s.tube > 0.0f || s.transformer > 0.0f;

            // Warm tape curve: asymmetric (a little bias), unity gain for small signals
            constexpr float bias = 0.15f;
            const float tanhBias = std::tanh (bias), tapeNorm = 1.0f / (1.0f - tanhBias * tanhBias);

            // Tube: softer on the negative side -> even harmonics
            const auto tube = [] (float x, float drive)
            {
                const float z = drive * x;
                return (z >= 0.0f ? std::tanh (z) : std::tanh (0.7f * z) / 0.7f) / drive;
            };

            float* io[] { l, r };
            Dynamics::Values bl, br;
            for (int i = 0; i < numSamples; ++i)
            {
                const float in[] { l[i], r[i] };
                float x[] { in[0], in[1] };

                if (tapeOn)
                {
                    // All band curves in one plain loop, so the compiler can vectorise the tanh
                    splitter.process (x[0], x[1], bl, br);
                    const Dynamics::Values* bands[] { &bl, &br };
                    constexpr size_t nb = (size_t) Dynamics::numBands;
                    std::array<float, 2 * nb> drive {}, curve {};
                    for (size_t c = 0; c < 2; ++c)
                        for (size_t b = 0; b < nb; ++b)
                            if (s.tape[b] > 0.0f)
                                drive[c * nb + b] = tapeDrive[b] * (1.0f + tapeSpread[b] * random());
                    for (size_t k = 0; k < curve.size(); ++k)
                        curve[k] = std::tanh (drive[k] * (*bands[k / nb])[k % nb] + bias);
                    for (size_t c = 0; c < 2; ++c)
                        for (size_t b = 0; b < (size_t) Dynamics::numBands; ++b)
                            if (s.tape[b] > 0.0f)
                            {
                                const size_t k = c * nb + b;
                                x[c] += tapeHarmonics[c][b] ((*bands[c])[b], (curve[k] - tanhBias) * tapeNorm / drive[k], bandCoefficient[b]);
                            }
                }

                for (size_t c = 0; c < 2; ++c)
                {
                    float y = x[c];
                    if (s.tube > 0.0f)
                        y += tubeHarmonics[c] (y, tube (y, tubeDrive * (1.0f + 0.05f * tubeAmount * random())), stageCoefficient);

                    if (s.transformer > 0.0f)
                    {
                        // The core saturates with the low frequencies (flux), the rest passes
                        core[c] += coreCoefficient * (y - core[c]);
                        const float drive = transformerDrive * (1.0f + 0.05f * transformerAmount * random());
                        y += transformerHarmonics[c] (core[c], std::tanh (drive * core[c]) / drive, stageCoefficient);
                    }

                    // DC blocker (asymmetric curves)
                    dcOut[c] = y - dcIn[c] + (1.0f - dcCoefficient) * dcOut[c];
                    dcIn[c] = y;
                    y = dcOut[c];

                    // Gain staging: the level of the section follows its input (slowly, +-3 dB)
                    powerIn[c]  += levelCoefficient * (in[c] * in[c] - powerIn[c]);
                    powerOut[c] += levelCoefficient * (y * y - powerOut[c]);
                    const float target = any && powerOut[c] > 1.0e-10f
                                             ? juce::jlimit (0.7f, 1.4f, std::sqrt (powerIn[c] / powerOut[c])) : 1.0f;
                    makeUp[c] += makeUpCoefficient * (target - makeUp[c]);
                    io[c][i] = y * makeUp[c];
                }
            }
        }

        double sampleRate = 48000.0;
        juce::dsp::Oversampling<float> oversampling { 2, 1, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true };
        Dynamics::Splitter splitter;

        juce::AudioBuffer<float> transport, dry, input;
        int transportDelay = 48, transportPosition = 0, dryPosition = 0, latency = 0, driftCounter = 0;
        double wowPhase = 0.0, flutterPhase = 0.0;
        float drift = 0.0f, driftTarget = 0.0f, flutterNoise = 0.0f, rateJitter = 0.0f, wowAmount = 0.0f, flutterAmount = 0.0f;

        std::array<float, 2> crackleEnv {}, crackleState {}, crackleLast {}, dcIn {}, dcOut {}, core {}, powerIn {}, powerOut {}, makeUp {};
        float crackleDecay = 0.0f, crackleHighpass = 0.0f, dcCoefficient = 0.0f, coreCoefficient = 0.0f;
        double humPhase = 0.0;
        float humAmount = 0.0f, humWander = 0.0f, vuCoefficient = 0.0f;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> driveGain { 1.0f };
        std::array<float, 8192> driveNow {};
        std::array<float, 2> vuState {};
        std::array<std::atomic<float>, 2> vu { -100.0f, -100.0f };
        std::array<std::array<Harmonics, Dynamics::numBands>, 2> tapeHarmonics;
        std::array<Harmonics, 2> tubeHarmonics, transformerHarmonics;
        Dynamics::Values bandCoefficient {};
        float stageCoefficient = 0.0f, levelCoefficient = 0.0f, makeUpCoefficient = 0.0f;
        float fade = 0.0f, fadeStep = 0.0f;
        uint32_t seed = 0x9e3779b9u;
    };
}
