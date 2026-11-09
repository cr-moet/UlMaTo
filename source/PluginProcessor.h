#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "MatchEngine.h"

namespace ParamIDs
{
    inline constexpr const char* amount = "amount";   // match amount 0 ... 100 %
}

//==============================================================================
// UlMaTo - Ultimate Mastering Tool
//
// A reference song is analysed (long-term spectrum of L/R, mid/side and the phase
// correlation per band). The signal running through the plugin is analysed
// continuously, and a linear-phase mid/side EQ moves its tonal balance and stereo
// image towards the reference by the selected amount.
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
    int getReferenceVersion() const noexcept               { return referenceVersion.load(); }

    MatchEngine::State getMatchState() const               { return engine.getState(); }

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void setReferenceProfile (const Spectrum::Profile& profile);

    juce::AudioFormatManager formatManager;
    MatchEngine engine;
    std::atomic<float>* amountParam = nullptr;

    mutable juce::CriticalSection referenceLock;
    Spectrum::Profile referenceProfile;
    std::atomic<int> referenceVersion { 0 };

    juce::ThreadPool analysisPool { juce::ThreadPoolOptions{}.withThreadName ("UlMaTo analysis").withNumberOfThreads (1) };
    std::atomic<bool> analysing { false }, shuttingDown { false };
    std::atomic<float> analysisProgress { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UlMaToProcessor)
};
