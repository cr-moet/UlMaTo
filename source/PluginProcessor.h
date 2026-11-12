#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "MatchEngine.h"

namespace ParamIDs
{
    inline constexpr const char* amount   = "amount";     // match amount 0 ... 100 %
    inline constexpr const char* response = "response";   // time constant of the live analysis (s)
    inline constexpr const char* dynamics = "dynamics";   // dynamics match amount 0 ... 100 %
    inline constexpr const char* bypass   = "bypass";

    // Analog heat (all 0 ... 100 %)
    inline constexpr const char* tape[] { "tape1", "tape2", "tape3", "tape4", "tape5" };
    inline constexpr const char* wowDepth     = "wowDepth";       // 0 % (off) ... 100 %
    inline constexpr const char* wowRate      = "wowRate";        // Hz
    inline constexpr const char* flutterDepth = "flutterDepth";
    inline constexpr const char* flutterRate  = "flutterRate";
    inline constexpr const char* crackle     = "crackle";
    inline constexpr const char* hum         = "hum";
    inline constexpr const char* hum60       = "hum60";        // mains hum 60 Hz (off: 50 Hz)
    inline constexpr const char* analogDrive = "analogDrive";  // dB into the analog heat, taken back after it
    inline constexpr const char* tube        = "tube";
    inline constexpr const char* transformer = "transformer";

    // Graphic EQ: twelve bands (off until created), bell / low shelf / high shelf, bypassable
    inline constexpr const char* eqOn[12] { "eqOn1", "eqOn2", "eqOn3", "eqOn4", "eqOn5", "eqOn6", "eqOn7", "eqOn8", "eqOn9", "eqOn10", "eqOn11", "eqOn12" };
    inline constexpr const char* eqBypass[12] { "eqBypass1", "eqBypass2", "eqBypass3", "eqBypass4", "eqBypass5", "eqBypass6", "eqBypass7", "eqBypass8", "eqBypass9", "eqBypass10", "eqBypass11", "eqBypass12" };
    inline constexpr const char* eqType[12] { "eqType1", "eqType2", "eqType3", "eqType4", "eqType5", "eqType6", "eqType7", "eqType8", "eqType9", "eqType10", "eqType11", "eqType12" };
    inline constexpr const char* eqFreq[12] { "eqFreq1", "eqFreq2", "eqFreq3", "eqFreq4", "eqFreq5", "eqFreq6", "eqFreq7", "eqFreq8", "eqFreq9", "eqFreq10", "eqFreq11", "eqFreq12" };
    inline constexpr const char* eqGain[12] { "eqGain1", "eqGain2", "eqGain3", "eqGain4", "eqGain5", "eqGain6", "eqGain7", "eqGain8", "eqGain9", "eqGain10", "eqGain11", "eqGain12" };
    inline constexpr const char* eqQ[12] { "eqQ1", "eqQ2", "eqQ3", "eqQ4", "eqQ5", "eqQ6", "eqQ7", "eqQ8", "eqQ9", "eqQ10", "eqQ11", "eqQ12" };

    // PhaseQ: twelve nodes (off until created), each left or right, bell / low shelf / high shelf, bypassable
    inline constexpr const char* phaseOn[12] { "phaseOn1", "phaseOn2", "phaseOn3", "phaseOn4", "phaseOn5", "phaseOn6", "phaseOn7", "phaseOn8", "phaseOn9", "phaseOn10", "phaseOn11", "phaseOn12" };
    inline constexpr const char* phaseSide[12] { "phaseSide1", "phaseSide2", "phaseSide3", "phaseSide4", "phaseSide5", "phaseSide6", "phaseSide7", "phaseSide8", "phaseSide9", "phaseSide10", "phaseSide11", "phaseSide12" };
    inline constexpr const char* phaseBypass[12] { "phaseBypass1", "phaseBypass2", "phaseBypass3", "phaseBypass4", "phaseBypass5", "phaseBypass6", "phaseBypass7", "phaseBypass8", "phaseBypass9", "phaseBypass10", "phaseBypass11", "phaseBypass12" };
    inline constexpr const char* phaseShape[12] { "phaseShape1", "phaseShape2", "phaseShape3", "phaseShape4", "phaseShape5", "phaseShape6", "phaseShape7", "phaseShape8", "phaseShape9", "phaseShape10", "phaseShape11", "phaseShape12" };
    inline constexpr const char* phaseFreq[12] { "phaseFreq1", "phaseFreq2", "phaseFreq3", "phaseFreq4", "phaseFreq5", "phaseFreq6", "phaseFreq7", "phaseFreq8", "phaseFreq9", "phaseFreq10", "phaseFreq11", "phaseFreq12" };
    inline constexpr const char* phaseAmount[12] { "phaseAmount1", "phaseAmount2", "phaseAmount3", "phaseAmount4", "phaseAmount5", "phaseAmount6", "phaseAmount7", "phaseAmount8", "phaseAmount9", "phaseAmount10", "phaseAmount11", "phaseAmount12" };
    inline constexpr const char* phaseQ[12] { "phaseQ1", "phaseQ2", "phaseQ3", "phaseQ4", "phaseQ5", "phaseQ6", "phaseQ7", "phaseQ8", "phaseQ9", "phaseQ10", "phaseQ11", "phaseQ12" };

    // Limiter
    inline constexpr const char* limiterOn       = "limiterOn";
    inline constexpr const char* limiterTruePeak = "limiterTruePeak";
    inline constexpr const char* limiterInput    = "limiterInput";
    inline constexpr const char* limiterCeiling  = "limiterCeiling";
    inline constexpr const char* limiterRelease  = "limiterRelease";
    inline constexpr const char* limiterLink     = "limiterLink";          // channel link of the transients
    inline constexpr const char* limiterLinkRelease = "limiterLinkRelease";
    inline constexpr const char* limiterLookahead = "limiterLookahead";
    inline constexpr const char* limiterAttack   = "limiterAttack";
    inline constexpr const char* limiterAutoRelease = "limiterAutoRelease";
    inline constexpr const char* limiterOversampling = "limiterOversampling";
    inline constexpr const char* limiterDc       = "limiterDc";
    inline constexpr const char* limiterUnity    = "limiterUnity";
    inline constexpr const char* limiterDither   = "limiterDither";
    inline constexpr const char* limiterShaping  = "limiterShaping";

    // Bypass per stage (MatchEngine::Stage order)
    inline constexpr const char* stageBypass[] { "bypassEqMatch", "bypassDynamics", "bypassAnalog", "bypassPhaseQ", "bypassGraphicEq", "bypassLimiter" };
}

//==============================================================================
// UlMaTo - Ultimate Mastering Tool
//
// A reference song is analysed (long-term spectrum of L/R, mid/side and the phase
// between the channels per band). The input of the plugin can be learned in the same
// way (LEARN); MATCH then moves the signal towards the reference with a static
// correction (learned input -> reference) followed by a dynamic one that follows
// the music. The dynamics of the reference in five bands are matched by a multiband
// compressor / expander with its own amount (DYNAMICS). ANALOG adds tape, wow/flutter,
// vinyl crackle, a tube and a transformer stage; PHASEQ changes only the phase, GRAPHIC EQ
// is a linear-phase EQ, LIMITER a lookahead limiter. The six stages run in any order.
// See MatchEngine, Dynamics, Analog, ToneStages and Limiter.
// Bypass: cross-fade to the input, delayed by the latency.
//==============================================================================
class UlMaToProcessor final : public juce::AudioProcessor
{
public:
    UlMaToProcessor();
    ~UlMaToProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                        { return true; }

    const juce::String getName() const override            { return JucePlugin_Name; }
    bool acceptsMidi() const override                      { return false; }
    bool producesMidi() const override                     { return false; }
    bool isMidiEffect() const override                     { return false; }
    double getTailLengthSeconds() const override           { return 0.0; }

    int getNumPrograms() override                          { return 1; }
    int getCurrentProgram() override                       { return 0; }
    void setCurrentProgram (int) override                  {}
    const juce::String getProgramName (int) override       { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    juce::AudioProcessorParameter* getBypassParameter() const override   { return bypassParam; }

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==========================================================================
    // Reference song (message thread)
    juce::AudioFormatManager& getFormatManager()           { return formatManager; }
    void setReferenceFile (const juce::File& file);
    juce::File getReferenceFile() const;
    void setReferenceRange (double startSeconds, double endSeconds);
    juce::Range<double> getReferenceRange() const;

    // Analyses the selected range in the background
    void analyseReference();
    bool isAnalysing() const noexcept                      { return analysing.load(); }
    float getAnalysisProgress() const noexcept             { return analysisProgress.load(); }

    // Result of the last analysis (any thread except audio); the version increases
    // with every new result, so the display knows when to update
    Spectrum::Profile getReferenceProfile() const;
    Dynamics::Profile getReferenceDynamics() const;
    int getReferenceVersion() const noexcept               { return referenceVersion.load(); }

    // LEARN: analyse the input of the plugin while switched on
    void setLearning (bool shouldLearn)                    { engine.setLearning (shouldLearn); }
    bool isLearning() const noexcept                       { return engine.isLearning(); }
    Spectrum::Profile getLearnedProfile() const            { return engine.getLearned(); }
    Dynamics::Profile getLearnedDynamics() const           { return engine.getLearnedDynamics(); }
    int getLearnedVersion() const noexcept                 { return engine.getLearnedVersion(); }

    // Spectrum of the processed signal (for the display)
    Spectrum::Profile getOutputProfile() const             { return engine.getOutputProfile(); }

    // Order of the stages (message thread; stored with the project)
    void setStageOrder (const MatchEngine::Order& order);
    MatchEngine::Order getStageOrder() const;

    // Limiter meters
    float getLimiterReductionDb() const noexcept           { return engine.getLimiterReductionDb(); }
    Limit::Meters getLimiterMeters() const noexcept        { return engine.getLimiterMeters(); }
    float getAnalogVuDb (int channel) const noexcept        { return engine.getAnalogVuDb (channel); }
    int readLimiterColumns (Limit::Column* destination, int maxCount) noexcept   { return engine.readLimiterColumns (destination, maxCount); }
    void resetLimiterLoudness() noexcept                   { engine.resetLimiterLoudness(); }

    // Dynamics: live input / output distributions, slopes and current gain per band
    MatchEngine::DynamicsState getDynamicsState() const    { return engine.getDynamicsState(); }
    float getDynamicsGainDb (int band) const noexcept      { return engine.getDynamicsGainDb (band); }

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void setReferenceProfile (const Spectrum::Profile& profile, const Dynamics::Profile& dynamics);

    juce::AudioFormatManager formatManager;
    MatchEngine engine;
    juce::AudioParameterBool* bypassParam = nullptr;

    // Raw values of the parameters, looked up once
    using Raw = std::atomic<float>*;
    struct NodeParams    { Raw on, bypass, shape, side, freq, value, q; };   // side: PhaseQ only
    struct AnalogParams  { Raw drive, wowDepth, wowRate, flutterDepth, flutterRate, crackle, hum, hum60, tube, transformer; };
    struct LimiterParams { Raw on, truePeak, input, ceiling, release, autoRelease, linkTransients, linkRelease, attack, lookahead,
                           oversampling, dc, unity, dither, shaping; };
    struct
    {
        Raw amount = nullptr, response = nullptr, dynamics = nullptr;
        std::array<Raw, Dynamics::numBands> tape {};
        AnalogParams analog {};
        std::array<NodeParams, Tone::numBands> eq {};
        std::array<NodeParams, Tone::numPhaseNodes> phase {};
        LimiterParams limiter {};
        std::array<Raw, MatchEngine::numStages> stageBypass {};
    } params;

    // Settings of the other stages, read from the parameters for every block
    MatchEngine::StageSettings readStageSettings() const;

    // Bypass: the input delayed by the latency, cross-fade over 20 ms
    juce::AudioBuffer<float> dryDelay, dryBlock;
    int dryPosition = 0;
    juce::SmoothedValue<float> bypassMix;

    mutable juce::CriticalSection referenceLock;
    Spectrum::Profile referenceProfile;
    Dynamics::Profile referenceDynamics;
    std::atomic<int> referenceVersion { 0 };

    juce::ThreadPool analysisPool { juce::ThreadPoolOptions{}.withThreadName ("UlMaTo analysis").withNumberOfThreads (1) };
    std::atomic<bool> analysing { false }, shuttingDown { false };
    std::atomic<float> analysisProgress { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UlMaToProcessor)
};
