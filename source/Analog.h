#pragma once

#include "Dynamics.h"

//==============================================================================
// Analog heat: the colour of analog gear, in this order
//   1. tape transport: wow (slow) and flutter (fast) pitch movement - a modulated delay,
//   2. vinyl crackle: clicks and the odd pop, each at its own place in the stereo field,
//   3. (2x oversampled from here on)
//      tape saturation per band (the five bands of the dynamics), warm and asymmetric,
//      tube output stage: asymmetric (even harmonics),
//      transformer output stage: saturates mostly in the bass, like a real core.
// The saturation is stereo: the drive of each saturator is varied by a random value per
// sample, separately for left and right.
// All amounts 0: the input, delayed by the latency, unchanged (cross-faded over 20 ms).
//==============================================================================
namespace Analog
{
    struct Settings
    {
        Dynamics::Values tape {};   // 0 ... 1 per band
        float wow = 0.0f, flutter = 0.0f, crackle = 0.0f, tube = 0.0f, transformer = 0.0f;

        bool isActive() const noexcept
        {
            bool any = wow > 0.0f || flutter > 0.0f || crackle > 0.0f || tube > 0.0f || transformer > 0.0f;
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

            transportDelay = juce::jmax (8, juce::roundToInt (sampleRate * 0.001));   // 1 ms around which wow/flutter move
            latency = transportDelay + juce::roundToInt (oversampling.getLatencyInSamples());
            transport.setSize (2, juce::nextPowerOfTwo (transportDelay * 2 + 8));
            dry.setSize (2, latency + 1);
            input.setSize (2, maxBlockSize);

            fadeStep = 1.0f / (float) (sampleRate * 0.02);
            dcCoefficient = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * 5.0f / (float) (sampleRate * 2.0));
            coreCoefficient = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * 120.0f / (float) (sampleRate * 2.0));
            crackleHighpass = std::exp (-juce::MathConstants<float>::twoPi * 900.0f / (float) sampleRate);
            reset();
        }

        void reset()
        {
            oversampling.reset();
            splitter.reset();
            transport.clear();
            dry.clear();
            transportPosition = dryPosition = 0;
            wowPhase = flutterPhase = flutterPhase2 = 0.0;
            drift = driftTarget = flutterNoise = 0.0f;
            driftCounter = 0;
            crackleEnv = { 0.0f, 0.0f };
            crackleDecay = 0.0f;
            crackleState = { 0.0f, 0.0f };
            crackleLast = { 0.0f, 0.0f };
            dcIn = dcOut = core = { 0.0f, 0.0f };
            fade = 0.0f;
        }

        int getLatencySamples() const noexcept   { return latency; }

        // In place, numSamples <= maxBlockSize
        void process (float* left, float* right, int numSamples, const Settings& s) noexcept
        {
            // Dry path: the input delayed by the latency
            float* dryL = dry.getWritePointer (0);
            float* dryR = dry.getWritePointer (1);
            const int dryLength = dry.getNumSamples();

            const bool active = s.isActive();
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
                return;
            }

            float* inL = input.getWritePointer (0);
            float* inR = input.getWritePointer (1);
            for (int i = 0; i < numSamples; ++i)
            {
                inL[i] = left[i];
                inR[i] = right[i];
                transportAndCrackle (left[i], right[i], s);
            }

            // Saturation, 2x oversampled
            float* channels[] { left, right };
            juce::dsp::AudioBlock<float> block (channels, 2, (size_t) numSamples);
            auto up = oversampling.processSamplesUp (block);
            saturate (up.getChannelPointer (0), up.getChannelPointer (1), (int) up.getNumSamples(), s);
            oversampling.processSamplesDown (block);

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
        void transportAndCrackle (float& l, float& r, const Settings& s) noexcept
        {
            const int length = transport.getNumSamples();
            float* tl = transport.getWritePointer (0);
            float* tr = transport.getWritePointer (1);
            tl[transportPosition] = l;
            tr[transportPosition] = r;

            // Delay in samples: wow up to 0.25 % pitch at ~0.5 Hz with a slow random drift,
            // flutter up to 0.12 % at ~7 Hz plus 11 Hz and a little noise
            const double twoPi = juce::MathConstants<double>::twoPi;
            wowPhase      = std::fmod (wowPhase + twoPi * 0.55 / sampleRate, twoPi);
            flutterPhase  = std::fmod (flutterPhase + twoPi * 7.1 / sampleRate, twoPi);
            flutterPhase2 = std::fmod (flutterPhase2 + twoPi * 11.3 / sampleRate, twoPi);
            if (--driftCounter <= 0)
            {
                driftCounter = juce::roundToInt (sampleRate * 0.7);
                driftTarget = random();
            }
            drift += (driftTarget - drift) * (float) (2.0 / sampleRate);
            flutterNoise += (random() - flutterNoise) * (float) (60.0 / sampleRate);

            const float wowDepth = (float) (0.0025 * sampleRate / (twoPi * 0.55)), flutterDepth = (float) (0.0012 * sampleRate / (twoPi * 7.1));
            const float modulation = s.wow * wowDepth * (0.75f * (float) std::sin (wowPhase) + 0.25f * drift)
                                   + s.flutter * flutterDepth * ((float) std::sin (flutterPhase) + 0.4f * (float) std::sin (flutterPhase2) + 3.0f * flutterNoise);
            const float delay = (float) transportDelay + juce::jlimit (-(float) transportDelay + 2.0f, (float) transportDelay - 2.0f, modulation);

            // Cubic (Hermite) interpolation
            const int whole = (int) std::floor (delay);
            const float frac = delay - (float) whole;
            const auto at = [&] (const float* x, int back) { return x[(transportPosition - back + length) & (length - 1)]; };
            const auto hermite = [frac] (float xm1, float x0, float x1, float x2)
            {
                const float c1 = 0.5f * (x1 - xm1), c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
                return ((c3 * frac + c2) * frac + c1) * frac + x0;
            };
            l = hermite (at (tl, whole - 1), at (tl, whole), at (tl, whole + 1), at (tl, whole + 2));
            r = hermite (at (tr, whole - 1), at (tr, whole), at (tr, whole + 1), at (tr, whole + 2));
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
                l += crackleState[0];
                r += crackleState[1];
            }
        }

        // Tape per band, tube, transformer (oversampled)
        void saturate (float* l, float* r, int numSamples, const Settings& s) noexcept
        {
            Dynamics::Values tapeDrive {}, tapeSpread {};
            bool tapeOn = false;
            for (size_t b = 0; b < (size_t) Dynamics::numBands; ++b)
            {
                tapeDrive[b]  = juce::Decibels::decibelsToGain (s.tape[b] * 12.0f);   // up to +12 dB into the tape
                tapeSpread[b] = 0.06f * s.tape[b];                                   // random L/R drive variation
                tapeOn = tapeOn || s.tape[b] > 0.0f;
            }
            const float tubeDrive = 1.0f + 3.0f * s.tube, transformerDrive = 1.0f + 5.0f * s.transformer;

            // Warm tape curve: asymmetric (a little bias), unity gain for small signals
            constexpr float bias = 0.15f;
            const float tapeNorm = 1.0f / (1.0f - std::tanh (bias) * std::tanh (bias));
            const auto tape = [&] (float x, float drive) { return (std::tanh (drive * x + bias) - std::tanh (bias)) * tapeNorm / drive; };

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
                float x[] { l[i], r[i] };

                if (tapeOn)
                {
                    splitter.process (x[0], x[1], bl, br);
                    x[0] = x[1] = 0.0f;
                    for (size_t b = 0; b < (size_t) Dynamics::numBands; ++b)
                    {
                        if (s.tape[b] <= 0.0f)
                        {
                            x[0] += bl[b];
                            x[1] += br[b];
                            continue;
                        }
                        x[0] += tape (bl[b], tapeDrive[b] * (1.0f + tapeSpread[b] * random()));
                        x[1] += tape (br[b], tapeDrive[b] * (1.0f + tapeSpread[b] * random()));
                    }
                }

                for (size_t c = 0; c < 2; ++c)
                {
                    float y = x[c];
                    if (s.tube > 0.0f)
                        y = tube (y, tubeDrive * (1.0f + 0.05f * s.tube * random()));

                    if (s.transformer > 0.0f)
                    {
                        // The core saturates with the low frequencies (flux), the rest passes
                        core[c] += coreCoefficient * (y - core[c]);
                        const float drive = transformerDrive * (1.0f + 0.05f * s.transformer * random());
                        y += std::tanh (drive * core[c]) / drive - core[c];
                    }

                    // DC blocker (asymmetric curves)
                    dcOut[c] = y - dcIn[c] + (1.0f - dcCoefficient) * dcOut[c];
                    dcIn[c] = y;
                    io[c][i] = dcOut[c];
                }
            }
        }

        double sampleRate = 48000.0;
        juce::dsp::Oversampling<float> oversampling { 2, 1, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true };
        Dynamics::Splitter splitter;

        juce::AudioBuffer<float> transport, dry, input;
        int transportDelay = 48, transportPosition = 0, dryPosition = 0, latency = 0, driftCounter = 0;
        double wowPhase = 0.0, flutterPhase = 0.0, flutterPhase2 = 0.0;
        float drift = 0.0f, driftTarget = 0.0f, flutterNoise = 0.0f;

        std::array<float, 2> crackleEnv {}, crackleState {}, crackleLast {}, dcIn {}, dcOut {}, core {};
        float crackleDecay = 0.0f, crackleHighpass = 0.0f, dcCoefficient = 0.0f, coreCoefficient = 0.0f;
        float fade = 0.0f, fadeStep = 0.0f;
        uint32_t seed = 0x9e3779b9u;
    };
}
