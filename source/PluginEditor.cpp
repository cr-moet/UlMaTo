#include "PluginEditor.h"

namespace
{
    constexpr int headerHeight = 52;
    const juce::Identifier pageProperty ("page");   // selected tab, stored with the plugin state
}

UlMaToEditor::UlMaToEditor (UlMaToProcessor& p)
    : AudioProcessorEditor (p), processorRef (p)
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
        processorRef.setStageOrder (order);
        tabs.setState (currentPage, order, bypassedStages());
    };
    tabs.onBypassToggle = [this] (int stage)
    {
        auto* parameter = processorRef.apvts.getParameter (ParamIDs::stageBypass[stage]);
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost (parameter->getValue() > 0.5f ? 0.0f : 1.0f);
        parameter->endChangeGesture();
        tabs.setState (currentPage, processorRef.getStageOrder(), bypassedStages());
    };
    addAndMakeVisible (tabs);

    addAndMakeVisible (waveform);
    waveform.onFileChanged = [this] { statusLabel.setText ("Select a range and press ANALYZE", juce::dontSendNotification); };

    analyseButton.setColour (juce::TextButton::textColourOffId, Colours::accent);
    analyseButton.onClick = [this] { processorRef.analyseReference(); };
    addAndMakeVisible (analyseButton);

    statusLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    addAndMakeVisible (statusLabel);

    learnButton.setClickingTogglesState (true);
    learnButton.getProperties().set ("led", true);
    learnButton.setToggleState (processorRef.isLearning(), juce::dontSendNotification);
    learnButton.onClick = [this] { processorRef.setLearning (learnButton.getToggleState()); };
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
        k.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (processorRef.apvts, id, k.knob);
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
    setUpKnob (heatKnobs[0], ParamIDs::wowDepth,     "WOW DEPTH");
    setUpKnob (heatKnobs[1], ParamIDs::wowRate,      "WOW RATE");
    setUpKnob (heatKnobs[2], ParamIDs::flutterDepth, "FLUTTER DEPTH");
    setUpKnob (heatKnobs[3], ParamIDs::flutterRate,  "FLUTTER RATE");
    setUpKnob (heatKnobs[4], ParamIDs::crackle,      "VINYL CRACKLE");
    setUpKnob (heatKnobs[5], ParamIDs::tube,         "TUBE STAGE");
    setUpKnob (heatKnobs[6], ParamIDs::transformer,  "TRANSFORMER");
    for (auto& k : tapeKnobs)
        k.label.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));

    // PhaseQ
    addChildComponent (phaseQView);
    pages[MatchEngine::phaseStage] = { &phaseQView };

    // Graphic EQ
    addChildComponent (graphicEqView);
    pages[MatchEngine::graphicEqStage] = { &graphicEqView };

    // Limiter
    addChildComponent (limiterView);
    {
        const std::pair<const char*, const char*> knobs[] { { ParamIDs::limiterInput, "GAIN" }, { ParamIDs::limiterCeiling, "OUTPUT" },
                                                            { ParamIDs::limiterLookahead, "LOOKAHEAD" }, { ParamIDs::limiterAttack, "ATTACK" },
                                                            { ParamIDs::limiterRelease, "RELEASE" }, { ParamIDs::limiterLink, "LINK\nTRANSIENTS" },
                                                            { ParamIDs::limiterLinkRelease, "LINK\nRELEASE" } };
        for (size_t i = 0; i < limiterKnobs.size(); ++i)
            setUpKnobOn (MatchEngine::limiterStage, limiterKnobs[i], knobs[i].first, knobs[i].second);

        const std::pair<const char*, const char*> buttons[] { { ParamIDs::limiterOn, "LIMITER ON" }, { ParamIDs::limiterTruePeak, "TRUE PEAK" },
                                                              { ParamIDs::limiterDc, "DC FILTER" }, { ParamIDs::limiterUnity, "UNITY GAIN" },
                                                              { ParamIDs::limiterAutoRelease, "AUTO" } };
        for (size_t i = 0; i < limiterButtons.size(); ++i)
        {
            auto& b = limiterButtons[i];
            b.setButtonText (buttons[i].second);
            b.setClickingTogglesState (true);
            b.getProperties().set ("led", true);
            limiterButtonAttachments[i] = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (processorRef.apvts, buttons[i].first, b);
            addChildComponent (b);
            pages[MatchEngine::limiterStage].push_back (&b);
        }
        limiterButtons[4].setTooltip ("Automatic release: fast after transients, slow while it keeps limiting");
        auto& truePeak = limiterButtons[1];   // TRUE PEAK when on (LED lit), RMS when off
        truePeak.onStateChange = [&truePeak] { truePeak.setButtonText (truePeak.getToggleState() ? "TRUE PEAK" : "RMS"); };
        truePeak.onStateChange();
        auto& autoRelease = limiterButtons[4];   // AUTO: the release knob is not used
        autoRelease.onStateChange = [this, &autoRelease] { limiterKnobs[4].knob.setAlpha (autoRelease.getToggleState() ? 0.35f : 1.0f); };
        autoRelease.onStateChange();

        const std::pair<const char*, const char*> menus[] { { ParamIDs::limiterOversampling, "OVERSAMPLING" },
                                                            { ParamIDs::limiterDither, "DITHER" }, { ParamIDs::limiterShaping, "NOISE SHAPING" } };
        for (size_t i = 0; i < limiterMenus.size(); ++i)
        {
            auto& m = limiterMenus[i];
            if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (processorRef.apvts.getParameter (menus[i].first)))
                for (int c = 0; c < choice->choices.size(); ++c)
                    m.addItem (choice->choices[c].toUpperCase(), c + 1);
            limiterMenuAttachments[i] = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (processorRef.apvts, menus[i].first, m);
            auto& l = limiterMenuLabels[i];
            l.setText (menus[i].second, juce::dontSendNotification);
            l.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
            l.setJustificationType (juce::Justification::bottomLeft);
            addChildComponent (m);
            addChildComponent (l);
            pages[MatchEngine::limiterStage].insert (pages[MatchEngine::limiterStage].end(), { &m, &l });
        }
        pages[MatchEngine::limiterStage].push_back (&limiterView);
    }

    matchInfo.setFont (juce::Font (juce::FontOptions (11.0f)));
    matchInfo.setJustificationType (juce::Justification::centred);
    matchInfo.setColour (juce::Label::textColourId, Colours::text.withAlpha (0.7f));
    addChildComponent (matchInfo);
    pages[MatchEngine::eqStage] = { &stereoView, &midSideView, &phaseView, &liveView, &matchKnob, &responseKnob, &matchLabel, &responseLabel, &matchInfo };
    pages[MatchEngine::dynamicsStage] = { &dynamicsView, &dynamicsKnob, &dynamicsLabel, &dynamicsInfo };

    setResizable (true, true);
    setResizeLimits (900, 700, 2400, 1600);
    setSize (1100, 840);
    showPage ((int) processorRef.apvts.state.getProperty (pageProperty, 0));
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
    tabs.setState (currentPage, processorRef.getStageOrder(), bypassedStages());
    processorRef.apvts.state.setProperty (pageProperty, currentPage, nullptr);

    for (size_t p = 0; p < pages.size(); ++p)
        for (auto* c : pages[p])
            c->setVisible ((int) p == currentPage);
    repaint();
}

StageTabs::Bypassed UlMaToEditor::bypassedStages() const
{
    StageTabs::Bypassed b {};
    for (size_t stage = 0; stage < b.size(); ++stage)
        b[stage] = processorRef.apvts.getRawParameterValue (ParamIDs::stageBypass[stage])->load() > 0.5f;
    return b;
}

// Analysis state and new results
void UlMaToEditor::timerCallback()
{
    const bool busy = processorRef.isAnalysing();
    analyseButton.setEnabled (! busy && processorRef.getReferenceFile().existsAsFile());

    if (busy)
        statusLabel.setText ("Analysing ... " + juce::String (juce::roundToInt (processorRef.getAnalysisProgress() * 100.0f)) + " %",
                             juce::dontSendNotification);

    tabs.setState (currentPage, processorRef.getStageOrder(), bypassedStages());   // order / bypass may come from the host or a project

    // LEARN: button text and LED
    const bool learning = learnButton.getToggleState();
    const auto learned  = processorRef.getLearnedProfile();
    learnButton.setButtonText (learning ? "STOP LEARNING" : "LEARN INPUT");
    learnLed.setState (learning ? Led::State::blinking : learned.valid ? Led::State::on : Led::State::off);
    learnLabel.setText (learning ? "LEARNING INPUT ..." : learned.valid ? "INPUT ANALYSED" : "INPUT NOT ANALYSED",
                        juce::dontSendNotification);

    const int refVersion = processorRef.getReferenceVersion(), learnedVersion = processorRef.getLearnedVersion();
    if (refVersion != shownReferenceVersion || learnedVersion != shownLearnedVersion)
    {
        const auto profile = processorRef.getReferenceProfile();
        for (auto* view : { &stereoView, &midSideView, &phaseView })
            view->setProfiles (profile, learned);
        liveView.setReference (profile);
        dynamicsView.setReference (processorRef.getReferenceDynamics());

        matchInfo.setText (processorRef.getLearnedProfile().valid
                               ? "phase: full from 25 %\nstatic EQ: full from 50 %\ndynamic: 0 - 100 %"
                               : "dynamic: 0 - 100 %\n(LEARN INPUT for the static part)",
                           juce::dontSendNotification);

        dynamicsInfo.setText (! processorRef.getReferenceDynamics().valid
                                  ? "No dynamics of the reference yet:\npress ANALYZE"
                                  : processorRef.getLearnedDynamics().valid
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

    // Raised panels screwed onto the front around the controls of the page
    for (const auto& [page, r] : controlPanels)
        if (page == currentPage)
            Plot::drawScrewedPanel (g, r.toFloat());

    // Analog heat: titles of the three groups (right of the upper left screw)
    if (currentPage == MatchEngine::analogStage)
        for (auto [r, title] : { std::pair { tapePanel, "TAPE SATURATION PER BAND  (stereo: random drive per sample, left and right)" },
                                 std::pair { transportPanel, "TAPE TRANSPORT (WOW: PITCH, FLUTTER: LEVEL) / VINYL" },
                                 std::pair { outputPanel, "OUTPUT STAGES" } })
        {
            g.setColour (Colours::text);
            g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
            g.drawText (title, r.reduced (30, 9).removeFromTop (16), juce::Justification::centredLeft);
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

    controlPanels.clear();
    const auto panel = [this] (int page, juce::Rectangle<int> r, int padding)
    {
        controlPanels.push_back ({ page, r });
        return r.reduced (padding);
    };

    // Analog heat page: tape knobs in a row, below transport / vinyl and the output stages
    {
        auto page = area;
        tapePanel = page.removeFromTop (page.getHeight() / 2 - 6);
        page.removeFromTop (12);
        transportPanel = page.removeFromLeft (page.getWidth() * 5 / 7 - 6);
        page.removeFromLeft (12);
        outputPanel = page;
        for (auto r : { tapePanel, transportPanel, outputPanel })
            panel (MatchEngine::analogStage, r, 0);

        const auto placeRow = [] (juce::Rectangle<int> r, auto& knobs, size_t first, size_t count)
        {
            r = r.reduced (24, 12).withTrimmedTop (20);
            const int w = r.getWidth() / (int) count;
            for (size_t i = first; i < first + count; ++i)
            {
                auto cell = r.removeFromLeft (w);
                knobs[i].label.setBounds (cell.removeFromTop (32));
                knobs[i].knob.setBounds (cell.withSizeKeepingCentre (juce::jmin (cell.getWidth(), 150), juce::jmin (cell.getHeight(), 150)));
            }
        };
        placeRow (tapePanel, tapeKnobs, 0, tapeKnobs.size());
        placeRow (transportPanel, heatKnobs, 0, 5);
        placeRow (outputPanel, heatKnobs, 5, 2);
    }

    // PhaseQ and graphic EQ pages: the graph
    phaseQView.setBounds (area);
    graphicEqView.setBounds (area);

    // Limiter page: history above, controls below
    {
        auto page = area;
        auto limiterArea = panel (MatchEngine::limiterStage, page.removeFromBottom (juce::jmin (220, page.getHeight() / 2)), 18);
        page.removeFromBottom (14);
        limiterView.setBounds (page);

        // Right: buttons, then the menus; left: the knobs (AUTO above the release knob)
        auto buttons = limiterArea.removeFromRight (130);
        const int bh = juce::jmin (34, (buttons.getHeight() - 3 * 8) / 4);
        for (size_t i = 0; i < 4; ++i)
        {
            limiterButtons[i].setBounds (buttons.removeFromTop (bh));
            buttons.removeFromTop (8);
        }
        limiterArea.removeFromRight (14);
        auto menus = limiterArea.removeFromRight (150);
        const int mh = menus.getHeight() / 3;
        for (size_t i = 0; i < limiterMenus.size(); ++i)
        {
            auto cell = menus.removeFromTop (mh).withSizeKeepingCentre (menus.getWidth(), 48);
            limiterMenuLabels[i].setBounds (cell.removeFromTop (16));
            limiterMenus[i].setBounds (cell.removeFromTop (28));
        }
        limiterArea.removeFromRight (12);

        const int w = limiterArea.getWidth() / (int) limiterKnobs.size();
        for (size_t i = 0; i < limiterKnobs.size(); ++i)
        {
            auto& k = limiterKnobs[i];
            auto cell = limiterArea.removeFromLeft (w);
            if (i == 4)
                limiterButtons[4].setBounds (cell.removeFromTop (26).withSizeKeepingCentre (70, 24));
            else
                cell.removeFromTop (26);
            k.label.setBounds (cell.removeFromTop (26));
            k.knob.setBounds (cell.withSizeKeepingCentre (juce::jmin (cell.getWidth(), 112), juce::jmin (cell.getHeight(), 126)));
        }
    }

    // Dynamics page: the bands, the DYNAMICS knob on the right
    {
        auto page = area;
        auto knob = panel (MatchEngine::dynamicsStage, page.removeFromRight (220), 22);
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
    auto knobs = panel (MatchEngine::eqStage, area.removeFromRight (350), 20);
    area.removeFromRight (16);
    liveView.setBounds (area);

    auto match = knobs.removeFromLeft (200);
    matchLabel.setBounds (match.removeFromTop (24));
    matchInfo.setBounds (match.removeFromBottom (44));
    matchKnob.setBounds (match.withSizeKeepingCentre (match.getWidth(), juce::jmin (match.getHeight(), 240)));

    auto response = knobs.withTrimmedTop (knobs.getHeight() / 2 - 40);
    responseLabel.setBounds (response.removeFromTop (20));
    responseKnob.setBounds (response.removeFromTop (juce::jmin (130, response.getHeight())));
}
