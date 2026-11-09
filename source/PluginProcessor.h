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
    inline constexpr const char* wow         = "wow";
    inline constexpr const char* flutter     = "flutter";
    inline constexpr const char* crackle     = "crackle";
    inline constexpr const char* tube        = "tube";
    inline constexpr const char* transformer = "transformer";

    // Graphic EQ: six bands
    inline constexpr const char* eqFreq[] { "eqFreq1", "eqFreq2", "eqFreq3", "eqFreq4", "eqFreq5", "eqFreq6" };
    inline constexpr const char* eqGain[] { "eqGain1", "eqGain2", "eqGain3", "eqGain4", "eqGain5", "eqGain6" };
    inline constexpr const char* eqQ[]    { "eqQ1", "eqQ2", "eqQ3", "eqQ4", "eqQ5", "eqQ6" };

    // PhaseQ: six nodes, rotation, mode
    inline constexpr const char* phaseFreq[]   { "phaseFreq1", "phaseFreq2", "phaseFreq3", "phaseFreq4", "phaseFreq5", "phaseFreq6" };
    inline constexpr const char* phaseAmount[] { "phaseAmount1", "phaseAmount2", "phaseAmount3", "phaseAmount4", "phaseAmount5", "phaseAmount6" };
    inline constexpr const char* phaseWidth[]  { "phaseWidth1", "phaseWidth2", "phaseWidth3", "phaseWidth4", "phaseWidth5", "phaseWidth6" };
    inline constexpr const char* phaseRotation = "phaseRotation";
    inline constexpr const char* phaseMode     = "phaseMode";

    // Limiter
    inline constexpr const char* limiterOn       = "limiterOn";
    inline constexpr const char* limiterTruePeak = "limiterTruePeak";
    inline constexpr const char* limiterInput    = "limiterInput";
    inline constexpr const char* limiterCeiling  = "limiterCeiling";
    inline constexpr const char* limiterRelease  = "limiterRelease";
    inline constexpr const char* limiterLink     = "limiterLink";

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
    float getLimiterOutputDb() const noexcept              { return engine.getLimiterOutputDb(); }

    // Dynamics: live input / output distributions, slopes and current gain per band
    MatchEngine::DynamicsState getDynamicsState() const    { return engine.getDynamicsState(); }
    float getDynamicsGainDb (int band) const noexcept      { return engine.getDynamicsGainDb (band); }

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void setReferenceProfile (const Spectrum::Profile& profile, const Dynamics::Profile& dynamics);

    juce::AudioFormatManager formatManager;
    MatchEngine engine;
    std::atomic<float>* amountParam = nullptr;
    std::atomic<float>* responseParam = nullptr;
    std::atomic<float>* dynamicsParam = nullptr;
    juce::AudioParameterBool* bypassParam = nullptr;
    std::array<std::atomic<float>*, Dynamics::numBands> tapeParams {};
    std::atomic<float>* wowParam = nullptr;
    std::atomic<float>* flutterParam = nullptr;
    std::atomic<float>* crackleParam = nullptr;
    std::atomic<float>* tubeParam = nullptr;
    std::atomic<float>* transformerParam = nullptr;

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
