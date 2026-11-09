#pragma once

#include "MatrixConvolver.h"
#include <complex>
#include <array>

//==============================================================================
// PhaseQ and graphic EQ: settings, their curves and the FIR stage that runs them.
//
// PhaseQ   - changes only the phase (the magnitude stays exactly flat): a global
//            rotation plus six nodes, each a bump of phase over frequency with its own
//            width; applied to both channels, mid, side, left or right.
// Graphic  - six bands (low shelf, four bells, high shelf), linear phase: only the
// EQ         magnitude changes.
//==============================================================================
namespace Tone
{
    //==========================================================================
    // Settings shared between the audio thread (writes) and the engine thread (reads)
    template <typename T>
    class Shared
    {
    public:
        void set (const T& v) noexcept
        {
            const juce::SpinLock::ScopedTryLockType lock (mutex);   // never waits on the audio thread
            if (lock.isLocked())
                value = v;
        }

        T get() const noexcept
        {
            const juce::SpinLock::ScopedLockType lock (mutex);
            return value;
        }

    private:
        mutable juce::SpinLock mutex;
        T value {};
    };

    //==========================================================================
    constexpr int numBands = 6;
    using Values = std::array<float, numBands>;

    struct EqSettings
    {
        Values freq {}, gain {}, q {};   // Hz, dB, Q
        bool operator== (const EqSettings& o) const noexcept   { return freq == o.freq && gain == o.gain && q == o.q; }
        bool operator!= (const EqSettings& o) const noexcept   { return ! (*this == o); }
    };

    enum class EqType { lowShelf, bell, highShelf };
    inline constexpr EqType eqTypes[numBands] { EqType::lowShelf, EqType::bell, EqType::bell, EqType::bell, EqType::bell, EqType::highShelf };
    inline constexpr float eqDefaultFreq[numBands] { 60.0f, 200.0f, 600.0f, 2000.0f, 5000.0f, 12000.0f };

    // Magnitude of one band at f (analog prototypes: no cramping near Nyquist)
    inline float bandMagnitude (EqType type, float f0, float gainDb, float q, float f)
    {
        if (std::abs (gainDb) < 1.0e-3f)
            return 1.0f;

        const float a = std::pow (10.0f, gainDb / 40.0f), sq = std::sqrt (a);
        const std::complex<float> s (0.0f, f / f0), s2 = s * s;
        std::complex<float> num, den;
        switch (type)
        {
            case EqType::bell:      num = s2 + s * (a / q) + 1.0f;               den = s2 + s / (a * q) + 1.0f;          break;
            case EqType::lowShelf:  num = a * (s2 + s * (sq / q) + a);           den = a * s2 + s * (sq / q) + 1.0f;     break;
            case EqType::highShelf: num = a * (a * s2 + s * (sq / q) + 1.0f);    den = s2 + s * (sq / q) + a;            break;
        }
        return std::abs (num / den);
    }

    inline float eqMagnitude (const EqSettings& s, float f)
    {
        float m = 1.0f;
        for (size_t b = 0; b < (size_t) numBands; ++b)
            m *= bandMagnitude (eqTypes[b], s.freq[b], s.gain[b], s.q[b], f);
        return m;
    }

    //==========================================================================
    enum PhaseMode { phaseStereo, phaseMid, phaseSide, phaseLeft, phaseRight };
    inline const juce::StringArray phaseModeNames { "STEREO", "MID", "SIDE", "LEFT", "RIGHT" };
    inline constexpr float phaseDefaultFreq[numBands] { 50.0f, 120.0f, 300.0f, 800.0f, 2500.0f, 7000.0f };

    struct PhaseSettings
    {
        Values freq {}, amount {}, width {};   // Hz, degrees, octaves
        float rotation = 0.0f;                 // degrees
        int mode = phaseStereo;
        bool operator== (const PhaseSettings& o) const noexcept
        {
            return freq == o.freq && amount == o.amount && width == o.width && rotation == o.rotation && mode == o.mode;
        }
        bool operator!= (const PhaseSettings& o) const noexcept   { return ! (*this == o); }
    };

    // Phase (radians) at f: rotation (faded in above 10 ... 30 Hz, DC must stay real) plus
    // the nodes (a Gaussian bump over log frequency each), all faded out towards Nyquist
    inline float phaseAt (const PhaseSettings& s, float f)
    {
        const auto ramp = [] (float x, float from, float to) { return juce::jlimit (0.0f, 1.0f, (x - from) / (to - from)); };
        const float octave = std::log2 (juce::jmax (1.0f, f));
        float degrees = s.rotation * ramp (octave, std::log2 (10.0f), std::log2 (30.0f));
        for (size_t n = 0; n < (size_t) numBands; ++n)
        {
            const float d = (octave - std::log2 (s.freq[n])) / juce::jmax (0.05f, s.width[n] * 0.5f);
            degrees += s.amount[n] * std::exp (-0.5f * d * d);
        }
        return juce::degreesToRadians (degrees) * (1.0f - ramp (f, 18000.0f, 21000.0f));
    }

    //==========================================================================
    // A 2x2 FIR matrix stage (see MatrixConvolver) designed from a frequency response;
    // latency: half the filter length. New filters are designed on the engine thread
    // and cross-faded in by the audio thread.
    class FirStage
    {
    public:
        struct Response { std::complex<float> a { 1.0f }, b {}, c {}, d { 1.0f }; };

        void prepare (int newBlockSize, int newLength)
        {
            blockSize = newBlockSize;
            length = newLength;
            fft = std::make_unique<juce::dsp::FFT> (juce::roundToInt (std::log2 (length)));
            partitionFft = std::make_unique<juce::dsp::FFT> (juce::roundToInt (std::log2 (2 * blockSize)));
            for (auto& d : data)
                d.assign ((size_t) length * 2, 0.0f);
            ir.assign ((size_t) length, 0.0f);

            // Tukey window: flat in the middle, so phase filters keep their shape
            window.resize ((size_t) length);
            const int taper = length / 8;
            for (int i = 0; i < length; ++i)
            {
                const int edge = juce::jmin (i, length - 1 - i);
                window[(size_t) i] = edge >= taper ? 1.0f
                                                   : 0.5f * (1.0f - std::cos (juce::MathConstants<float>::pi * (float) edge / (float) taper));
            }

            convolver.prepare (blockSize, length);
            for (auto& f : filters)
                design (48000.0, [] (float) { return Response {}; }, f);
            active = 0;
            pending = false;
        }

        int getLatencySamples() const noexcept   { return length / 2; }

        // Engine thread: new filters from response (frequency); false while the audio
        // thread has not taken the previous ones yet (try again later)
        template <typename Fn>
        bool update (double sampleRate, Fn&& response)
        {
            if (pending.load (std::memory_order_acquire))
                return false;
            design (sampleRate, response, filters[(size_t) (1 - active.load (std::memory_order_relaxed))]);
            pending.store (true, std::memory_order_release);
            return true;
        }

        // Audio thread: one block in place
        void process (float* left, float* right) noexcept
        {
            const int current = active.load (std::memory_order_relaxed);
            const bool change = pending.load (std::memory_order_acquire);
            convolver.process (left, right, filters[(size_t) current], change ? &filters[(size_t) (1 - current)] : nullptr);
            if (change)
            {
                active.store (1 - current, std::memory_order_relaxed);
                pending.store (false, std::memory_order_release);
            }
        }

    private:
        template <typename Fn>
        void design (double sampleRate, Fn&& response, MatrixConvolver::Filters& f)
        {
            const int n = length;
            for (auto& d : data)
                std::fill (d.begin(), d.end(), 0.0f);

            for (int k = 0; k <= n / 2; ++k)
            {
                Response r = response ((float) ((double) k * sampleRate / n));
                if (k == 0 || k == n / 2)   // DC and Nyquist must stay real
                    r = { r.a.real(), r.b.real(), r.c.real(), r.d.real() };
                const std::complex<float> values[] { r.a, r.b, r.c, r.d };
                for (size_t i = 0; i < 4; ++i)
                {
                    data[i][(size_t) k * 2]     = values[i].real();
                    data[i][(size_t) k * 2 + 1] = values[i].imag();
                }
            }

            std::vector<float>* targets[] { &f.a, &f.b, &f.c, &f.d };
            for (size_t i = 0; i < 4; ++i)
            {
                fft->performRealOnlyInverseTransform (data[i].data());
                for (int j = 0; j < n; ++j)   // centred: latency n / 2
                    ir[(size_t) j] = data[i][(size_t) ((j + n / 2) % n)] * window[(size_t) j];
                MatrixConvolver::makeSpectra (ir.data(), n, blockSize, *partitionFft, scratch, *targets[i]);
            }
        }

        int blockSize = 512, length = 4096;
        std::unique_ptr<juce::dsp::FFT> fft, partitionFft;
        std::array<std::vector<float>, 4> data;
        std::vector<float> ir, window, scratch;
        MatrixConvolver convolver;
        std::array<MatrixConvolver::Filters, 2> filters;
        std::atomic<int> active { 0 };
        std::atomic<bool> pending { false };
    };

    // Responses for the stages
    inline FirStage::Response eqResponse (const EqSettings& s, float f)
    {
        const float m = eqMagnitude (s, f);
        return { m, 0.0f, 0.0f, m };
    }

    inline FirStage::Response phaseResponse (const PhaseSettings& s, float f)
    {
        const auto h = std::polar (1.0f, phaseAt (s, f));
        const std::complex<float> one (1.0f);
        switch (s.mode)
        {
            case phaseMid:   return { 0.5f * (h + one), 0.5f * (h - one), 0.5f * (h - one), 0.5f * (h + one) };
            case phaseSide:  return { 0.5f * (one + h), 0.5f * (one - h), 0.5f * (one - h), 0.5f * (one + h) };
            case phaseLeft:  return { h, 0.0f, 0.0f, one };
            case phaseRight: return { one, 0.0f, 0.0f, h };
            default:         return { h, 0.0f, 0.0f, h };
        }
    }
}
