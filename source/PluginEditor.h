#pragma once

#include "Views.h"

//==============================================================================
// Layout from top to bottom: header with BYPASS, reference song (waveform + ANALYZE,
// LEARN), tabs (in processing order, can be dragged), then one of two pages:
//  EQ MATCH       - the three analysis displays, the output spectrum, MATCH / RESPONSE
//  DYNAMICS MATCH - level distributions per band, DYNAMICS knob
//==============================================================================
class UlMaToEditor final : public juce::AudioProcessorEditor,
                           private juce::Timer
{
public:
    explicit UlMaToEditor (UlMaToProcessor&);
    ~UlMaToEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void showPage (int page);

    UlMaToProcessor& processor;
    UlMaToLookAndFeel lookAndFeel;

    juce::TextButton bypassButton { "BYPASS" };
    juce::AudioProcessorValueTreeState::ButtonAttachment bypassAttachment { processor.apvts, ParamIDs::bypass, bypassButton };

    StageTabs tabs;   // page selection and order of the stages

    WaveformView waveform { processor };
    juce::TextButton analyseButton { "ANALYZE" };
    juce::Label statusLabel;

    // LEARN: analyse the plugin input (click = start, click again = stop)
    juce::TextButton learnButton { "LEARN INPUT" };
    Led learnLed;
    juce::Label learnLabel;

    ProfileView stereoView  { ProfileView::Kind::stereo,  "STEREO  (L / R)  [dBFS]" },
                midSideView { ProfileView::Kind::midSide, "MID / SIDE  [dBFS]" },
                phaseView   { ProfileView::Kind::phase,   "PHASE CORRELATION" };
    LiveView liveView { processor };

    juce::Slider matchKnob    { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow },
                 responseKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label matchLabel, responseLabel;
    juce::AudioProcessorValueTreeState::SliderAttachment matchAttachment    { processor.apvts, ParamIDs::amount,   matchKnob },
                                                         responseAttachment { processor.apvts, ParamIDs::response, responseKnob };

    // Dynamics page
    DynamicsView dynamicsView { processor };
    juce::Slider dynamicsKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label dynamicsLabel, dynamicsInfo;
    juce::AudioProcessorValueTreeState::SliderAttachment dynamicsAttachment { processor.apvts, ParamIDs::dynamics, dynamicsKnob };

    juce::Rectangle<int> tabLine;
    int currentPage = 0;
    int shownReferenceVersion = -1, shownLearnedVersion = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UlMaToEditor)
};
