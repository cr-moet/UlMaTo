#include "MatchEngine.h"

namespace
{
    using namespace Spectrum;

    constexpr float floorDb        = -60.0f;   // relative level below which a band counts as empty
    constexpr float maxStaticDb    = 12.0f;    // mid/side limits per band
    constexpr float maxBalanceDb   = 3.0f;     // left/right: only the balance, gently
    constexpr float maxSideBoostDb = 6.0f;     // side may be raised this much at most (above 400 Hz)
    constexpr float maxDynamicDb   = 9.0f;
    constexpr float maxPhase       = juce::MathConstants<float>::pi / 4.0f;
    constexpr float holdDb         = 6.0f;     // current level this far from the average: hold
    constexpr float absentDb       = 10.0f;    // average this far below the long-term level: hold

    // Dynamic part, bass -> treble (blended between 300 Hz and 3 kHz): the bass moves slowly
    // and calmly (no pumping, no wandering image), the treble follows the music quickly
    struct Speed { float low, high; float at (float s) const { return low + s * (high - low); } };
    constexpr Speed fastShare      { 0.0f, 1.0f };     // share of the current (0.15 s) level in the target
    constexpr Speed glideSeconds   { 0.5f, 0.1f };     // exponential glide towards the target
    constexpr Speed midUpPerSec    { 4.0f, 30.0f };    // ... but never faster than this (dB/s)
    constexpr Speed midDownPerSec  { 24.0f, 40.0f };
    constexpr Speed sideUpPerSec   { 2.0f, 20.0f };
    constexpr Speed sideDownPerSec { 6.0f, 30.0f };
    constexpr Speed maxSideBoost   { 4.0f, 9.0f };     // dynamic side boost (cuts: maxDynamicDb)
    constexpr Speed holdExtraDb    { 0.0f, 30.0f };    // the treble is hardly ever held
    constexpr double fastSeconds   = 0.15;     // time constant of the current level
    constexpr double longSeconds   = 10.0;     // time constant of the long-term level

    // Dynamics: live level distributions cover about the last 15 s
    const double dynamicsDecay = std::exp (-1.0 / (15.0 * Dynamics::readingsPerSecond));
    constexpr float minSlope = 0.4f, maxSlope = 2.5f;   // at most 2.5:1 compression / 1:2.5 expansion

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
    dynamics.reset();
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

void MatchEngine::Tap::read (double decay, double fastDecay, Spectrum::Analyser* alsoTo, Dynamics::Analyser* alsoToDynamics)
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

        // Dynamics: every sample; the live distribution covers about the last 15 s
        const float* newL = frame.getReadPointer (0, size - hop + newSamples);
        const float* newR = frame.getReadPointer (1, size - hop + newSamples);
        if (withDynamics)
            dynamics.process (newL, newR, count, dynamicsDecay);
        if (alsoToDynamics != nullptr)
            alsoToDynamics->process (newL, newR, count);

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
    dynamics.prepare (sampleRate);
    stageDynamics.prepare (sampleRate);
    pivot = {};
    pivotValid = {};
    output.dynamics.prepare (sampleRate);
    output.withDynamics = true;
    learnDynamics.prepare (sampleRate);
    analog.prepare (sampleRate, blockSize);
    phaseFilter.prepare (blockSize, firLength);
    graphicFilter.prepare (blockSize, firLength);
    designedPhase = {};
    designedEq = {};
    limiter.prepare (sampleRate);
    for (int stage = 0; stage < numStages; ++stage)
    {
        auto& b = bypass[(size_t) stage];
        b.l.assign ((size_t) stageLatency (stage), 0.0f);
        b.r.assign ((size_t) stageLatency (stage), 0.0f);
        b.position = 0;
        b.mix = bypassRequest[(size_t) stage] ? 0.0f : 1.0f;
    }
    dryL.assign ((size_t) blockSize, 0.0f);
    dryR.assign ((size_t) blockSize, 0.0f);
    orderCode = orderRequest.load();
    switchGain = 1.0f;
    switchStep = 1.0f / (float) (sampleRate * 0.01);
    switchMute = 0;

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

void MatchEngine::setReferenceDynamics (const Dynamics::Profile& p)
{
    const juce::ScopedLock lock (stateLock);
    referenceDynamics = p;
}

void MatchEngine::setLearnedDynamics (const Dynamics::Profile& p)
{
    {
        const juce::ScopedLock lock (stateLock);
        learnedDynamics = p;
    }
    ++learnedVersion;
}

Dynamics::Profile MatchEngine::getLearnedDynamics() const
{
    const juce::ScopedLock lock (stateLock);
    return learnedDynamics;
}

MatchEngine::DynamicsState MatchEngine::getDynamicsState() const
{
    const juce::ScopedLock lock (stateLock);
    return dynamicsState;
}

Spectrum::Profile MatchEngine::getOutputProfile() const
{
    const juce::ScopedLock lock (stateLock);
    return outputProfile;
}

//==============================================================================
void MatchEngine::process (juce::AudioBuffer<float>& buffer, float amount, float response, float dynamicsAmount,
                           const StageSettings& stages) noexcept
{
    const int numSamples = buffer.getNumSamples();
    if (! prepared || numSamples == 0 || buffer.getNumChannels() == 0)
        return;

    currentAmount.store (amount, std::memory_order_relaxed);
    currentResponse.store (response, std::memory_order_relaxed);
    currentDynamicsAmount.store (dynamicsAmount, std::memory_order_relaxed);
    analogSettings  = stages.analog;
    limiterSettings = stages.limiter;
    bypassRequest   = stages.bypass;
    phaseShared.set (stages.phase);
    eqShared.set (stages.eq);

    float* left  = buffer.getWritePointer (0);
    float* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : left;

    input.push (left, right, numSamples);

    for (int i = 0; i < numSamples; ++i)
    {
        // Collect one block, hand out the processed previous one
        const auto b = (size_t) blockPosition;
        blockL[b] = left[i];
        blockR[b] = right[i];
        float l = outL[b], r = outR[b];

        // Changing the order: fade out, switch, stay silent until the old signal has left
        // all stages, fade in
        const int requested = orderRequest.load (std::memory_order_relaxed);
        if (switchMute > 0 || requested != orderCode || switchGain < 1.0f)
        {
            if (switchMute > 0)
                --switchMute;
            else if (requested != orderCode)
            {
                if ((switchGain -= switchStep) <= 0.0f)
                {
                    switchGain = 0.0f;
                    orderCode = requested;
                    switchMute = getLatencySamples() + 3 * firLength;   // also the history of the three filter stages
                }
            }
            else
                switchGain = juce::jmin (1.0f, switchGain + switchStep);
            l *= switchGain;
            r *= switchGain;
        }

        left[i] = l;
        if (right != left)
            right[i] = r;

        if (++blockPosition == blockSize)
        {
            blockPosition = 0;
            processBlock();
        }
    }

    output.push (left, right, numSamples);

    for (int ch = 2; ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);
}

int MatchEngine::stageLatency (int stage) const noexcept
{
    switch (stage)
    {
        case eqStage:        return firLength / 2;
        case dynamicsStage:  return lookahead;
        case analogStage:    return analog.getLatencySamples();
        case phaseStage:     return phaseFilter.getLatencySamples();
        case graphicEqStage: return graphicFilter.getLatencySamples();
        default:             return limiter.getLatencySamples();
    }
}

// The stages on one block (blockL/R, in place), in the selected order
void MatchEngine::processBlock() noexcept
{
    const float mixStep = (float) blockSize / (float) (sampleRate * 0.02);   // bypass cross-fade: 20 ms

    for (int stage : decode (orderCode))
    {
        // The stage input, delayed by its latency (always kept, so bypass can start any time)
        auto& by = bypass[(size_t) stage];
        const auto length = by.l.size();
        for (size_t j = 0; j < (size_t) blockSize; ++j)
        {
            if (length == 0)
            {
                dryL[j] = blockL[j];
                dryR[j] = blockR[j];
                continue;
            }
            dryL[j] = by.l[(size_t) by.position];
            dryR[j] = by.r[(size_t) by.position];
            by.l[(size_t) by.position] = blockL[j];
            by.r[(size_t) by.position] = blockR[j];
            by.position = (by.position + 1) % (int) length;
        }

        if (stage == eqStage)
        {
            // New filters from the engine thread: cross-fade within this block
            const int active  = activeFilters.load (std::memory_order_relaxed);
            const bool change = newFilters.load (std::memory_order_acquire);
            convolver.process (blockL.data(), blockR.data(), filters[(size_t) active],
                               change ? &filters[(size_t) (1 - active)] : nullptr);
            if (change)
            {
                activeFilters.store (1 - active, std::memory_order_relaxed);
                newFilters.store (false, std::memory_order_release);
            }
        }
        else if (stage == dynamicsStage)
        {
            // Detector on the signal now, gain on the signal 10 ms later (lookahead)
            float* delayL = delayLine.getWritePointer (0);
            float* delayR = delayLine.getWritePointer (1);
            for (size_t j = 0; j < (size_t) blockSize; ++j)
            {
                const auto d = (size_t) delayPosition;
                float l = delayL[d], r = delayR[d];
                dynamics.process (blockL[j], blockR[j], l, r);
                delayL[d] = blockL[j];
                delayR[d] = blockR[j];
                blockL[j] = l;
                blockR[j] = r;
                delayPosition = (delayPosition + 1) % lookahead;
            }
        }
        else if (stage == analogStage)
            analog.process (blockL.data(), blockR.data(), blockSize, analogSettings);
        else if (stage == phaseStage)
            phaseFilter.process (blockL.data(), blockR.data());
        else if (stage == graphicEqStage)
            graphicFilter.process (blockL.data(), blockR.data());
        else
            limiter.process (blockL.data(), blockR.data(), blockSize, limiterSettings);

        // Bypass: ramp over the block towards the target
        const float target = bypassRequest[(size_t) stage] ? 0.0f : 1.0f;
        if (by.mix != 1.0f || target != 1.0f)
        {
            const float start = by.mix, end = start + juce::jlimit (-mixStep, mixStep, target - start);
            for (size_t j = 0; j < (size_t) blockSize; ++j)
            {
                const float mix = start + (end - start) * (float) (j + 1) / (float) blockSize;
                blockL[j] = dryL[j] + mix * (blockL[j] - dryL[j]);
                blockR[j] = dryR[j] + mix * (blockR[j] - dryR[j]);
            }
            by.mix = end;
        }
    }

    std::swap (blockL, outL);
    std::swap (blockR, outR);
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
        input.read (decay, std::exp (-frameSeconds / fastSeconds), learning.load() ? &learnAnalyser : nullptr,
                    learning.load() ? &learnDynamics : nullptr);
        output.read (decay, 0.0);

        const auto now = juce::Time::getMillisecondCounterHiRes();
        const float amount = currentAmount.load (std::memory_order_relaxed);
        const double elapsed = (now - lastUpdate) * 0.001;

        if (elapsed >= updateSeconds || std::abs (amount - lastAmount) > 0.002f || profilesChanged.load())
        {
            updateFilters (juce::jmin (elapsed, 0.5));
            updateDynamicsMatch();
            updateToneStages();
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
    {
        learnAnalyser.reset();
        learnDynamics.reset();
    }
    else if (learnAnalyser.hasData())
    {
        if (learnDynamics.hasData())
            setLearnedDynamics (learnDynamics.getProfile());
        setLearned (learnAnalyser.getProfile (sampleRate));
    }

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

    // MATCH: the static correction from the learned input - its phase part fully in at 25 %,
    // balance and mid/side at 50 % - stays full above; the dynamic part (levels only, never
    // the phase) follows the knob from 0 to 100 %.
    // (the dynamic curves are smoothed once more: held and moving bands sit side by side)
    const float amount = currentAmount.load (std::memory_order_relaxed);
    const float phaseAmount   = juce::jmin (1.0f, 4.0f * amount);
    const float staticAmount  = juce::jmin (1.0f, 2.0f * amount);
    const float dynamicAmount = amount;
    const auto dynMid = smooth (dynamicMid, maxDynamicDb), dynSide = smooth (dynamicSide, maxDynamicDb);
    Correction c;
    for (size_t b = 0; b < (size_t) numBands; ++b)
    {
        c.left[b]  = staticAmount * staticCorrection.left[b];
        c.right[b] = staticAmount * staticCorrection.right[b];
        c.phase[b] = phaseAmount * staticCorrection.phase[b];
        c.mid[b]   = staticAmount * staticCorrection.mid[b] + dynamicAmount * dynMid[b];
        c.side[b]  = limitSide (b, staticAmount * staticCorrection.side[b] + dynamicAmount * dynSide[b]);
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
// Per band, from bass to treble: the target uses more of the current level instead of the
// average, glides there faster and may move faster. In the bass a band is held (its
// correction stays as it is)
//  - while its current level differs from its average by more than holdDb: it drops out
//    (e.g. a bass pause) or comes back and the average has not caught up yet,
//  - while its average is more than absentDb below its long-term level: it is missing
//    in this part of the song, however long that lasts.
// So a missing bass is never boosted and does not overshoot when it returns, while the
// treble follows changes of tone and width without lagging behind.
void MatchEngine::updateDynamic (const Spectrum::Profile& ref, const Spectrum::Profile& live,
                                 const Spectrum::Curve& fastMid, const Spectrum::Curve& fastSide, double seconds)
{
    auto after = applyCorrection (live, staticCorrection);
    const float refLevel = ref.referenceLevelDb(), afterLevel = after.referenceLevelDb();

    // Blend in the current level (as a power ratio on top of the static correction)
    for (size_t b = 0; b < (size_t) numBands; ++b)
    {
        const float share = fastShare.at (fade (b, 300.0f, 3000.0f));
        const auto blend = [share] (float average, float current)
        {
            return average > 1.0e-30f && current > 1.0e-30f ? std::pow (current / average, share) : 1.0f;
        };
        after.mid[b]  *= blend (live.mid[b],  fastMid[b]);
        after.side[b] *= blend (live.side[b], fastSide[b]);
    }

    const auto targetMid  = matchCurve (ref.mid,  refLevel, after.mid,  afterLevel, maxDynamicDb);
    const auto targetSide = matchCurve (ref.side, refLevel, after.side, afterLevel, maxDynamicDb);
    const float dt = (float) seconds;
    const float longStep = 1.0f - std::exp (-dt / (float) longSeconds);

    const auto follow = [&] (float& value, float target, float averagePower, float currentPower, float& longDb,
                             float speed, Speed up, Speed down)
    {
        const float average = toDb (averagePower);
        if (! longTermValid)
            longDb = average;

        const float extra = holdExtraDb.at (speed);
        if (std::abs (toDb (currentPower) - average) > holdDb + extra || average < longDb - absentDb - extra)
            return;   // hold

        const float glide = 1.0f - std::exp (-dt / glideSeconds.at (speed));
        value  += juce::jlimit (-down.at (speed) * dt, up.at (speed) * dt, glide * (target - value));
        longDb += longStep * (average - longDb);
    };

    for (size_t b = 0; b < (size_t) numBands; ++b)
    {
        const float speed = fade (b, 300.0f, 3000.0f);
        follow (dynamicMid[b], targetMid[b], live.mid[b], fastMid[b], longMid[b], speed, midUpPerSec, midDownPerSec);
        follow (dynamicSide[b], juce::jmin (targetSide[b], maxSideBoost.at (speed)),
                live.side[b], fastSide[b], longSide[b], speed, sideUpPerSec, sideDownPerSec);
    }
    longTermValid = true;
}

// PhaseQ and graphic EQ: new filters when their settings have changed
void MatchEngine::updateToneStages()
{
    const auto phase = phaseShared.get();
    if (phase.valid && phase != designedPhase
         && phaseFilter.update (sampleRate, [&phase] (float f) { return Tone::phaseResponse (phase, f); }))
        designedPhase = phase;

    const auto eq = eqShared.get();
    if (eq.freq[0] > 0.0f && eq != designedEq
         && graphicFilter.update (sampleRate, [&eq] (float f) { return Tone::eqResponse (eq, f); }))
        designedEq = eq;
}

// Dynamics: slope per band from the reference range and the input range (of the learned
// input if there is one, otherwise of the live input), scaled by the dynamics amount; the
// pivot is always the live centre, so a changed input level does not matter.
void MatchEngine::updateDynamicsMatch()
{
    DynamicsState state;
    Dynamics::Profile ref, learnedInput;
    {
        const juce::ScopedLock lock (stateLock);
        ref = referenceDynamics;
        learnedInput = learnedDynamics;
    }

    // Readings of the stage; their average per band tells whether a band is there right now
    Dynamics::Values current {};
    int numReadings = 0;
    dynamics.readLevels ([&] (const Dynamics::Values& levels)
    {
        stageDynamics.addLevels (levels, dynamicsDecay);
        for (size_t b = 0; b < (size_t) Dynamics::numBands; ++b)
            current[b] += levels[b];
        ++numReadings;
    });
    if (stageDynamics.hasData (2.0))
        state.input = stageDynamics.getProfile();
    if (output.dynamics.hasData (2.0))
        state.output = output.dynamics.getProfile();

    state.fromLearned = learnedInput.valid;
    const auto& in = learnedInput.valid ? learnedInput : state.input;
    const float amount = currentDynamicsAmount.load (std::memory_order_relaxed);

    // Pivot: the live centre, gliding (3 s of audio, counted in readings) so that it never
    // jumps; held while a band is missing right now (more than 15 dB below it), so it is in
    // place when the band returns
    const float glide = 1.0f - std::exp (-(float) numReadings / (float) (3.0 * Dynamics::readingsPerSecond));

    for (size_t b = 0; b < (size_t) Dynamics::numBands; ++b)
    {
        state.slope[b] = 1.0f;
        const bool missing = pivotValid[b] && numReadings > 0 && current[b] / (float) numReadings < pivot[b] - 15.0f;
        if (state.input.valid && state.input.active[b] && ! missing)
            pivot[b] = pivotValid[b] ? pivot[b] + glide * (state.input.centre[b] - pivot[b]) : state.input.centre[b];
        pivotValid[b] = pivotValid[b] || (state.input.valid && state.input.active[b]);
        if (! (ref.valid && state.input.valid && ref.active[b] && in.active[b] && state.input.active[b]
                && ref.range[b] > 0.3f && in.upper[b] > 0.3f && in.lower[b] > 0.3f))
            continue;

        // Both work on both sides of the centre: the whole range becomes the reference range
        const float slope = ref.range[b] / in.range[b];
        state.slope[b] = std::pow (juce::jlimit (minSlope, maxSlope, slope), amount);
    }
    dynamics.setTargets (state.slope, pivot);

    const juce::ScopedLock lock (stateLock);
    dynamicsState = state;
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
