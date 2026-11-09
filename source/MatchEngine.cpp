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
void MatchEngine::Tap::reset()
{
    fifo.reset();
    frame.clear();
    newSamples = 0;
    analyser.reset();
}

void MatchEngine::Tap::push (const float* l, const float* r, int numSamples) noexcept
{
    const auto scope = fifo.write (numSamples);   // if the engine lags behind, the rest is dropped
    for (int ch = 0; ch < 2; ++ch)
    {
        const float* src = ch == 0 ? l : r;
        if (scope.blockSize1 > 0) fifoBuffer.copyFrom (ch, scope.startIndex1, src, scope.blockSize1);
        if (scope.blockSize2 > 0) fifoBuffer.copyFrom (ch, scope.startIndex2, src + scope.blockSize1, scope.blockSize2);
    }
}

void MatchEngine::Tap::read (double decay)
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

        if ((newSamples += count) < hop)
            continue;

        // Full frame. Silence (pauses, stopped transport) is skipped, so the analysis
        // and the correction stay as they are.
        if (frame.getRMSLevel (0, 0, size) + frame.getRMSLevel (1, 0, size) > 1.0e-4f)
            analyser.addFrame (frame.getReadPointer (0), frame.getReadPointer (1), decay);

        for (int ch = 0; ch < 2; ++ch)   // keep the newest samples for the next frame (regions overlap: memmove)
            std::memmove (frame.getWritePointer (ch), frame.getReadPointer (ch, hop), sizeof (float) * (size_t) (size - hop));
        newSamples = 0;
    }
}

//==============================================================================
MatchEngine::MatchEngine() : juce::Thread ("UlMaTo match engine") {}

MatchEngine::~MatchEngine()
{
    stopThread (4000);
}

void MatchEngine::prepare (double newSampleRate, int)
{
    stopThread (4000);

    sampleRate = newSampleRate;

    // Filter length: about 85 ms (frequency resolution ~12 Hz), power of two
    firLength = juce::nextPowerOfTwo ((int) (sampleRate * 0.085));
    designFft    = std::make_unique<juce::dsp::FFT> (juce::roundToInt (std::log2 (firLength)));
    partitionFft = std::make_unique<juce::dsp::FFT> (juce::roundToInt (std::log2 (2 * blockSize)));
    designData.assign ((size_t) firLength * 2, 0.0f);
    designIr.assign ((size_t) firLength, 0.0f);
    designWindow.resize ((size_t) firLength);
    juce::dsp::WindowingFunction<float>::fillWindowingTables (designWindow.data(), (size_t) firLength,
                                                              juce::dsp::WindowingFunction<float>::blackman, false);

    for (auto* c : { &midConvolver, &sideConvolver })
        c->prepare (blockSize, firLength);

    // Both slots start with a neutral filter (pure delay)
    const Spectrum::Curve flat {};
    for (auto& set : filters)
    {
        designFilter (flat, set.mid);
        set.side = set.mid;
    }
    activeFilter = 0;
    newFilter = false;

    for (auto* v : { &midBlock, &sideBlock, &midOut, &sideOut })
        v->assign ((size_t) blockSize, 0.0f);
    blockPosition = 0;

    input.reset();
    output.reset();
    appliedMid = appliedSide = flat;
    filtersValid = true;

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

Spectrum::Profile MatchEngine::getOutputProfile() const
{
    const juce::ScopedLock lock (stateLock);
    return outputProfile;
}

//==============================================================================
void MatchEngine::process (juce::AudioBuffer<float>& buffer, float amount, float response) noexcept
{
    const int numSamples = buffer.getNumSamples();
    if (! prepared || numSamples == 0 || buffer.getNumChannels() == 0)
        return;

    currentAmount.store (amount, std::memory_order_relaxed);
    currentResponse.store (response, std::memory_order_relaxed);

    float* left  = buffer.getWritePointer (0);
    float* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : left;

    input.push (left, right, numSamples);

    // Mid / side in internal blocks of 'blockSize' samples (output one block later)
    for (int i = 0; i < numSamples; ++i)
    {
        const auto pos = (size_t) blockPosition;
        midBlock[pos]  = 0.5f * (left[i] + right[i]);
        sideBlock[pos] = 0.5f * (left[i] - right[i]);
        left[i]  = midOut[pos] + sideOut[pos];
        right[i] = midOut[pos] - sideOut[pos];   // mono: left == right == mid

        if (++blockPosition < blockSize)
            continue;

        blockPosition = 0;

        // New filter from the engine thread: cross-fade within this block
        const int active = activeFilter.load (std::memory_order_relaxed);
        const bool change = newFilter.load (std::memory_order_acquire);
        const auto& current = filters[(size_t) active];
        const auto* next = change ? &filters[(size_t) (1 - active)] : nullptr;

        midConvolver.process  (midBlock.data(),  current.mid.data(),  next != nullptr ? next->mid.data()  : nullptr);
        sideConvolver.process (sideBlock.data(), current.side.data(), next != nullptr ? next->side.data() : nullptr);
        std::swap (midBlock, midOut);
        std::swap (sideBlock, sideOut);

        if (change)
        {
            activeFilter.store (1 - active, std::memory_order_relaxed);
            newFilter.store (false, std::memory_order_release);
        }
    }

    output.push (left, right, numSamples);

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
        // Exponential averaging per frame with the selected time constant
        const double frameSeconds = Spectrum::Analyser::hop / sampleRate;
        const double decay = std::exp (-frameSeconds / juce::jmax (0.05, (double) currentResponse.load()));
        input.read (decay);
        output.read (decay);

        const auto now = juce::Time::getMillisecondCounterHiRes();
        const float amount = currentAmount.load (std::memory_order_relaxed);

        if (now - lastUpdate >= updateSeconds * 1000.0 || std::abs (amount - lastAmount) > 0.002f
            || referenceChanged.exchange (false))
        {
            updateFilters();
            lastUpdate = now;
            lastAmount = amount;
        }

        wait (10);
    }
}

void MatchEngine::updateFilters()
{
    Spectrum::Profile ref;
    {
        const juce::ScopedLock lock (stateLock);
        ref = reference;
    }

    const auto live = input.analyser.hasData() ? input.analyser.getProfile (sampleRate) : Spectrum::Profile {};

    Spectrum::Curve mid {}, side {};
    if (ref.valid && live.valid)
        calculateCorrection (ref, live, mid, side);

    const float amount = currentAmount.load (std::memory_order_relaxed);
    for (size_t b = 0; b < mid.size(); ++b)
    {
        mid[b]  *= amount;
        side[b] *= amount;
    }

    // New filters only when the curves have changed and the audio thread has taken
    // the previous ones (otherwise the next update tries again)
    if ((maxDifference (mid, appliedMid) > 0.02f || maxDifference (side, appliedSide) > 0.02f)
        && ! newFilter.load (std::memory_order_acquire))
    {
        auto& set = filters[(size_t) (1 - activeFilter.load (std::memory_order_relaxed))];
        designFilter (mid, set.mid);
        designFilter (side, set.side);
        newFilter.store (true, std::memory_order_release);
        appliedMid  = mid;
        appliedSide = side;
    }

    const auto out = output.analyser.hasData() ? output.analyser.getProfile (sampleRate) : Spectrum::Profile {};
    const juce::ScopedLock lock (stateLock);
    outputProfile = out;
}

// Linear-phase FIR filter with the given magnitude response (dB per band):
// symmetric spectrum -> inverse FFT -> zero-phase impulse response, centred and
// windowed -> partition spectra for the convolution
void MatchEngine::designFilter (const Spectrum::Curve& gainDb, std::vector<float>& spectra)
{
    const int n = firLength;
    std::fill (designData.begin(), designData.end(), 0.0f);

    for (int k = 0; k <= n / 2; ++k)
    {
        const auto hz = (float) ((double) k * sampleRate / n);
        designData[(size_t) k * 2] = juce::Decibels::decibelsToGain (Spectrum::interpolate (gainDb, Spectrum::bandOfFrequency (hz)));
    }

    designFft->performRealOnlyInverseTransform (designData.data());

    for (int i = 0; i < n; ++i)
        designIr[(size_t) i] = designData[(size_t) ((i + n / 2) % n)] * designWindow[(size_t) i];

    PartitionedConvolver::makeSpectra (designIr.data(), n, blockSize, *partitionFft, partitionScratch, spectra);
}
