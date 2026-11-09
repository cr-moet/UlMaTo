#pragma once

#include "Spectrum.h"
#include "MatrixConvolver.h"
#include "Dynamics.h"
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
// Both are combined into one 2x2 filter matrix (one convolution stage). Before or after
// it (setDynamicsFirst) sits the multiband dynamics stage (Dynamics::Matcher), which
// maps the dynamic range of its input (learned or live) per band to the one of the
// reference. The 10 ms lookahead delay always sits in front of the dynamics stage, so
// both orders have the same latency. The audio runs 10 ms behind the analysis.
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

    // Lookahead + internal block + linear-phase filters (half their length)
    int getLatencySamples() const noexcept   { return lookahead + blockSize + firLength / 2; }

    // Audio thread. amount = 0 ... 1 (0 = no correction, signal only delayed),
    // response = time constant of the live analysis in seconds,
    // dynamicsAmount = 0 ... 1 (0 = dynamics stage off)
    void process (juce::AudioBuffer<float>& buffer, float amount, float response, float dynamicsAmount) noexcept;

    // Profiles (any thread except audio)
    void setReference (const Spectrum::Profile& reference);
    void setLearned (const Spectrum::Profile& learned);
    Spectrum::Profile getLearned() const;
    Spectrum::Profile getOutputProfile() const;   // processed signal, for the display

    // Order of the stages; a change mutes the output for about 0.1 s (the paths differ)
    void setDynamicsFirst (bool first) noexcept      { dynamicsFirstRequest = first; }

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
    void updateDynamic (const Spectrum::Profile& ref, const Spectrum::Profile& live,
                        const Spectrum::Curve& fastMid, const Spectrum::Curve& fastSide, double seconds);
    void designFilters (const Correction& c, MatrixConvolver::Filters& filters);
    void updateDynamicsMatch();

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

    // Filter design (engine thread)
    std::unique_ptr<juce::dsp::FFT> designFft, partitionFft;
    std::array<std::vector<float>, 4> designData;
    std::vector<float> designWindow, designIr, partitionScratch;

    // Hand-over: the engine fills slot [1 - active] while newFilters is false,
    // the audio thread switches (cross-fade) and clears newFilters
    std::array<MatrixConvolver::Filters, 2> filters;
    std::atomic<int> activeFilters { 0 };
    std::atomic<bool> newFilters { false };

    // Audio thread: lookahead delay, block buffering, convolution
    MatrixConvolver convolver;
    Dynamics::Matcher dynamics;
    Dynamics::Analyser stageDynamics;   // engine thread: readings of the dynamics stage

    // Order of the stages and the mute while it changes
    std::atomic<bool> dynamicsFirstRequest { false };
    bool dynamicsFirst = false;
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
