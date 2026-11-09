#include "MatchEngine.h"

namespace
{
    using namespace Spectrum;

    constexpr float floorDb        = -60.0f;   // relative level below which a band counts as empty
    constexpr float maxStaticDb    = 12.0f;    // mid/side limits per band
    constexpr float maxBalanceDb   = 3.0f;     // left/right: only the balance, gently
    constexpr float maxSideBoostDb = 6.0f;     // side may be raised this much at most (above 400 Hz)
    constexpr float maxDynamicDb   = 9.0f;
    constexpr float maxDynamicSide = 4.0f;     // side: boosts (cuts may go to maxDynamicDb)
    constexpr float maxPhase       = juce::MathConstants<float>::pi / 4.0f;
    constexpr float boostDbPerSec  = 4.0f;     // dynamic part: gain may rise slowly ...
    constexpr float cutDbPerSec    = 24.0f;    // ... and fall quickly
    constexpr float sideUpPerSec   = 2.0f;     // side moves slower (the image must not wander)
    constexpr float sideDownPerSec = 6.0f;
    constexpr float holdDb         = 6.0f;     // current level this far from the average: hold
    constexpr float absentDb       = 10.0f;    // average this far below the long-term level: hold
    constexpr double fastSeconds   = 0.15;     // time constant of the current level
    constexpr double longSeconds   = 10.0;     // time constant of the long-term level

    float powerGain (float db)   { return std::pow (10.0f, db * 0.1f); }

    // 0 at or below 'from' Hz, 1 at or above 'to' Hz, logarithmic in between
    float fade (size_t band, float from, float to)
    {
        return juce::jlimit (0.0f, 1.0f, std::log2 (bandFrequency ((float) band) / from) / std::log2 (to / from));
    }

    // Smoothing twice with a width growing towards the bass (1/3 octave above 800 Hz, one
    // octave below 200 Hz): broad curves only, so the linear-phase filters ring as little
    // as possible - narrow bass corrections are what makes linear-phase EQs sound smeared.
    Curve smooth (const Curve& raw, float limit, int minWidth = 2)
    {
        Curve a = raw, b {};
        for (int pass = 0; pass < 2; ++pass)
        {
            for (int k = 0; k < numBands; ++k)
            {
                const int w = juce::jmax (minWidth, 2 + juce::roundToInt (4.0f * (1.0f - fade ((size_t) k, 200.0f, 800.0f))));
                float sum = 0.0f;
                int count = 0;
                for (int i = juce::jmax (0, k - w); i <= juce::jmin (numBands - 1, k + w); ++i, ++count)
                    sum += a[(size_t) i];
                b[(size_t) k] = sum / (float) count;
            }
            a = b;
        }
        for (auto& v : a)
            v = juce::jlimit (-limit, limit, v);
        return a;
    }

    // Unsmoothed correction (dB) that turns one curve into the reference, both relative to
    // their own average mid level (only the balance is matched, not the loudness). Empty bands: 0.
    Curve rawMatch (const Curve& ref, float refLevel, const Curve& in, float inLevel)
    {
        Curve raw {};
        for (size_t b = 0; b < (size_t) numBands; ++b)
        {
            const float r = toDb (ref[b]) - refLevel, l = toDb (in[b]) - inLevel;
            raw[b] = r > floorDb && l > floorDb ? r - l : 0.0f;
        }
        return raw;
    }

    Curve matchCurve (const Curve& ref, float refLevel, const Curve& in, float inLevel, float limit)
    {
        return smooth (rawMatch (ref, refLevel, in, inLevel), limit);
    }

    // Raising the side signal brings up whatever is decorrelated in it. In the bass that is
    // what sounds phasy and pulls at the ears, so side boosts fade in between 120 and 400 Hz
    // and stay moderate everywhere; cuts are always allowed.
    float limitSide (size_t band, float db)
    {
        return juce::jlimit (-maxStaticDb, maxSideBoostDb * fade (band, 120.0f, 400.0f), db);
    }

    // Profile as it is after the left/right gains, the phase and the mid/side gains of a correction
    Profile applyCorrection (const Profile& p, const MatchEngine::Correction& c)
    {
        Profile q = p;
        for (size_t b = 0; b < (size_t) numBands; ++b)
        {
            const float pl = powerGain (c.left[b]), pr = powerGain (c.right[b]);
            const float g = std::sqrt (pl * pr), cs = std::cos (c.phase[b]), sn = std::sin (c.phase[b]);
            q.left[b]    = p.left[b] * pl;
            q.right[b]   = p.right[b] * pr;
            q.crossRe[b] = g * (p.crossRe[b] * cs - p.crossIm[b] * sn);
            q.crossIm[b] = g * (p.crossRe[b] * sn + p.crossIm[b] * cs);

            const float sum = q.left[b] + q.right[b];
            q.mid[b]  = juce::jmax (0.0f, 0.25f * (sum + 2.0f * q.crossRe[b])) * powerGain (c.mid[b]);
            q.side[b] = juce::jmax (0.0f, 0.25f * (sum - 2.0f * q.crossRe[b])) * powerGain (c.side[b]);
        }
        return q;
    }

    // Static correction from the learned input to the reference:
    //  - left/right: only the balance between the channels (the tone is done by mid/side),
    //    broad and within +-3 dB, so the image does not shift from band to band,
    //  - phase between the channels: only where it is consistent in both signals, not in
    //    the bass, within +-45 degrees and very broad,
    //  - mid/side for what is left after that.
    MatchEngine::Correction calculateStatic (const Profile& ref, const Profile& in)
    {
        MatchEngine::Correction c;
        const float refLevel = ref.referenceLevelDb(), inLevel = in.referenceLevelDb();
        const auto dl = rawMatch (ref.left,  refLevel, in.left,  inLevel);
        const auto dr = rawMatch (ref.right, refLevel, in.right, inLevel);

        Curve balance {}, phase {};
        for (size_t b = 0; b < (size_t) numBands; ++b)
        {
            balance[b] = 0.5f * (dl[b] - dr[b]);
            if (ref.coherence (b) > 0.5f && in.coherence (b) > 0.5f)
                phase[b] = juce::jlimit (-maxPhase, maxPhase, std::remainder (ref.phase (b) - in.phase (b), juce::MathConstants<float>::twoPi));
        }
        balance = smooth (balance, maxBalanceDb, 6);
        phase   = smooth (phase, maxPhase, 6);
        for (size_t b = 0; b < (size_t) numBands; ++b)
        {
            c.left[b]  =  balance[b];
            c.right[b] = -balance[b];
            c.phase[b] = phase[b] * fade (b, 150.0f, 400.0f);
        }

        const auto after = applyCorrection (in, c);
        const float afterLevel = after.referenceLevelDb();
        c.mid  = matchCurve (ref.mid,  refLevel, after.mid,  afterLevel, maxStaticDb);
        c.side = matchCurve (ref.side, refLevel, after.side, afterLevel, maxStaticDb);
        for (size_t b = 0; b < (size_t) numBands; ++b)
            c.side[b] = limitSide (b, c.side[b]);
        return c;
    }

    float maxDifference (const MatchEngine::Correction& a, const MatchEngine::Correction& b)
    {
        float d = 0.0f;
        for (size_t i = 0; i < (size_t) numBands; ++i)
            d = std::max ({ d, std::abs (a.left[i] - b.left[i]), std::abs (a.right[i] - b.right[i]),
                            10.0f * std::abs (a.phase[i] - b.phase[i]),
                            std::abs (a.mid[i] - b.mid[i]), std::abs (a.side[i] - b.side[i]) });
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

void MatchEngine::Tap::read (double decay, double fastDecay, Spectrum::Analyser* alsoTo)
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
        {
            analyser.addFrame (frame.getReadPointer (0), frame.getReadPointer (1), decay, fastDecay);
            if (alsoTo != nullptr)
                alsoTo->addFrame (frame.getReadPointer (0), frame.getReadPointer (1));
        }

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
    lookahead  = juce::jmax (1, juce::roundToInt (sampleRate * lookaheadMs * 0.001));

    // Filter length: about 85 ms (frequency resolution ~12 Hz), power of two
    firLength    = juce::nextPowerOfTwo ((int) (sampleRate * 0.085));
    designFft    = std::make_unique<juce::dsp::FFT> (juce::roundToInt (std::log2 (firLength)));
    partitionFft = std::make_unique<juce::dsp::FFT> (juce::roundToInt (std::log2 (2 * blockSize)));
    for (auto& d : designData)
        d.assign ((size_t) firLength * 2, 0.0f);
    designIr.assign ((size_t) firLength, 0.0f);
    designWindow.resize ((size_t) firLength);
    juce::dsp::WindowingFunction<float>::fillWindowingTables (designWindow.data(), (size_t) firLength,
                                                              juce::dsp::WindowingFunction<float>::blackman, false);

    convolver.prepare (blockSize, firLength);

    // Both slots start neutral (pure delay)
    applied = {};
    for (auto& f : filters)
        designFilters (applied, f);
    activeFilters = 0;
    newFilters = false;
    firstFilters = false;

    delayLine.setSize (2, lookahead);
    delayLine.clear();
    delayPosition = 0;
    for (auto* v : { &blockL, &blockR, &outL, &outR })
        v->assign ((size_t) blockSize, 0.0f);
    blockPosition = 0;

    input.reset();
    output.reset();
    dynamicMid = dynamicSide = {};
    longTermValid = false;
    profilesChanged = true;

    prepared = true;
    startThread (juce::Thread::Priority::low);
}

void MatchEngine::release()
{
    stopThread (4000);
    prepared = false;
}

void MatchEngine::setReference (const Spectrum::Profile& p)
{
    {
        const juce::ScopedLock lock (stateLock);
        reference = p;
    }
    profilesChanged = true;
}

void MatchEngine::setLearned (const Spectrum::Profile& p)
{
    {
        const juce::ScopedLock lock (stateLock);
        learned = p;
    }
    profilesChanged = true;
    ++learnedVersion;
}

Spectrum::Profile MatchEngine::getLearned() const
{
    const juce::ScopedLock lock (stateLock);
    return learned;
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

    // The analysis gets the input now, the audio follows 10 ms later (lookahead)
    input.push (left, right, numSamples);

    float* delayL = delayLine.getWritePointer (0);
    float* delayR = delayLine.getWritePointer (1);

    for (int i = 0; i < numSamples; ++i)
    {
        const auto d = (size_t) delayPosition, b = (size_t) blockPosition;
        blockL[b] = delayL[d];
        blockR[b] = delayR[d];
        delayL[d] = left[i];
        delayR[d] = right[i];
        delayPosition = (delayPosition + 1) % lookahead;

        left[i] = outL[b];   // processed block, one block behind
        if (right != left)
            right[i] = outR[b];

        if (++blockPosition < blockSize)
            continue;

        blockPosition = 0;

        // New filters from the engine thread: cross-fade within this block
        const int active  = activeFilters.load (std::memory_order_relaxed);
        const bool change = newFilters.load (std::memory_order_acquire);
        convolver.process (blockL.data(), blockR.data(), filters[(size_t) active],
                           change ? &filters[(size_t) (1 - active)] : nullptr);
        std::swap (blockL, outL);
        std::swap (blockR, outR);

        if (change)
        {
            activeFilters.store (1 - active, std::memory_order_relaxed);
            newFilters.store (false, std::memory_order_release);
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
        updateLearning();

        // Exponential averaging per frame with the selected time constant
        const double frameSeconds = Spectrum::Analyser::hop / sampleRate;
        const double decay = std::exp (-frameSeconds / juce::jmax (0.05, (double) currentResponse.load()));
        input.read (decay, std::exp (-frameSeconds / fastSeconds), learning.load() ? &learnAnalyser : nullptr);
        output.read (decay, 0.0);

        const auto now = juce::Time::getMillisecondCounterHiRes();
        const float amount = currentAmount.load (std::memory_order_relaxed);
        const double elapsed = (now - lastUpdate) * 0.001;

        if (elapsed >= updateSeconds || std::abs (amount - lastAmount) > 0.002f || profilesChanged.load())
        {
            updateFilters (juce::jmin (elapsed, 0.5));
            lastUpdate = now;
            lastAmount = amount;
        }

        wait (10);
    }
}

// LEARN switched on: collect the input with equal weight; switched off: keep the result
void MatchEngine::updateLearning()
{
    const bool wanted = learnRequest.load();
    if (wanted == learning.load())
        return;

    if (wanted)
        learnAnalyser.reset();
    else if (learnAnalyser.hasData())
        setLearned (learnAnalyser.getProfile (sampleRate));

    learning = wanted;
}

void MatchEngine::updateFilters (double seconds)
{
    Spectrum::Profile ref, learnedInput;
    {
        const juce::ScopedLock lock (stateLock);
        ref = reference;
        learnedInput = learned;
    }

    if (profilesChanged.exchange (false))
        staticCorrection = ref.valid && learnedInput.valid ? calculateStatic (ref, learnedInput) : Correction {};

    if (ref.valid && input.analyser.hasData())
    {
        Spectrum::Curve fastMid {}, fastSide {};
        input.analyser.getFastMidSide (sampleRate, fastMid, fastSide);
        updateDynamic (ref, input.analyser.getProfile (sampleRate), fastMid, fastSide, seconds);
    }
    else if (! ref.valid)
        dynamicMid = dynamicSide = {};

    // MATCH scales the static and the dynamic part
    // (the dynamic curves are smoothed once more: held and moving bands sit side by side)
    const float amount = currentAmount.load (std::memory_order_relaxed);
    const auto dynMid = smooth (dynamicMid, maxDynamicDb), dynSide = smooth (dynamicSide, maxDynamicDb);
    Correction c;
    for (size_t b = 0; b < (size_t) numBands; ++b)
    {
        c.left[b]  = amount * staticCorrection.left[b];
        c.right[b] = amount * staticCorrection.right[b];
        c.phase[b] = amount * staticCorrection.phase[b];
        c.mid[b]   = amount * (staticCorrection.mid[b] + dynMid[b]);
        c.side[b]  = amount * limitSide (b, staticCorrection.side[b] + dynSide[b]);
    }

    // New filters only when something changed and the audio thread has taken the
    // previous ones (otherwise the next update tries again)
    if ((firstFilters || maxDifference (c, applied) > 0.02f) && ! newFilters.load (std::memory_order_acquire))
    {
        designFilters (c, filters[(size_t) (1 - activeFilters.load (std::memory_order_relaxed))]);
        newFilters.store (true, std::memory_order_release);
        applied = c;
        firstFilters = false;
    }

    const auto out = output.analyser.hasData() ? output.analyser.getProfile (sampleRate) : Spectrum::Profile {};
    const juce::ScopedLock lock (stateLock);
    outputProfile = out;
}

// Dynamic part: compares the input as it sounds after the static part with the reference.
// A band is held (its correction stays as it is)
//  - while its current level differs from its average by more than holdDb: it drops out
//    (e.g. a bass pause) or comes back and the average has not caught up yet,
//  - while its average is more than absentDb below its long-term level: it is missing
//    in this part of the song, however long that lasts.
// So a missing band is never boosted, and nothing overshoots when it returns.
void MatchEngine::updateDynamic (const Spectrum::Profile& ref, const Spectrum::Profile& live,
                                 const Spectrum::Curve& fastMid, const Spectrum::Curve& fastSide, double seconds)
{
    const auto after = applyCorrection (live, staticCorrection);
    const float refLevel = ref.referenceLevelDb(), afterLevel = after.referenceLevelDb();
    const auto targetMid  = matchCurve (ref.mid,  refLevel, after.mid,  afterLevel, maxDynamicDb);
    auto targetSide = matchCurve (ref.side, refLevel, after.side, afterLevel, maxDynamicDb);
    for (auto& v : targetSide)
        v = juce::jmin (v, maxDynamicSide);

    const float up = boostDbPerSec * (float) seconds, down = cutDbPerSec * (float) seconds;
    const float sideUp = sideUpPerSec * (float) seconds, sideDown = sideDownPerSec * (float) seconds;
    const float longStep = 1.0f - (float) std::exp (-seconds / longSeconds);

    const auto follow = [&] (float& value, float target, float averagePower, float currentPower, float& longDb, float rise, float fall)
    {
        const float average = toDb (averagePower);
        if (! longTermValid)
            longDb = average;

        if (std::abs (toDb (currentPower) - average) > holdDb || average < longDb - absentDb)
            return;   // hold

        value  += juce::jlimit (-fall, rise, target - value);
        longDb += longStep * (average - longDb);
    };

    for (size_t b = 0; b < (size_t) numBands; ++b)
    {
        follow (dynamicMid[b],  targetMid[b],  live.mid[b],  fastMid[b],  longMid[b],  up, down);
        follow (dynamicSide[b], targetSide[b], live.side[b], fastSide[b], longSide[b], sideUp, sideDown);
    }
    longTermValid = true;
}

// Linear-phase 2x2 filter matrix from the correction curves:
//   HL, HR: left/right gain with half of the phase difference each,
//   M, S:   mid/side gain  ->  a = (M+S)/2 HL, b = (M-S)/2 HR, c = (M-S)/2 HL, d = (M+S)/2 HR
// Each response -> inverse FFT -> centred, windowed impulse response -> partition spectra
void MatchEngine::designFilters (const Correction& c, MatrixConvolver::Filters& f)
{
    const int n = firLength;
    for (auto& d : designData)
        std::fill (d.begin(), d.end(), 0.0f);

    for (int k = 0; k <= n / 2; ++k)
    {
        const float band = bandOfFrequency ((float) ((double) k * sampleRate / n));
        const bool  real = k == 0 || k == n / 2;   // DC and Nyquist must stay real
        const float half = real ? 0.0f : 0.5f * interpolate (c.phase, band);

        const auto hl = std::polar (juce::Decibels::decibelsToGain (interpolate (c.left,  band)),  half);
        const auto hr = std::polar (juce::Decibels::decibelsToGain (interpolate (c.right, band)), -half);
        const float m = juce::Decibels::decibelsToGain (interpolate (c.mid,  band));
        const float s = juce::Decibels::decibelsToGain (interpolate (c.side, band));
        const float p = 0.5f * (m + s), q = 0.5f * (m - s);

        const std::complex<float> values[] { p * hl, q * hr, q * hl, p * hr };
        for (size_t i = 0; i < 4; ++i)
        {
            designData[i][(size_t) k * 2]     = values[i].real();
            designData[i][(size_t) k * 2 + 1] = values[i].imag();
        }
    }

    std::vector<float>* targets[] { &f.a, &f.b, &f.c, &f.d };
    for (size_t i = 0; i < 4; ++i)
    {
        designFft->performRealOnlyInverseTransform (designData[i].data());
        for (int j = 0; j < n; ++j)
            designIr[(size_t) j] = designData[i][(size_t) ((j + n / 2) % n)] * designWindow[(size_t) j];
        MatrixConvolver::makeSpectra (designIr.data(), n, blockSize, *partitionFft, partitionScratch, *targets[i]);
    }
}
