#include "MatchEngine.h"

namespace
{
    // Correction per band (dB) that turns the live tonal balance into the reference.
    // Both profiles are taken relative to their own average mid level, so only the
    // balance is matched, not the loudness. Mid and side use the same reference level,
    // so the side/mid ratio (stereo width, phase correlation) is matched as well.
    void calculateCorrection (const Spectrum::Profile& ref, const Spectrum::Profile& live,
                              Spectrum::Curve& mid, Spectrum::Curve& side)
    {
        using namespace Spectrum;
        constexpr float floorDb = -60.0f;   // below this (relative) a band is treated as empty

        const float refLevel  = ref.referenceLevelDb();
        const float liveLevel = live.referenceLevelDb();

        Curve rawMid {}, rawSide {};
        for (size_t b = 0; b < (size_t) numBands; ++b)
        {
            const auto diff = [&] (float refPower, float livePower)
            {
                const float r = toDb (refPower) - refLevel, l = toDb (livePower) - liveLevel;
                // Empty bands are not corrected: boosting silence or noise does not help
                return r > floorDb && l > floorDb ? r - l : 0.0f;
            };
            rawMid[b]  = diff (ref.mid[b],  live.mid[b]);
            rawSide[b] = diff (ref.side[b], live.side[b]);
        }

        // Smoothing over 1/3 octave (+-2 bands), so the EQ has no narrow peaks or notches
        constexpr int radius = 2;
        for (int b = 0; b < numBands; ++b)
        {
            float m = 0.0f, s = 0.0f;
            int count = 0;
            for (int i = juce::jmax (0, b - radius); i <= juce::jmin (numBands - 1, b + radius); ++i, ++count)
            {
                m += rawMid[(size_t) i];
                s += rawSide[(size_t) i];
            }
            mid[(size_t) b]  = juce::jlimit (-MatchEngine::maxCorrectionDb, MatchEngine::maxCorrectionDb, m / (float) count);
            side[(size_t) b] = juce::jlimit (-MatchEngine::maxCorrectionDb, MatchEngine::maxCorrectionDb, s / (float) count);
        }
    }

    float maxDifference (const Spectrum::Curve& a, const Spectrum::Curve& b)
    {
        float d = 0.0f;
        for (size_t i = 0; i < a.size(); ++i)
            d = juce::jmax (d, std::abs (a[i] - b[i]));
        return d;
    }
}

//==============================================================================
MatchEngine::MatchEngine() : juce::Thread ("UlMaTo match engine") {}

MatchEngine::~MatchEngine()
{
    stopThread (4000);
}

void MatchEngine::prepare (double newSampleRate, int maxBlockSize)
{
    stopThread (4000);

    sampleRate = newSampleRate;

    // Filter length: about 85 ms (frequency resolution ~12 Hz), power of two
    firLength = juce::nextPowerOfTwo ((int) (sampleRate * 0.085));
    designFft = std::make_unique<juce::dsp::FFT> (juce::roundToInt (std::log2 (firLength)));
    designData.assign ((size_t) firLength * 2, 0.0f);
    designWindow.resize ((size_t) firLength);
    juce::dsp::WindowingFunction<float>::fillWindowingTables (designWindow.data(), (size_t) firLength,
                                                              juce::dsp::WindowingFunction<float>::blackman, false);

    midBuffer.setSize (1, maxBlockSize);
    sideBuffer.setSize (1, maxBlockSize);

    const juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxBlockSize, 1 };
    midFilter.prepare (spec);
    sideFilter.prepare (spec);

    // Start with a neutral filter (pure delay), so the latency is right from the start
    Spectrum::Curve flat {};
    for (auto* filter : { &midFilter, &sideFilter })
    {
        juce::AudioBuffer<float> ir;
        designFilter (flat, ir);
        filter->loadImpulseResponse (std::move (ir), sampleRate, juce::dsp::Convolution::Stereo::no,
                                     juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);
    }
    midFilter.reset();
    sideFilter.reset();

    fifo.reset();
    analyser.reset();
    frame.clear();
    newSamples = 0;
    frameDecay = std::exp (-(double) Spectrum::Analyser::hop / (sampleRate * trackingSeconds));
    appliedMid = appliedSide = flat;
    filtersValid = false;
    {
        const juce::SpinLock::ScopedLockType lock (pendingLock);
        pendingReady = false;
    }

    prepared = true;
    startThread (juce::Thread::Priority::low);
}

void MatchEngine::release()
{
    stopThread (4000);
    prepared = false;
}

void MatchEngine::setReference (const Spectrum::Profile& newReference)
{
    {
        const juce::ScopedLock lock (stateLock);
        reference = newReference;
    }
    referenceChanged = true;
}

MatchEngine::State MatchEngine::getState() const
{
    const juce::ScopedLock lock (stateLock);
    return state;
}

//==============================================================================
void MatchEngine::process (juce::AudioBuffer<float>& buffer, float amount) noexcept
{
    const int numSamples = buffer.getNumSamples();
    if (! prepared || numSamples == 0 || buffer.getNumChannels() == 0)
        return;

    currentAmount.store (amount, std::memory_order_relaxed);

    // New filters from the engine thread (try-lock: never wait on the audio thread)
    {
        const juce::SpinLock::ScopedTryLockType lock (pendingLock);
        if (lock.isLocked() && pendingReady)
        {
            using C = juce::dsp::Convolution;
            midFilter.loadImpulseResponse  (std::move (pendingMid),  sampleRate, C::Stereo::no, C::Trim::no, C::Normalise::no);
            sideFilter.loadImpulseResponse (std::move (pendingSide), sampleRate, C::Stereo::no, C::Trim::no, C::Normalise::no);
            pendingReady = false;
        }
    }

    float* left  = buffer.getWritePointer (0);
    float* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;
    const float* rightIn = right != nullptr ? right : left;

    // Input -> engine thread (if the thread lags behind, the rest is dropped)
    {
        const auto scope = fifo.write (numSamples);
        for (int ch = 0; ch < 2; ++ch)
        {
            const float* src = ch == 0 ? left : rightIn;
            if (scope.blockSize1 > 0) fifoBuffer.copyFrom (ch, scope.startIndex1, src, scope.blockSize1);
            if (scope.blockSize2 > 0) fifoBuffer.copyFrom (ch, scope.startIndex2, src + scope.blockSize1, scope.blockSize2);
        }
    }

    // Mid / side, filter, back to left / right
    float* mid  = midBuffer.getWritePointer (0);
    float* side = sideBuffer.getWritePointer (0);
    juce::FloatVectorOperations::add      (mid,  left, rightIn, numSamples);
    juce::FloatVectorOperations::subtract (side, left, rightIn, numSamples);
    juce::FloatVectorOperations::multiply (mid,  0.5f, numSamples);
    juce::FloatVectorOperations::multiply (side, 0.5f, numSamples);

    auto midBlock  = juce::dsp::AudioBlock<float> (midBuffer).getSubBlock (0, (size_t) numSamples);
    auto sideBlock = juce::dsp::AudioBlock<float> (sideBuffer).getSubBlock (0, (size_t) numSamples);
    midFilter.process  (juce::dsp::ProcessContextReplacing<float> (midBlock));
    sideFilter.process (juce::dsp::ProcessContextReplacing<float> (sideBlock));

    juce::FloatVectorOperations::add (left, mid, side, numSamples);
    if (right != nullptr)
        juce::FloatVectorOperations::subtract (right, mid, side, numSamples);

    for (int ch = 2; ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);
}

//==============================================================================
void MatchEngine::run()
{
    auto lastUpdate = juce::Time::getMillisecondCounterHiRes();
    float lastAmount = -1.0f;

    while (! threadShouldExit())
    {
        readInput();

        const auto now    = juce::Time::getMillisecondCounterHiRes();
        const float amount = currentAmount.load (std::memory_order_relaxed);

        if (now - lastUpdate >= updateSeconds * 1000.0 || std::abs (amount - lastAmount) > 0.002f
            || referenceChanged.exchange (false))
        {
            updateFilters();
            lastUpdate = now;
            lastAmount = amount;
        }

        wait (20);
    }
}

// Collects the input into overlapping frames and adds them to the live analysis
void MatchEngine::readInput()
{
    constexpr int size = Spectrum::Analyser::size, hop = Spectrum::Analyser::hop;

    for (int ready = fifo.getNumReady(); ready > 0; ready = fifo.getNumReady())
    {
        const int count = juce::jmin (ready, hop - newSamples);
        const auto scope = fifo.read (count);

        for (int ch = 0; ch < 2; ++ch)
        {
            float* dest = frame.getWritePointer (ch, size - hop + newSamples);
            if (scope.blockSize1 > 0) juce::FloatVectorOperations::copy (dest, fifoBuffer.getReadPointer (ch, scope.startIndex1), scope.blockSize1);
            if (scope.blockSize2 > 0) juce::FloatVectorOperations::copy (dest + scope.blockSize1, fifoBuffer.getReadPointer (ch, scope.startIndex2), scope.blockSize2);
        }
        newSamples += count;

        if (newSamples < hop)
            continue;

        // Full frame. Silence (pauses, stopped transport) is skipped, so the analysis
        // and the correction stay as they are.
        const float level = frame.getRMSLevel (0, 0, size) + frame.getRMSLevel (1, 0, size);
        if (level > 1.0e-4f)
            analyser.addFrame (frame.getReadPointer (0), frame.getReadPointer (1), frameDecay);

        for (int ch = 0; ch < 2; ++ch)   // keep the second half for the next (overlapping) frame
            juce::FloatVectorOperations::copy (frame.getWritePointer (ch), frame.getReadPointer (ch, hop), size - hop);
        newSamples = 0;
    }
}

void MatchEngine::updateFilters()
{
    Spectrum::Profile ref;
    {
        const juce::ScopedLock lock (stateLock);
        ref = reference;
    }

    const auto live = analyser.hasData() ? analyser.getProfile (sampleRate) : Spectrum::Profile {};

    Spectrum::Curve mid {}, side {};
    if (ref.valid && live.valid)
        calculateCorrection (ref, live, mid, side);

    const float amount = currentAmount.load (std::memory_order_relaxed);
    for (size_t b = 0; b < mid.size(); ++b)
    {
        mid[b]  *= amount;
        side[b] *= amount;
    }

    // New filters only when the curves have changed audibly
    if (! filtersValid || maxDifference (mid, appliedMid) > 0.05f || maxDifference (side, appliedSide) > 0.05f)
    {
        juce::AudioBuffer<float> irMid, irSide;
        designFilter (mid, irMid);
        designFilter (side, irSide);
        {
            const juce::SpinLock::ScopedLockType lock (pendingLock);
            pendingMid   = std::move (irMid);
            pendingSide  = std::move (irSide);
            pendingReady = true;
        }
        appliedMid   = mid;
        appliedSide  = side;
        filtersValid = true;
    }

    const juce::ScopedLock lock (stateLock);
    state.live = live;
    state.mid  = appliedMid;
    state.side = appliedSide;
}

// Linear-phase FIR filter with the given magnitude response (dB per band):
// symmetric spectrum -> inverse FFT -> zero-phase impulse response, centred and windowed
void MatchEngine::designFilter (const Spectrum::Curve& gainDb, juce::AudioBuffer<float>& ir)
{
    const int n = firLength;
    std::fill (designData.begin(), designData.end(), 0.0f);

    for (int k = 0; k <= n / 2; ++k)
    {
        const auto hz = (float) ((double) k * sampleRate / n);
        designData[(size_t) k * 2] = juce::Decibels::decibelsToGain (Spectrum::interpolate (gainDb, Spectrum::bandOfFrequency (hz)));
    }

    designFft->performRealOnlyInverseTransform (designData.data());

    ir.setSize (1, n);
    auto* out = ir.getWritePointer (0);
    for (int i = 0; i < n; ++i)
        out[i] = designData[(size_t) ((i + n / 2) % n)] * designWindow[(size_t) i];
}
