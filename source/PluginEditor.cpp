#include "PluginEditor.h"

namespace
{
    constexpr int headerHeight = 52;
    const juce::Identifier pageProperty ("page");   // selected tab, stored with the plugin state
}

UlMaToEditor::UlMaToEditor (UlMaToProcessor& p)
    : AudioProcessorEditor (p), processor (p)
{
    setLookAndFeel (&lookAndFeel);

    bypassButton.setClickingTogglesState (true);
    bypassButton.getProperties().set ("led", true);
    bypassButton.getProperties().set ("ledInverted", true);   // lit = plugin active
    addAndMakeVisible (bypassButton);

    // Tabs: click = page, drag = order of the stages
    tabs.onSelect = [this] (int page) { showPage (page); };
    tabs.onOrderChange = [this] (const MatchEngine::Order& order)
    {
        processor.setStageOrder (order);
        tabs.setState (currentPage, order, bypassedStages());
    };
    tabs.onBypassToggle = [this] (int stage)
    {
        auto* p = processor.apvts.getParameter (ParamIDs::stageBypass[stage]);
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->getValue() > 0.5f ? 0.0f : 1.0f);
        p->endChangeGesture();
        tabs.setState (currentPage, processor.getStageOrder(), bypassedStages());
    };
    addAndMakeVisible (tabs);

    addAndMakeVisible (waveform);
    waveform.onFileChanged = [this] { statusLabel.setText ("Select a range and press ANALYZE", juce::dontSendNotification); };

    analyseButton.setColour (juce::TextButton::textColourOffId, Colours::accent);
    analyseButton.onClick = [this] { processor.analyseReference(); };
    addAndMakeVisible (analyseButton);

    statusLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    addAndMakeVisible (statusLabel);

    learnButton.setClickingTogglesState (true);
    learnButton.getProperties().set ("led", true);
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

    // Dynamics page
    addChildComponent (dynamicsView);
    dynamicsKnob.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 90, 24);
    dynamicsKnob.setColour (juce::Slider::textBoxBackgroundColourId, Colours::text);
    dynamicsKnob.setColour (juce::Slider::textBoxTextColourId, Colours::accent);
    addChildComponent (dynamicsKnob);
    dynamicsLabel.setText ("DYNAMICS", juce::dontSendNotification);
    dynamicsLabel.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
    dynamicsLabel.setJustificationType (juce::Justification::centred);
    addChildComponent (dynamicsLabel);
    dynamicsInfo.setFont (juce::Font (juce::FontOptions (11.0f)));
    dynamicsInfo.setJustificationType (juce::Justification::centredTop);
    dynamicsInfo.setColour (juce::Label::textColourId, Colours::text.withAlpha (0.7f));
    addChildComponent (dynamicsInfo);

    // Analog heat page
    const auto setUpKnobOn = [this] (int page, LabelledKnob& k, const char* id, const juce::String& text)
    {
        k.knob.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 96, 20);
        k.knob.setColour (juce::Slider::textBoxBackgroundColourId, Colours::text);
        k.knob.setColour (juce::Slider::textBoxTextColourId, Colours::accent);
        k.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (processor.apvts, id, k.knob);
        k.label.setText (text, juce::dontSendNotification);
        k.label.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        k.label.setJustificationType (juce::Justification::centred);
        addChildComponent (k.knob);
        addChildComponent (k.label);
        pages[(size_t) page].insert (pages[(size_t) page].end(), { &k.knob, &k.label });
    };
    const auto setUpKnob = [&] (LabelledKnob& k, const char* id, const juce::String& text) { setUpKnobOn (MatchEngine::analogStage, k, id, text); };
    for (size_t b = 0; b < tapeKnobs.size(); ++b)
        setUpKnob (tapeKnobs[b], ParamIDs::tape[b], juce::String (Dynamics::bandNames[b]) + "\n" + Dynamics::bandRanges[b]);
    setUpKnob (heatKnobs[0], ParamIDs::wow,         "WOW");
    setUpKnob (heatKnobs[1], ParamIDs::flutter,     "FLUTTER");
    setUpKnob (heatKnobs[2], ParamIDs::crackle,     "VINYL CRACKLE");
    setUpKnob (heatKnobs[3], ParamIDs::tube,        "TUBE STAGE");
    setUpKnob (heatKnobs[4], ParamIDs::transformer, "TRANSFORMER");
    for (auto& k : tapeKnobs)
        k.label.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));

    // PhaseQ: nodes, rotation, mode
    addChildComponent (phaseQView);
    setUpKnobOn (MatchEngine::phaseStage, rotationKnob, ParamIDs::phaseRotation, "ROTATION");
    phaseModeBox.addItemList (Tone::phaseModeNames, 1);
    phaseModeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (processor.apvts, ParamIDs::phaseMode, phaseModeBox);
    addChildComponent (phaseModeBox);
    phaseModeLabel.setText ("APPLY TO", juce::dontSendNotification);
    phaseModeLabel.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
    phaseModeLabel.setJustificationType (juce::Justification::centred);
    addChildComponent (phaseModeLabel);
    pages[MatchEngine::phaseStage].insert (pages[MatchEngine::phaseStage].end(), { &phaseQView, &phaseModeBox, &phaseModeLabel });

    // Graphic EQ
    addChildComponent (graphicEqView);
    pages[MatchEngine::graphicEqStage] = { &graphicEqView };

    // Limiter
    addChildComponent (limiterView);
    setUpKnobOn (MatchEngine::limiterStage, limiterKnobs[0], ParamIDs::limiterInput,   "INPUT GAIN");
    setUpKnobOn (MatchEngine::limiterStage, limiterKnobs[1], ParamIDs::limiterCeiling, "CEILING");
    setUpKnobOn (MatchEngine::limiterStage, limiterKnobs[2], ParamIDs::limiterRelease, "RELEASE");
    setUpKnobOn (MatchEngine::limiterStage, limiterKnobs[3], ParamIDs::limiterLink,    "STEREO LINK");
    for (auto* b : { &limiterOnButton, &truePeakButton })
    {
        b->setClickingTogglesState (true);
        b->getProperties().set ("led", true);
        addChildComponent (b);
    }
    pages[MatchEngine::limiterStage].insert (pages[MatchEngine::limiterStage].end(), { &limiterView, &limiterOnButton, &truePeakButton });

    pages[MatchEngine::eqStage] = { &stereoView, &midSideView, &phaseView, &liveView, &matchKnob, &responseKnob, &matchLabel, &responseLabel };
    pages[MatchEngine::dynamicsStage] = { &dynamicsView, &dynamicsKnob, &dynamicsLabel, &dynamicsInfo };

    setResizable (true, true);
    setResizeLimits (900, 700, 2400, 1600);
    setSize (1100, 840);
    showPage ((int) processor.apvts.state.getProperty (pageProperty, 0));
    sendLookAndFeelChange();   // the value boxes of the knobs were created before the look was set

    timerCallback();
    startTimerHz (10);
}

UlMaToEditor::~UlMaToEditor()
{
    setLookAndFeel (nullptr);
}

void UlMaToEditor::showPage (int page)
{
    currentPage = juce::jlimit (0, (int) pages.size() - 1, page);
    tabs.setState (currentPage, processor.getStageOrder(), bypassedStages());
    processor.apvts.state.setProperty (pageProperty, currentPage, nullptr);

    for (size_t p = 0; p < pages.size(); ++p)
        for (auto* c : pages[p])
            c->setVisible ((int) p == currentPage);
    repaint();
}

StageTabs::Bypassed UlMaToEditor::bypassedStages() const
{
    StageTabs::Bypassed b {};
    for (size_t stage = 0; stage < b.size(); ++stage)
        b[stage] = processor.apvts.getRawParameterValue (ParamIDs::stageBypass[stage])->load() > 0.5f;
    return b;
}

// Analysis state and new results
void UlMaToEditor::timerCallback()
{
    const bool busy = processor.isAnalysing();
    analyseButton.setEnabled (! busy && processor.getReferenceFile().existsAsFile());

    if (busy)
        statusLabel.setText ("Analysing ... " + juce::String (juce::roundToInt (processor.getAnalysisProgress() * 100.0f)) + " %",
                             juce::dontSendNotification);

    tabs.setState (currentPage, processor.getStageOrder(), bypassedStages());   // order / bypass may come from the host or a project

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
        dynamicsView.setReference (processor.getReferenceDynamics());

        dynamicsInfo.setText (! processor.getReferenceDynamics().valid
                                  ? "No dynamics of the reference yet:\npress ANALYZE"
                                  : processor.getLearnedDynamics().valid
                                        ? "Input dynamics: learned (LEARN)"
                                        : "Input dynamics: live\n(last 15 s) - use LEARN for\na fixed ratio per band",
                              juce::dontSendNotification);

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

    // Line under the tabs, the page belongs to the selected tab
    g.setColour (Colours::text);
    g.fillRect (tabLine);

    // Analog heat: three groups
    if (currentPage == MatchEngine::analogStage)
        for (auto [r, title] : { std::pair { tapePanel, "TAPE SATURATION PER BAND  (stereo: random drive per sample, left and right)" },
                                 std::pair { transportPanel, "TAPE TRANSPORT / VINYL" },
                                 std::pair { outputPanel, "OUTPUT STAGES" } })
        {
            Plot::drawPanel (g, r.toFloat());
            g.setColour (Colours::text);
            g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
            g.drawText (title, r.reduced (10, 6).removeFromTop (16), juce::Justification::centredLeft);
        }
}

void UlMaToEditor::resized()
{
    bypassButton.setBounds (getWidth() - 14 - 110, 8, 110, 38);

    auto area = getLocalBounds().reduced (14, 0);
    area.removeFromTop (headerHeight + 14);
    area.removeFromBottom (12);

    // Reference song
    waveform.setBounds (area.removeFromTop (150));
    area.removeFromTop (8);
    auto controls = area.removeFromTop (38);
    analyseButton.setBounds (controls.removeFromLeft (140));
    controls.removeFromLeft (14);
    learnLabel.setBounds (controls.removeFromRight (150));
    learnLed.setBounds (controls.removeFromRight (24));
    controls.removeFromRight (6);
    learnButton.setBounds (controls.removeFromRight (150));
    statusLabel.setBounds (controls);
    area.removeFromTop (12);

    // Tabs
    tabs.setBounds (area.removeFromTop (36));
    tabLine = area.removeFromTop (2);
    area.removeFromTop (12);

    // Analog heat page: tape knobs in a row, below transport / vinyl and the output stages
    {
        auto page = area;
        tapePanel = page.removeFromTop (page.getHeight() / 2 - 6);
        page.removeFromTop (12);
        transportPanel = page.removeFromLeft (page.getWidth() * 3 / 5 - 6);
        page.removeFromLeft (12);
        outputPanel = page;

        const auto placeRow = [] (juce::Rectangle<int> r, auto& knobs, size_t first, size_t count)
        {
            r = r.reduced (10).withTrimmedTop (22);
            const int w = r.getWidth() / (int) count;
            for (size_t i = first; i < first + count; ++i)
            {
                auto cell = r.removeFromLeft (w);
                knobs[i].label.setBounds (cell.removeFromTop (32));
                knobs[i].knob.setBounds (cell.withSizeKeepingCentre (juce::jmin (cell.getWidth(), 150), juce::jmin (cell.getHeight(), 150)));
            }
        };
        placeRow (tapePanel, tapeKnobs, 0, tapeKnobs.size());
        placeRow (transportPanel, heatKnobs, 0, 3);
        placeRow (outputPanel, heatKnobs, 3, 2);
    }

    // PhaseQ page: the nodes, rotation and mode on the right
    {
        auto page = area;
        auto side = page.removeFromRight (200);
        page.removeFromRight (16);
        phaseQView.setBounds (page);
        side.removeFromTop (24);
        rotationKnob.label.setBounds (side.removeFromTop (20));
        rotationKnob.knob.setBounds (side.removeFromTop (juce::jmin (180, side.getHeight() / 2)));
        side.removeFromTop (20);
        phaseModeLabel.setBounds (side.removeFromTop (20));
        phaseModeBox.setBounds (side.removeFromTop (28).reduced (20, 0));
    }

    // Graphic EQ page
    graphicEqView.setBounds (area);

    // Limiter page: history above, controls below
    {
        auto page = area;
        auto knobRow = page.removeFromBottom (juce::jmin (210, page.getHeight() / 2));
        page.removeFromBottom (12);
        limiterView.setBounds (page);

        auto buttons = knobRow.removeFromRight (180).withSizeKeepingCentre (160, 90);
        limiterOnButton.setBounds (buttons.removeFromTop (40));
        buttons.removeFromTop (10);
        truePeakButton.setBounds (buttons.removeFromTop (40));

        const int w = knobRow.getWidth() / (int) limiterKnobs.size();
        for (auto& k : limiterKnobs)
        {
            auto cell = knobRow.removeFromLeft (w);
            k.label.setBounds (cell.removeFromTop (22));
            k.knob.setBounds (cell.withSizeKeepingCentre (juce::jmin (cell.getWidth(), 160), juce::jmin (cell.getHeight(), 170)));
        }
    }

    // Dynamics page: the bands, the DYNAMICS knob on the right
    {
        auto page = area;
        auto knob = page.removeFromRight (200);
        page.removeFromRight (16);
        dynamicsView.setBounds (page);
        dynamicsLabel.setBounds (knob.removeFromTop (24));
        dynamicsKnob.setBounds (knob.removeFromTop (juce::jmin (knob.getHeight() - 70, 240)));
        knob.removeFromTop (10);
        dynamicsInfo.setBounds (knob);
    }

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
