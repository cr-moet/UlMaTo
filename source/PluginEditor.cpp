#include "PluginEditor.h"

namespace
{
    constexpr int headerHeight = 52;
}

UlMaToEditor::UlMaToEditor (UlMaToProcessor& p)
    : AudioProcessorEditor (p), processor (p)
{
    setLookAndFeel (&lookAndFeel);

    addAndMakeVisible (waveform);
    waveform.onFileChanged = [this] { statusLabel.setText ("Select a range and press ANALYZE", juce::dontSendNotification); };

    analyseButton.setColour (juce::TextButton::buttonColourId, Colours::accent);
    analyseButton.setColour (juce::TextButton::textColourOffId, Colours::background);
    analyseButton.onClick = [this] { processor.analyseReference(); };
    addAndMakeVisible (analyseButton);

    statusLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    addAndMakeVisible (statusLabel);

    for (auto* view : std::initializer_list<juce::Component*> { &stereoView, &midSideView, &phaseView, &liveView })
        addAndMakeVisible (view);

    matchKnob.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 90, 26);
    matchKnob.setColour (juce::Slider::textBoxBackgroundColourId, Colours::text);   // LED-style value display
    matchKnob.setColour (juce::Slider::textBoxTextColourId, Colours::accent);
    addAndMakeVisible (matchKnob);
    matchLabel.setText ("MATCH", juce::dontSendNotification);
    matchLabel.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
    matchLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (matchLabel);

    setResizable (true, true);
    setResizeLimits (900, 660, 2400, 1600);
    setSize (1100, 800);

    timerCallback();
    startTimerHz (10);
}

UlMaToEditor::~UlMaToEditor()
{
    setLookAndFeel (nullptr);
}

// Analysis state and new results
void UlMaToEditor::timerCallback()
{
    const bool busy = processor.isAnalysing();
    analyseButton.setEnabled (! busy && processor.getReferenceFile().existsAsFile());

    if (busy)
        statusLabel.setText ("Analysing ... " + juce::String (juce::roundToInt (processor.getAnalysisProgress() * 100.0f)) + " %",
                             juce::dontSendNotification);

    if (const int version = processor.getReferenceVersion(); version != shownReferenceVersion)
    {
        shownReferenceVersion = version;
        const auto profile = processor.getReferenceProfile();
        for (auto* view : { &stereoView, &midSideView, &phaseView })
            view->setProfile (profile);
        liveView.setReference (profile);

        if (! busy)
            statusLabel.setText (profile.valid ? "Reference analysed" : "Drop a reference song above",
                                 juce::dontSendNotification);
    }
}

void UlMaToEditor::paint (juce::Graphics& g)
{
    g.fillAll (Colours::background);

    // Title: filled box with the name in the background colour
    const juce::Rectangle<float> box (14.0f, 10.0f, 96.0f, 34.0f);
    g.setColour (Colours::text);
    g.fillRect (box);
    g.setColour (Colours::background);
    g.setFont (juce::Font (juce::FontOptions (22.0f, juce::Font::bold)));
    g.drawText ("UlMaTo", box, juce::Justification::centred);

    g.setColour (Colours::text.withAlpha (0.8f));
    g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
    g.drawText ("ULTIMATE MASTERING TOOL", juce::Rectangle<float> (124.0f, 10.0f, 300.0f, 34.0f), juce::Justification::centredLeft);

    g.setColour (Colours::text.withAlpha (0.2f));
    g.drawHorizontalLine (headerHeight + 4, 0.0f, (float) getWidth());
}

void UlMaToEditor::resized()
{
    auto area = getLocalBounds().reduced (14, 0);
    area.removeFromTop (headerHeight + 14);
    area.removeFromBottom (12);

    // Reference song
    waveform.setBounds (area.removeFromTop (150));
    area.removeFromTop (8);
    auto controls = area.removeFromTop (32);
    analyseButton.setBounds (controls.removeFromLeft (140));
    controls.removeFromLeft (14);
    statusLabel.setBounds (controls);
    area.removeFromTop (12);

    // Analysis of the reference
    auto analysis = area.removeFromTop (juce::jmax (180, area.getHeight() * 2 / 5));
    const int w = (analysis.getWidth() - 2 * 16) / 3;
    stereoView.setBounds (analysis.removeFromLeft (w));
    analysis.removeFromLeft (16);
    midSideView.setBounds (analysis.removeFromLeft (w));
    analysis.removeFromLeft (16);
    phaseView.setBounds (analysis);
    area.removeFromTop (14);

    // Live spectrum and the MATCH knob
    auto knobArea = area.removeFromRight (220);
    area.removeFromRight (16);
    liveView.setBounds (area);

    knobArea = knobArea.withSizeKeepingCentre (knobArea.getWidth(), juce::jmin (knobArea.getHeight(), 260));
    matchLabel.setBounds (knobArea.removeFromTop (26));
    matchKnob.setBounds (knobArea);
}
