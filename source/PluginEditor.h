#pragma once

#include "ToneViews.h"

//==============================================================================
// Layout from top to bottom: header with BYPASS, reference song (waveform + ANALYZE,
// LEARN), tabs (in processing order, can be dragged), then one page per stage:
//  EQ MATCH    - the three analysis displays, the output spectrum, MATCH / RESPONSE
//  DYNAMICS    - level distributions per band, DYNAMICS knob
//  ANALOG HEAT - tape saturation per band, wow, flutter, crackle, tube, transformer
//  PHASEQ      - left and right in one graph, nodes created by double-click
//  GRAPHIC EQ  - six band nodes
//  LIMITER     - reduction / output history, input, ceiling, release, link
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
    StageTabs::Bypassed bypassedStages() const;

    UlMaToProcessor& processorRef;
    UlMaToLookAndFeel lookAndFeel;

    juce::TextButton bypassButton { "BYPASS" };
    juce::AudioProcessorValueTreeState::ButtonAttachment bypassAttachment { processorRef.apvts, ParamIDs::bypass, bypassButton };

    StageTabs tabs;   // page selection and order of the stages

    WaveformView waveform { processorRef };
    juce::TextButton analyseButton { "ANALYZE" };
    juce::Label statusLabel;

    // LEARN: analyse the plugin input (click = start, click again = stop)
    juce::TextButton learnButton { "LEARN INPUT" };
    Led learnLed;
    juce::Label learnLabel;

    ProfileView stereoView  { ProfileView::Kind::stereo,  "STEREO  (L / R)  [dBFS]" },
                midSideView { ProfileView::Kind::midSide, "MID / SIDE  [dBFS]" },
                phaseView   { ProfileView::Kind::phase,   "PHASE CORRELATION" };
    LiveView liveView { processorRef };

    juce::Slider matchKnob    { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow },
                 responseKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label matchLabel, responseLabel;
    juce::AudioProcessorValueTreeState::SliderAttachment matchAttachment    { processorRef.apvts, ParamIDs::amount,   matchKnob },
                                                         responseAttachment { processorRef.apvts, ParamIDs::response, responseKnob };

    // Dynamics page
    DynamicsView dynamicsView { processorRef };
    juce::Slider dynamicsKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label dynamicsLabel, dynamicsInfo;
    juce::AudioProcessorValueTreeState::SliderAttachment dynamicsAttachment { processorRef.apvts, ParamIDs::dynamics, dynamicsKnob };

    // Analog heat page: a knob with its name above it
    struct LabelledKnob
    {
        juce::Slider knob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };
    std::array<LabelledKnob, Dynamics::numBands> tapeKnobs;
    std::array<LabelledKnob, 5> heatKnobs;   // wow, flutter, crackle, tube, transformer
    juce::Rectangle<int> tapePanel, transportPanel, outputPanel;

    // PhaseQ, graphic EQ, limiter pages
    // PhaseQ: one graph for both channels, with the box of the selected node inside
    PhaseQView phaseQView { processorRef };

    GraphicEqView graphicEqView { processorRef };

    LimiterView limiterView { processorRef };
    std::array<LabelledKnob, 4> limiterKnobs;   // input, ceiling, release, link
    juce::TextButton limiterOnButton { "LIMITER ON" }, truePeakButton { "TRUE PEAK" };
    juce::AudioProcessorValueTreeState::ButtonAttachment limiterOnAttachment { processorRef.apvts, ParamIDs::limiterOn, limiterOnButton },
                                                         truePeakAttachment  { processorRef.apvts, ParamIDs::limiterTruePeak, truePeakButton };

    std::array<std::vector<juce::Component*>, MatchEngine::numStages> pages;
    juce::Label matchInfo;   // what the MATCH range does right now
    juce::Rectangle<int> tabLine;
    int currentPage = 0;
    int shownReferenceVersion = -1, shownLearnedVersion = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UlMaToEditor)
};
