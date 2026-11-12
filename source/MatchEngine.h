#pragma once

#include "Spectrum.h"
#include "Dynamics.h"
#include "Analog.h"
#include "ToneStages.h"
#include "Limiter.h"
#include <complex>

//==============================================================================
// Matching engine: static + dynamic linear-phase correction towards the reference.
//
// Static part  - from a learned profile of the input (LEARN) to the reference:
//                EQ of left and right, phase between the channels, EQ of mid and side.
// Dynamic part - follows the music: compares the current input (as it sounds after
//                the static part) with the reference and corrects mid and side.
//                Bands that drop out or return are held until the analysis has caught
//                up, boosts change slowly and cuts quickly, so returning bass does not
//                pump or overshoot.
// Both are combined into one 2x2 filter matrix (one convolution stage).
//
// Six stages in any order (setStageOrder), each working on blocks of 512 samples:
//  EQ MATCH   - the filter matrix,
//  DYNAMICS   - multiband dynamics (Dynamics::Matcher) with its own 10 ms lookahead delay:
//               maps the dynamic range of its input per band to the one of the reference,
//  ANALOG     - analog heat (Analog::Heat): tape, wow/flutter, crackle, tube, transformer,
//  PHASEQ     - phase only (Tone::FirStage),
//  GRAPHIC EQ - linear-phase six band EQ (Tone::FirStage),
//  LIMITER    - lookahead brickwall limiter (Limit::Limiter).
// The latency is the same in every order.
//
// Audio thread  - process(): hands input/output to the engine thread, applies the filters.
// Engine thread - analysis, learning, correction and filter design (about every 40 ms);
//                 new filters are handed over without locks and cross-faded.
//==============================================================================
class MatchEngine final : private juce::Thread
{
public:
    static constexpr int    blockSize      = 512;     // internal processing block
    static constexpr double lookaheadMs    = 10.0;
    static constexpr double updateSeconds  = 0.04;    // filter update interval

    MatchEngine();
    ~MatchEngine() override;

    // Called while the audio is stopped (prepareToPlay / releaseResources)
    void prepare (double sampleRate, int maxBlockSize);
    void release();

    // Internal block + all stages
    int getLatencySamples() const noexcept
    {
        return blockSize + firLength / 2 + lookahead + analog.getLatencySamples()
             + phaseFilter.getLatencySamples() + graphicFilter.getLatencySamples() + limiter.getLatencySamples();
    }

    // Settings of the stages that are not set by the engine itself (audio thread)
    struct StageSettings
    {
        Analog::Settings analog;
        Tone::PhaseSettings phase;
        Tone::EqSettings eq;
        Limit::Settings limiter;
        std::array<bool, 6> bypass {};   // per stage (Stage order)
    };

    // Audio thread. amount = 0 ... 1 (0 = no correction, signal only delayed; static phase
    // full from 0.25 on, static levels from 0.5 on, the dynamic part follows amount),
    // response = time constant of the live analysis in seconds,
    // dynamicsAmount = 0 ... 1 (0 = dynamics stage off), stages: settings of the other stages
    void process (juce::AudioBuffer<float>& buffer, float amount, float response, float dynamicsAmount,
                  const StageSettings& stages) noexcept;

    // Limiter meters (any thread)
    float getLimiterReductionDb() const noexcept   { return limiter.getGainReductionDb(); }
    Limit::Meters getLimiterMeters() const noexcept   { return limiter.getMeters(); }
    float getAnalogVuDb (int channel) const noexcept  { return analog.getVuDb (channel); }
    int readLimiterColumns (Limit::Column* destination, int maxCount) noexcept   { return limiter.readColumns (destination, maxCount); }
    void resetLimiterLoudness() noexcept           { limiter.resetLoudness(); }

    // Profiles (any thread except audio)
    void setReference (const Spectrum::Profile& reference);
    void setLearned (const Spectrum::Profile& learned);
    Spectrum::Profile getLearned() const;
    Spectrum::Profile getOutputProfile() const;   // processed signal, for the display

    // Order of the stages; a change mutes the output for about 0.3 s (the paths differ)
    enum Stage { eqStage, dynamicsStage, analogStage, phaseStage, graphicEqStage, limiterStage, numStages };
    using Order = std::array<int, numStages>;
    static Order defaultOrder() noexcept   { return { eqStage, dynamicsStage, analogStage, phaseStage, graphicEqStage, limiterStage }; }
    static_assert (numStages == 6, "StageSettings::bypass and the bypass delays assume six stages");
    void setStageOrder (const Order& order) noexcept
    {
        if (isValid (order))   // every stage exactly once
            orderRequest = encode (order);
    }
    static bool isValid (const Order& order) noexcept
    {
        std::array<bool, numStages> seen {};
        for (int stage : order)
        {
            if (stage < 0 || stage >= numStages || seen[(size_t) stage])
                return false;
            seen[(size_t) stage] = true;
        }
        return true;
    }

    // Dynamics (any thread except audio)
    void setReferenceDynamics (const Dynamics::Profile& reference);
    void setLearnedDynamics (const Dynamics::Profile& learned);
    Dynamics::Profile getLearnedDynamics() const;

    struct DynamicsState
    {
        Dynamics::Profile input, output;   // live input of the dynamics stage (last ~15 s), output
        Dynamics::Values slope {};         // applied slope per band (output range / input range)
        bool fromLearned = false;          // input range taken from the learned input
    };
    DynamicsState getDynamicsState() const;
    float getDynamicsGainDb (int band) const noexcept   { return dynamics.getGainDb ((size_t) band); }

    // LEARN: start / stop the analysis of the input (the result follows a moment later)
    void setLearning (bool shouldLearn) noexcept   { learnRequest = shouldLearn; }
    bool isLearning() const noexcept               { return learning.load(); }
    int getLearnedVersion() const noexcept         { return learnedVersion.load(); }

    // Correction curves (dB / radians per band)
    struct Correction
    {
        Spectrum::Curve left {}, right {}, phase {}, mid {}, side {};
    };

private:
    //==========================================================================
    // Collects one stereo signal from the audio thread into overlapping frames
    struct Tap
    {
        static constexpr int fifoSize = 1 << 16;
        juce::AbstractFifo fifo { fifoSize };
        juce::AudioBuffer<float> fifoBuffer { 2, fifoSize };
        juce::AudioBuffer<float> frame { 2, Spectrum::Analyser::size };
        int newSamples = 0;
        Spectrum::Analyser analyser;
        Dynamics::Analyser dynamics;   // only used with withDynamics
        bool withDynamics = false;

        void reset();
        void push (const float* l, const float* r, int numSamples) noexcept;   // audio thread
        void read (double decay, double fastDecay, Spectrum::Analyser* alsoTo = nullptr,
                   Dynamics::Analyser* alsoToDynamics = nullptr);              // engine thread
    };

    void run() override;
    void updateLearning();
    void updateFilters (double seconds);
    Tone::FirStage::Response matchResponse (const Correction& c, float frequency) const;
    void updateDynamic (const Spectrum::Profile& ref, const Spectrum::Profile& live,
                        const Spectrum::Curve& fastMid, const Spectrum::Curve& fastSide, double seconds);
    void updateDynamicsMatch();
    void processBlock() noexcept;
    void updateToneStages();
    int stageLatency (int stage) const noexcept;
    static int encode (const Order& o) noexcept
    {
        int code = 0;
        for (auto it = o.rbegin(); it != o.rend(); ++it)
            code = code * numStages + *it;
        return code;
    }
    static Order decode (int code) noexcept
    {
        Order o {};
        for (auto& stage : o)
        {
            stage = code % numStages;
            code /= numStages;
        }
        return o;
    }

    double sampleRate = 48000.0;
    int firLength = 4096, lookahead = 480;

    std::atomic<float> currentAmount { 0.0f }, currentResponse { 1.0f }, currentDynamicsAmount { 0.0f };
    Tap input, output;

    // Learning (engine thread)
    Spectrum::Analyser learnAnalyser;
    Dynamics::Analyser learnDynamics;
    std::atomic<bool> learnRequest { false }, learning { false };
    std::atomic<int> learnedVersion { 0 };

    // Correction state (engine thread)
    Correction staticCorrection, applied;
    Spectrum::Curve dynamicMid {}, dynamicSide {}, longMid {}, longSide {};   // dB
    bool firstFilters = true, longTermValid = false;
    std::atomic<bool> profilesChanged { true };

    // EQ match filter: designed on the engine thread, cross-faded in on the audio thread
    Tone::FirStage matchFilter;

    // Audio thread: lookahead delay, block buffering
    Dynamics::Matcher dynamics;
    Dynamics::Analyser stageDynamics;   // engine thread: readings of the dynamics stage
    Dynamics::Values pivot {};          // engine thread: gliding centre per band
    std::array<bool, Dynamics::numBands> pivotValid {};

    Analog::Heat analog;
    Analog::Settings analogSettings;    // audio thread
    Tone::FirStage phaseFilter, graphicFilter;
    Tone::Shared<Tone::PhaseSettings> phaseShared;
    Tone::Shared<Tone::EqSettings> eqShared;
    Tone::PhaseSettings designedPhase;  // engine thread
    Tone::EqSettings designedEq;
    Limit::Limiter limiter;
    Limit::Settings limiterSettings;    // audio thread

    // Bypass per stage: the stage input, delayed by the latency of the stage, cross-faded
    // with its output (the stages keep running, so switching back is seamless)
    struct Bypass
    {
        std::vector<float> l, r;   // delay of the stage latency
        int position = 0;
        float mix = 1.0f;          // 1 = stage active, 0 = bypassed
    };
    std::array<Bypass, 6> bypass;
    std::array<bool, 6> bypassRequest {};
    std::vector<float> dryL, dryR;

    // Order of the stages and the mute while it changes
    std::atomic<int> orderRequest { encode (defaultOrder()) };
    int orderCode = 0;
    float switchGain = 1.0f, switchStep = 0.0f;
    int switchMute = 0;
    juce::AudioBuffer<float> delayLine;
    int delayPosition = 0;
    std::vector<float> blockL, blockR, outL, outR;
    int blockPosition = 0;
    bool prepared = false;

    // Shared with the GUI / message thread
    mutable juce::CriticalSection stateLock;
    Spectrum::Profile reference, learned, outputProfile;
    Dynamics::Profile referenceDynamics, learnedDynamics;
    DynamicsState dynamicsState;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MatchEngine)
};
