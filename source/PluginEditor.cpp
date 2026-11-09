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

    learnButton.setClickingTogglesState (true);
    learnButton.setToggleState (processor.isLearning(), juce::dontSendNotification);
    learnButton.onClick = [this] { processor.setLearning (learnButton.getToggleState()); };
    addAndMakeVisible (learnButton);
    addAndMakeVisible (learnLed);
    learnLabel.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
    addAndMakeVisible (learnLabel);

    for (auto* view : std::initializer_list<juce::Component*> { &stereoView, &midSideView, &phaseView, &liveView })
        addAndMakeVisible (view);

    // MATCH: how much of the correction is applied; RESPONSE: how fast it follows the music
    for (auto [knob, label, text, size] : { std::tuple { &matchKnob, &matchLabel, "MATCH", 16.0f },
                                            std::tuple { &responseKnob, &responseLabel, "RESPONSE", 12.0f } })
    {
        knob->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 90, 24);
        knob->setColour (juce::Slider::textBoxBackgroundColourId, Colours::text);   // LED-style value display
        knob->setColour (juce::Slider::textBoxTextColourId, Colours::accent);
        addAndMakeVisible (knob);
        label->setText (text, juce::dontSendNotification);
        label->setFont (juce::Font (juce::FontOptions (size, juce::Font::bold)));
        label->setJustificationType (juce::Justification::centred);
        addAndMakeVisible (label);
    }
    responseKnob.setTooltip ("Time constant of the live analysis: short follows the music quickly, long is steadier");

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

    // LEARN: button text and LED
    const bool learning = learnButton.getToggleState();
    const auto learned  = processor.getLearnedProfile();
    learnButton.setButtonText (learning ? "STOP LEARNING" : "LEARN INPUT");
    learnLed.setState (learning ? Led::State::blinking : learned.valid ? Led::State::on : Led::State::off);
    learnLabel.setText (learning ? "LEARNING INPUT ..." : learned.valid ? "INPUT ANALYSED" : "INPUT NOT ANALYSED",
                        juce::dontSendNotification);

    const int refVersion = processor.getReferenceVersion(), learnedVersion = processor.getLearnedVersion();
    if (refVersion != shownReferenceVersion || learnedVersion != shownLearnedVersion)
    {
        const auto profile = processor.getReferenceProfile();
        for (auto* view : { &stereoView, &midSideView, &phaseView })
            view->setProfiles (profile, learned);
        liveView.setReference (profile);

        if (! busy && refVersion != shownReferenceVersion)
            statusLabel.setText (profile.valid ? "Reference analysed" : "Drop a reference song above",
                                 juce::dontSendNotification);

        shownReferenceVersion = refVersion;
        shownLearnedVersion   = learnedVersion;
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
    learnLabel.setBounds (controls.removeFromRight (150));
    learnLed.setBounds (controls.removeFromRight (24));
    controls.removeFromRight (6);
    learnButton.setBounds (controls.removeFromRight (150));
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

    // Live spectrum, next to it MATCH (large) and RESPONSE (small) side by side
    auto knobs = area.removeFromRight (330);
    area.removeFromRight (16);
    liveView.setBounds (area);

    auto match = knobs.removeFromLeft (200);
    matchLabel.setBounds (match.removeFromTop (24));
    matchKnob.setBounds (match.withSizeKeepingCentre (match.getWidth(), juce::jmin (match.getHeight(), 240)));

    auto response = knobs.withTrimmedTop (knobs.getHeight() / 2 - 40);
    responseLabel.setBounds (response.removeFromTop (20));
    responseKnob.setBounds (response.removeFromTop (juce::jmin (130, response.getHeight())));
}
