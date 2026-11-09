#pragma once

#include "Views.h"

//==============================================================================
// Layout from top to bottom: reference song (waveform + ANALYZE), the three
// analysis displays, the live spectrum with the MATCH knob next to it
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

    UlMaToProcessor& processor;
    UlMaToLookAndFeel lookAndFeel;

    WaveformView waveform { processor };
    juce::TextButton analyseButton { "ANALYZE" };
    juce::Label statusLabel;

    ProfileView stereoView  { ProfileView::Kind::stereo,  "STEREO  (L / R)  [dBFS]" },
                midSideView { ProfileView::Kind::midSide, "MID / SIDE  [dBFS]" },
                phaseView   { ProfileView::Kind::phase,   "PHASE CORRELATION" };
    LiveView liveView { processor };

    juce::Slider matchKnob    { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow },
                 responseKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label matchLabel, responseLabel;
    juce::AudioProcessorValueTreeState::SliderAttachment matchAttachment    { processor.apvts, ParamIDs::amount,   matchKnob },
                                                         responseAttachment { processor.apvts, ParamIDs::response, responseKnob };

    int shownReferenceVersion = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UlMaToEditor)
};
