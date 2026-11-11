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
//  LIMITER     - scrolling history and meters; gain, output, lookahead, attack, release, links,
//                auto release, oversampling, dither, true peak / RMS, DC, unity gain
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
    std::array<LabelledKnob, 7> heatKnobs;   // wow depth / rate, flutter depth / rate, crackle, tube, transformer
    juce::Rectangle<int> tapePanel, transportPanel, outputPanel;
    std::vector<std::pair<int, juce::Rectangle<int>>> controlPanels;   // page, raised screwed panel around its controls

    // PhaseQ, graphic EQ, limiter pages
    // PhaseQ: one graph for both channels, with the box of the selected node inside
    PhaseQView phaseQView { processorRef };

    GraphicEqView graphicEqView { processorRef };

    LimiterView limiterView { processorRef };
    std::array<LabelledKnob, 7> limiterKnobs;   // gain, output, lookahead, attack, release, link transients / release
    std::array<juce::TextButton, 5> limiterButtons;   // on, true peak, DC filter, unity gain, auto release
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>, 5> limiterButtonAttachments;
    std::array<juce::ComboBox, 3> limiterMenus;       // oversampling, dither, noise shaping
    std::array<juce::Label, 3> limiterMenuLabels;
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>, 3> limiterMenuAttachments;

    std::array<std::vector<juce::Component*>, MatchEngine::numStages> pages;
    juce::Label matchInfo;   // what the MATCH range does right now
    juce::Rectangle<int> tabLine;
    int currentPage = 0;
    int shownReferenceVersion = -1, shownLearnedVersion = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UlMaToEditor)
};
