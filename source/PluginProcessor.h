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
}

//==============================================================================
// UlMaTo - Ultimate Mastering Tool
//
// A reference song is analysed (long-term spectrum of L/R, mid/side and the phase
// between the channels per band). The input of the plugin can be learned in the same
// way (LEARN); MATCH then moves the signal towards the reference with a static
// correction (learned input -> reference) followed by a dynamic one that follows
// the music. The dynamics of the reference in five bands are matched by a multiband
// compressor / expander with its own amount (DYNAMICS); EQ and dynamics can run in
// either order. See MatchEngine and Dynamics.
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
    void setDynamicsFirst (bool first);
    bool isDynamicsFirst() const;

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
