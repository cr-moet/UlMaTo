#pragma once

#include "Views.h"

//==============================================================================
// Editors for PhaseQ and the graphic EQ: six nodes over log frequency. Drag a node:
// frequency (x) and value (y); mouse wheel: Q; double-click: back to 0. The selected node
// is reported (onSelect), so controls next to the graph can edit it.
//==============================================================================
class NodeEditor : public juce::Component,
                   private juce::Timer
{
public:
    NodeEditor (UlMaToProcessor& p, juce::String titleText, float bottomValue, float topValue, float gridStep)
        : processor (p), title (std::move (titleText)), bottom (bottomValue), top (topValue), step (gridStep)
    {
        startTimerHz (20);
    }

    void resized() override   { plot = getLocalBounds().toFloat().withTrimmedTop (18.0f).withTrimmedLeft (40.0f).withTrimmedBottom (14.0f + stripHeight).withTrimmedRight (4.0f); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours::text);
        g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        g.drawText (title, r.removeFromTop (18.0f), juce::Justification::centredLeft);

        Plot::drawDisplay (g, plot);
        drawFrequencyGrid (g);
        for (float v = top; v >= bottom - 0.001f; v -= step)
        {
            const float y = yOf (v);
            g.setColour (std::abs (v) < 0.001f ? Colours::text.withAlpha (0.35f) : Colours::grid);
            g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
            g.setColour (Colours::text.withAlpha (0.6f));
            g.setFont (juce::Font (juce::FontOptions (10.0f)));
            const float labelY = juce::jlimit (plot.getY(), plot.getBottom() - 12.0f, y - 6.0f);   // inside the plot height
            g.drawText ((v > 0.0f ? "+" : "") + juce::String (v, 0), juce::Rectangle<float> (plot.getX() - 42.0f, labelY, 38.0f, 12.0f),
                        juce::Justification::centredRight);
        }

        drawBackground (g);

        // The individual nodes faintly, the sum strongly
        for (int n = 0; n < numNodes(); ++n)
            if (isActive (n))
                drawCurve (g, [this, n] (float f) { return nodeCurve (n, f); }, Colours::text.withAlpha (0.2f), 1.0f);
        drawTotals (g);

        for (int n = 0; n < numNodes(); ++n)   // the selected one last, on top
            if (n != selected && isActive (n))
                drawNode (g, n, nodePoint (n), n == hovered);
        if (selected >= 0 && isActive (selected))
            drawNode (g, selected, nodePoint (selected), true);

        // Values of the selected (or hovered) node
        const int shown = hovered >= 0 ? hovered : selected;
        if (shown >= 0 && isActive (shown))
        {
            g.setColour (Colours::text);
            g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
            g.drawText (describe (shown), plot.reduced (8.0f, 4.0f).removeFromTop (16.0f), juce::Justification::centredRight);
        }
        g.setColour (Colours::text.withAlpha (0.55f));
        g.setFont (juce::Font (juce::FontOptions (11.0f)));
        g.drawText (hint, plot.reduced (8.0f, 4.0f).removeFromBottom (14.0f), juce::Justification::centredLeft);
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int n = nodeAt (e.position);
        if (n != hovered)
        {
            hovered = n;
            repaint();
        }
    }

    void mouseExit (const juce::MouseEvent&) override   { hovered = -1; repaint(); }

    std::function<void (int)> onSelect;
    int getSelected() const noexcept   { return selected; }

    void mouseDown (const juce::MouseEvent& e) override
    {
        // The selected node stays selected when it is under the mouse (so it can be dragged);
        // a click without dragging on stacked nodes then moves on to the next of them
        const auto under = nodesAt (e.position);
        const bool keep = std::find (under.begin(), under.end(), selected) != under.end();
        const int n = keep ? selected : nodeAt (e.position);
        cycleOnClick = keep && under.size() > 1;
        select (n);
        dragNode = n;
        if (dragNode >= 0)
            for (auto* id : { freqId (dragNode), valueId (dragNode) })
                parameter (id)->beginChangeGesture();
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragNode < 0 || ! isActive (dragNode))
            return;
        const float x = juce::jlimit (0.0f, 1.0f, (e.position.x - plot.getX()) / plot.getWidth());
        set (freqId (dragNode), 20.0f * std::pow (1000.0f, x));
        set (valueId (dragNode), juce::jlimit (bottom, top, top - (e.position.y - plot.getY()) / plot.getHeight() * (top - bottom)));
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (dragNode >= 0)
            for (auto* id : { freqId (dragNode), valueId (dragNode) })
                parameter (id)->endChangeGesture();
        dragNode = -1;

        if (cycleOnClick && ! e.mouseWasDraggedSinceMouseDown())
        {
            const auto under = nodesAt (e.position);
            const auto it = std::find (under.begin(), under.end(), selected);
            if (it != under.end())
                select (under[(size_t) (std::distance (under.begin(), it) + 1) % under.size()]);
            repaint();
        }
        cycleOnClick = false;
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int n = nodeAt (e.position);
        if (n >= 0)
            setWithGesture (valueId (n), 0.0f);
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        const int n = nodeAt (e.position) >= 0 ? nodeAt (e.position) : selected;
        if (n < 0)
            return;
        auto* p = parameter (widthId (n));
        const float factor = std::pow (1.15f, wheel.deltaY * (wheel.isReversed ? -4.0f : 4.0f));
        setWithGesture (widthId (n), p->convertFrom0to1 (p->getValue()) * factor);
    }

protected:
    virtual int numNodes() const   { return Tone::numBands; }
    virtual bool isActive (int) const   { return true; }
    virtual void drawTotals (juce::Graphics& g)   { drawCurve (g, [this] (float f) { return totalCurve (f); }, curveColour, 2.4f); }

    virtual void drawNode (juce::Graphics& g, int n, juce::Point<float> c, bool on)
    {
        const auto r = juce::Rectangle<float> (16.0f, 16.0f).withCentre (c);
        g.setColour (on ? curveColour : Colours::panel);
        g.fillEllipse (r);
        g.setColour (on ? Colours::background : Colours::text);
        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
        g.drawText (juce::String (n + 1), r, juce::Justification::centred);
        g.setColour (Colours::text);
        g.drawEllipse (r, 1.2f);
    }

    virtual const char* freqId (int n) const = 0;
    virtual const char* valueId (int n) const = 0;
    virtual const char* widthId (int n) const = 0;
    virtual float nodeCurve (int n, float f) const = 0;    // curve of one node (display units)
    virtual float totalCurve (float f) const = 0;
    virtual juce::String describe (int n) const = 0;
    virtual void drawBackground (juce::Graphics&) {}

    float value (const char* id) const   { return processor.apvts.getRawParameterValue (id)->load(); }

    template <typename Fn>
    void drawCurve (juce::Graphics& g, Fn curve, juce::Colour colour, float thickness, bool dashed = false) const
    {
        juce::Path path;
        bool first = true;
        for (float x = plot.getX(); x <= plot.getRight(); x += 2.0f)
        {
            const float f = 20.0f * std::pow (1000.0f, (x - plot.getX()) / plot.getWidth());
            const juce::Point<float> pt { x, yOf (curve (f)) };
            if (first) path.startNewSubPath (pt); else path.lineTo (pt);
            first = false;
        }
        g.setColour (colour);
        if (dashed)
        {
            const float dashes[] { 7.0f, 5.0f };
            juce::Path d;
            juce::PathStrokeType (thickness).createDashedStroke (d, path, dashes, 2);
            g.fillPath (d);
        }
        else
            g.strokePath (path, juce::PathStrokeType (thickness, juce::PathStrokeType::curved));
    }



    float xOf (float f) const   { return plot.getX() + plot.getWidth() * std::log (f / 20.0f) / std::log (1000.0f); }
    float yOf (float v) const   { return plot.getY() + plot.getHeight() * juce::jlimit (0.0f, 1.0f, (top - v) / (top - bottom)); }

    UlMaToProcessor& processor;
    juce::String title, hint { "drag a node  -  wheel: Q  -  double-click: reset" };
    juce::Colour curveColour = Colours::accent;
    juce::Rectangle<float> plot;
    float bottom, top, step;
    float stripHeight = 0.0f;   // room below the frequency labels (for controls)

    juce::RangedAudioParameter* parameter (const char* id) const   { return processor.apvts.getParameter (id); }

    void set (const char* id, float v)
    {
        auto* p = parameter (id);
        p->setValueNotifyingHost (p->convertTo0to1 (v));
    }

    void setWithGesture (const char* id, float v)
    {
        parameter (id)->beginChangeGesture();
        set (id, v);
        parameter (id)->endChangeGesture();
    }

    juce::Point<float> nodePoint (int n) const   { return { xOf (value (freqId (n))), yOf (value (valueId (n))) }; }

    std::vector<int> nodesAt (juce::Point<float> p) const
    {
        std::vector<int> under;
        for (int n = 0; n < numNodes(); ++n)
            if (isActive (n) && nodePoint (n).getDistanceFrom (p) < 14.0f)
                under.push_back (n);
        return under;
    }

    void select (int n, bool force = false)
    {
        if (n >= 0 && (n != selected || force))
        {
            selected = n;
            if (onSelect != nullptr)
                onSelect (n);
        }
    }

    int nodeAt (juce::Point<float> p) const
    {
        int best = -1;
        float distance = 14.0f;
        for (int n = 0; n < numNodes(); ++n)
            if (const float d = nodePoint (n).getDistanceFrom (p); isActive (n) && d < distance)
            {
                distance = d;
                best = n;
            }
        return best;
    }

    void drawFrequencyGrid (juce::Graphics& g) const
    {
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        for (float hz : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
        {
            const float x = xOf (hz);
            g.setColour (Colours::grid);
            g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
            g.setColour (Colours::text.withAlpha (0.6f));
            g.drawText (hz < 1000.0f ? juce::String ((int) hz) : juce::String ((int) hz / 1000) + "k",
                        juce::Rectangle<float> (x - 20.0f, plot.getBottom() + 1.0f, 40.0f, 12.0f), juce::Justification::centred);
        }
    }

    void timerCallback() override
    {
        if (isShowing())
            repaint();
    }

    int selected = 0, hovered = -1, dragNode = -1;
    bool cycleOnClick = false;
};

//==============================================================================
class GraphicEqView final : public NodeEditor
{
public:
    explicit GraphicEqView (UlMaToProcessor& p)
        : NodeEditor (p, "GRAPHIC EQ  [dB]  (linear phase: only the level changes)", -18.0f, 18.0f, 6.0f)
    {
        hint = "drag a node  -  wheel: Q  -  double-click: 0 dB   (1: low shelf, 6: high shelf)";
    }

protected:
    const char* freqId (int n) const override   { return ParamIDs::eqFreq[n]; }
    const char* valueId (int n) const override  { return ParamIDs::eqGain[n]; }
    const char* widthId (int n) const override  { return ParamIDs::eqQ[n]; }

    Tone::EqSettings settings() const
    {
        Tone::EqSettings s;
        for (size_t b = 0; b < (size_t) Tone::numBands; ++b)
            s = withBand (s, b);
        return s;
    }

    Tone::EqSettings withBand (Tone::EqSettings s, size_t b) const
    {
        s.freq[b] = value (ParamIDs::eqFreq[b]);
        s.gain[b] = value (ParamIDs::eqGain[b]);
        s.q[b]    = value (ParamIDs::eqQ[b]);
        return s;
    }

    float nodeCurve (int n, float f) const override
    {
        return juce::Decibels::gainToDecibels (Tone::bandMagnitude (Tone::eqTypes[n], value (ParamIDs::eqFreq[n]),
                                                                    value (ParamIDs::eqGain[n]), value (ParamIDs::eqQ[n]), f));
    }

    float totalCurve (float f) const override
    {
        return juce::Decibels::gainToDecibels (Tone::eqMagnitude (settings(), f));
    }

    juce::String describe (int n) const override
    {
        const float f = value (ParamIDs::eqFreq[n]), gain = value (ParamIDs::eqGain[n]);
        static constexpr const char* types[] { "LOW SHELF", "BELL", "HIGH SHELF" };
        return juce::String (n + 1) + "  " + types[(int) Tone::eqTypes[n]] + "   "
             + (f < 1000.0f ? juce::String (f, 0) + " Hz" : juce::String (f / 1000.0f, 2) + " kHz") + "   "
             + (gain > 0.0f ? "+" : "") + juce::String (gain, 1) + " dB   Q " + juce::String (value (ParamIDs::eqQ[n]), 2);
    }

    // The processed signal behind the curve (mid, scaled into the plot)
    void drawBackground (juce::Graphics& g) override
    {
        const auto out = processor.getOutputProfile();
        if (! out.valid)
            return;
        juce::Path area;
        area.startNewSubPath (plot.getX(), plot.getBottom());
        for (int b = 0; b < Spectrum::numBands; ++b)
        {
            const float level = juce::jlimit (0.0f, 1.0f, (Spectrum::toDb (out.mid[(size_t) b]) + 96.0f) / 96.0f);
            area.lineTo (xOf (Spectrum::bandFrequency ((float) b)), plot.getBottom() - level * plot.getHeight());
        }
        area.lineTo (plot.getRight(), plot.getBottom());
        area.closeSubPath();
        g.setColour (Colours::text.withAlpha (0.07f));
        g.fillPath (area);
    }
};

//==============================================================================
// PhaseQ node names: the channel and the number among the nodes of that channel
// (L1, L2, R1 ...), so a node switched to the right becomes the next free R number
inline juce::String phaseNodeName (const juce::AudioProcessorValueTreeState& state, int node)
{
    const auto value = [&state] (const char* id) { return state.getRawParameterValue (id)->load(); };
    const bool right = value (ParamIDs::phaseSide[node]) > 0.5f;
    int number = 0;
    for (int n = 0; n <= node; ++n)
        if ((value (ParamIDs::phaseOn[n]) > 0.5f || n == node) && (value (ParamIDs::phaseSide[n]) > 0.5f) == right)
            ++number;
    return (right ? "R" : "L") + juce::String (number);
}

//==============================================================================
// PhaseQ node strip: a thin row below the display with the controls of the selected
// node - name, LEFT / RIGHT, shape (bell, low shelf, high shelf), BYPASS, FREQ, PHASE,
// Q (small knobs) and X (remove) on the right
class PhaseNodeStrip final : public juce::Component,
                             private juce::Timer
{
public:
    explicit PhaseNodeStrip (juce::AudioProcessorValueTreeState& s) : state (s)
    {
        for (size_t i = 0; i < knobs.size(); ++i)
        {
            knobs[i].setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            knobs[i].setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 20);
            knobs[i].setColour (juce::Slider::textBoxBackgroundColourId, Colours::text);
            knobs[i].setColour (juce::Slider::textBoxTextColourId, Colours::accent);
            labels[i].setText (std::array { "FREQ", "PHASE", "Q" }[i], juce::dontSendNotification);
            labels[i].setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
            labels[i].setJustificationType (juce::Justification::centredLeft);
        }
        shapeBox.addItemList ({ "BELL", "LOW SHELF", "HIGH SHELF" }, 1);
        for (auto [button, side] : { std::pair { &leftButton, 0 }, std::pair { &rightButton, 1 } })
        {
            button->getProperties().set ("led", true);
            button->onClick = [this, choice = side] { setSide (choice); };
        }
        bypassButton.setClickingTogglesState (true);
        bypassButton.getProperties().set ("led", true);
        bypassButton.getProperties().set ("ledInverted", true);   // lit while the node is active
        removeButton.setTooltip ("Remove this node");
        removeButton.onClick = [this] { if (onRemove != nullptr) onRemove(); };
        for (auto* c : controls())
            addChildComponent (c);
        startTimerHz (15);
    }

    std::function<void()> onRemove;
    int getNode() const noexcept   { return node; }

    // Edit node n (-1: none selected - only a hint is shown)
    void bind (int n)
    {
        node = n;
        for (auto& a : attachments)
            a.reset();
        shapeAttachment.reset();
        bypassAttachment.reset();
        if (node >= 0)
        {
            const char* ids[] { ParamIDs::phaseFreq[node], ParamIDs::phaseAmount[node], ParamIDs::phaseQ[node] };
            for (size_t i = 0; i < knobs.size(); ++i)
                attachments[i] = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (state, ids[i], knobs[i]);
            shapeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, ParamIDs::phaseShape[node], shapeBox);
            bypassAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (state, ParamIDs::phaseBypass[node], bypassButton);
        }
        for (auto* c : controls())
            c->setVisible (node >= 0);
        refresh();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (Colours::text.withAlpha (0.06f));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (Colours::text.withAlpha (0.45f));
        g.drawRoundedRectangle (r, 4.0f, 1.0f);
        g.setColour (Colours::text);
        if (node >= 0)
        {
            g.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
            g.drawText (name, nameArea, juce::Justification::centred);
        }
        else
        {
            g.setColour (Colours::text.withAlpha (0.6f));
            g.setFont (juce::Font (juce::FontOptions (12.0f)));
            g.drawText ("no node selected  -  double-click into the display to create one", getLocalBounds().reduced (12, 0), juce::Justification::centredLeft);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8, 6);
        const auto gap = [&r] (int w) { r.removeFromLeft (w); };
        removeButton.setBounds (r.removeFromRight (32).withSizeKeepingCentre (30, 30));
        nameArea = r.removeFromLeft (44);
        gap (6);
        const auto centred = [] (juce::Rectangle<int> c) { return c.withSizeKeepingCentre (c.getWidth(), 30); };
        leftButton.setBounds (centred (r.removeFromLeft (62)));
        gap (4);
        rightButton.setBounds (centred (r.removeFromLeft (62)));
        gap (10);
        shapeBox.setBounds (centred (r.removeFromLeft (130)));
        gap (10);
        bypassButton.setBounds (centred (r.removeFromLeft (86)));
        gap (14);

        const int w = juce::jmin (180, r.getWidth() / (int) knobs.size());
        for (size_t i = 0; i < knobs.size(); ++i)
        {
            auto cell = r.removeFromLeft (w);
            const int knobSize = juce::jmin (cell.getHeight(), 50);
            knobs[i].setBounds (cell.withTrimmedRight (cell.getWidth() - knobSize - 78));
            labels[i].setBounds (cell.getX() + knobSize + 4, cell.getY() - 2, 76, 14);
        }
    }

private:
    std::vector<juce::Component*> controls()
    {
        return { &knobs[0], &knobs[1], &knobs[2], &labels[0], &labels[1], &labels[2],
                 &leftButton, &rightButton, &shapeBox, &bypassButton, &removeButton };
    }

    void setSide (int side)
    {
        auto* p = state.getParameter (ParamIDs::phaseSide[node]);
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 ((float) side));
        p->endChangeGesture();
        refresh();
    }

    void refresh()
    {
        if (node < 0)
            return;
        const bool right = state.getRawParameterValue (ParamIDs::phaseSide[node])->load() > 0.5f;
        leftButton.setToggleState (! right, juce::dontSendNotification);
        rightButton.setToggleState (right, juce::dontSendNotification);
        if (const auto newName = phaseNodeName (state, node); newName != name)
        {
            name = newName;
            repaint();
        }
    }

    void timerCallback() override
    {
        if (isShowing())
            refresh();
    }

    juce::AudioProcessorValueTreeState& state;
    std::array<juce::Slider, 3> knobs;   // freq, phase, Q
    std::array<juce::Label, 3> labels;
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>, 3> attachments;
    juce::ComboBox shapeBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> shapeAttachment;
    juce::TextButton leftButton { "LEFT" }, rightButton { "RIGHT" }, bypassButton { "BYPASS" }, removeButton { "X" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAttachment;
    juce::Rectangle<int> nameArea;
    juce::String name;
    int node = -1;
};

//==============================================================================
// PhaseQ: both channels in one graph - left solid, right dashed (both orange). It starts
// without nodes; a double-click on the graph creates one (on a node: removes it). The strip
// below the display edits the selected node.
class PhaseQView final : public NodeEditor
{
public:
    explicit PhaseQView (UlMaToProcessor& p)
        : NodeEditor (p, juce::String (juce::CharPointer_UTF8 ("PHASEQ  [\xc2\xb0]  -  LEFT solid, RIGHT dashed  (only the phase changes, the level stays flat)")),
                      -180.0f, 180.0f, 45.0f),
          strip (p.apvts)
    {
        curveColour = Colours::correlation;
        hint = "double-click: new node / remove node  -  drag a node  -  wheel: Q";
        stripHeight = 74.0f;
        addAndMakeVisible (strip);
        strip.onRemove = [this] { remove (selected); };
        onSelect = [this] (int) { updateStrip(); };
        selected = -1;
        for (int n = numNodes(); --n >= 0;)
            if (isActive (n))
                selected = n;
        updateStrip();
    }

    void resized() override
    {
        NodeEditor::resized();
        strip.setBounds (getLocalBounds().removeFromBottom (62).withTrimmedLeft ((int) plot.getX()).withTrimmedRight (4));
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (const int n = nodeAt (e.position); n >= 0)
            remove (n);
        else if (plot.contains (e.position))
            create (e.position);
    }

protected:
    int numNodes() const override              { return Tone::numPhaseNodes; }
    bool isActive (int n) const override       { return value (ParamIDs::phaseOn[n]) > 0.5f; }
    const char* freqId (int n) const override   { return ParamIDs::phaseFreq[n]; }
    const char* valueId (int n) const override  { return ParamIDs::phaseAmount[n]; }
    const char* widthId (int n) const override  { return ParamIDs::phaseQ[n]; }

    int channelOf (int n) const       { return value (ParamIDs::phaseSide[n]) > 0.5f ? 1 : 0; }
    bool isBypassed (int n) const     { return value (ParamIDs::phaseBypass[n]) > 0.5f; }
    Tone::PhaseShape shapeOf (int n) const   { return (Tone::PhaseShape) juce::roundToInt (value (ParamIDs::phaseShape[n])); }

    // withBypassed: bypassed nodes count too (for their own faint curve)
    Tone::PhaseSettings settings (bool withBypassed = false) const
    {
        Tone::PhaseSettings s;
        for (int n = 0; n < Tone::numPhaseNodes; ++n)
            s.node[(size_t) n] = { isActive (n) && (withBypassed || ! isBypassed (n)), channelOf (n), shapeOf (n),
                                   value (ParamIDs::phaseFreq[n]), value (ParamIDs::phaseAmount[n]), value (ParamIDs::phaseQ[n]) };
        return s;
    }

    float nodeCurve (int n, float f) const override   { return juce::radiansToDegrees (Tone::phaseAt (settings (true), channelOf (n), f, n)); }
    float totalCurve (float f) const override         { return juce::radiansToDegrees (Tone::phaseAt (settings(), 0, f)); }

    void drawTotals (juce::Graphics& g) override
    {
        const auto s = settings();
        drawCurve (g, [&s] (float f) { return juce::radiansToDegrees (Tone::phaseAt (s, 0, f)); }, curveColour, 2.4f);
        drawCurve (g, [&s] (float f) { return juce::radiansToDegrees (Tone::phaseAt (s, 1, f)); }, curveColour.darker (0.35f), 2.2f, true);
    }

    void drawNode (juce::Graphics& g, int n, juce::Point<float> c, bool on) override
    {
        const auto r = juce::Rectangle<float> (26.0f, 16.0f).withCentre (c);
        const bool right = channelOf (n) == 1, bypassed = isBypassed (n);
        const float corner = shapeOf (n) == Tone::PhaseShape::bell ? 8.0f : 2.0f;   // shelves: square corners
        const auto fill = bypassed ? Colours::display.brighter (0.25f) : curveColour;
        g.setColour (on ? fill : (right ? Colours::display : fill.withAlpha (0.45f)));
        g.fillRoundedRectangle (r, corner);
        g.setColour (on && ! bypassed ? Colours::background : Colours::text.withAlpha (bypassed ? 0.55f : 1.0f));
        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
        g.drawText (phaseNodeName (processor.apvts, n), r, juce::Justification::centred);
        g.setColour (Colours::text.withAlpha (bypassed ? 0.55f : 1.0f));
        juce::Path outline;
        outline.addRoundedRectangle (r, corner);
        if (right)
        {
            juce::Path dashed;
            const float dashes[] { 3.0f, 2.0f };
            juce::PathStrokeType (1.2f).createDashedStroke (dashed, outline, dashes, 2);
            g.fillPath (dashed);
        }
        else
            g.strokePath (outline, juce::PathStrokeType (1.2f));
    }

    juce::String describe (int n) const override
    {
        static constexpr const char* shapes[] { "BELL", "LOW SHELF", "HIGH SHELF" };
        const float f = value (ParamIDs::phaseFreq[n]), amount = value (ParamIDs::phaseAmount[n]);
        return phaseNodeName (processor.apvts, n) + "  " + shapes[(int) shapeOf (n)] + (isBypassed (n) ? " (BYPASSED)   " : "   ")
             + (f < 1000.0f ? juce::String (f, 0) + " Hz" : juce::String (f / 1000.0f, 2) + " kHz") + "   "
             + (amount > 0.0f ? "+" : "") + juce::String (amount, 0) + juce::String (juce::CharPointer_UTF8 ("\xc2\xb0"))
             + "   Q " + juce::String (value (ParamIDs::phaseQ[n]), 2);
    }

private:
    void timerCallback() override
    {
        NodeEditor::timerCallback();
        updateStrip();   // nodes can also change from the host (automation, undo, presets)
    }

    void updateStrip()
    {
        if (selected >= 0 && ! isActive (selected))
            selected = -1;
        if (strip.getNode() != selected)
            strip.bind (selected);
    }

    // A new node where the mouse is: the first free one, left channel, bell, Q 1, active
    void create (juce::Point<float> p)
    {
        for (int n = 0; n < numNodes(); ++n)
            if (! isActive (n))
            {
                const float x = juce::jlimit (0.0f, 1.0f, (p.x - plot.getX()) / plot.getWidth());
                setWithGesture (ParamIDs::phaseFreq[n], 20.0f * std::pow (1000.0f, x));
                setWithGesture (ParamIDs::phaseAmount[n], juce::jlimit (bottom, top, top - (p.y - plot.getY()) / plot.getHeight() * (top - bottom)));
                setWithGesture (ParamIDs::phaseQ[n], 1.0f);
                setWithGesture (ParamIDs::phaseSide[n], 0.0f);
                setWithGesture (ParamIDs::phaseShape[n], 0.0f);
                setWithGesture (ParamIDs::phaseBypass[n], 0.0f);
                setWithGesture (ParamIDs::phaseOn[n], 1.0f);
                select (n, true);
                return;
            }
    }

    // Remove a node; the selection moves on to another node (or none is left)
    void remove (int n)
    {
        if (n < 0)
            return;
        setWithGesture (ParamIDs::phaseOn[n], 0.0f);
        setWithGesture (ParamIDs::phaseAmount[n], 0.0f);
        if (n == selected)
        {
            selected = -1;
            for (int i = 0; i < numNodes(); ++i)
                if (isActive (i))
                {
                    select (i, true);
                    break;
                }
        }
        updateStrip();
        repaint();
    }

    PhaseNodeStrip strip;
};

//==============================================================================
// Limiter: gain reduction and output peak over the last 4 seconds, current values
//==============================================================================
class LimiterView final : public juce::Component,
                          private juce::Timer
{
public:
    explicit LimiterView (UlMaToProcessor& p) : processor (p)
    {
        reductionHistory.fill (0.0f);
        outputHistory.fill (-100.0f);
        startTimerHz (rate);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours::text);
        g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        g.drawText ("LIMITER  -  output peak and gain reduction, last 4 s", r.removeFromTop (18.0f), juce::Justification::centredLeft);

        auto numbers = r.removeFromRight (190.0f);
        r.removeFromRight (12.0f);
        const auto plot = r.withTrimmedLeft (40.0f).withTrimmedBottom (4.0f);
        Plot::drawDisplay (g, plot);

        // Level scale 0 ... -24 dB (output peak from below, reduction from the top)
        constexpr float range = 24.0f;
        const auto yOf = [&] (float db) { return plot.getY() + plot.getHeight() * juce::jlimit (0.0f, 1.0f, -db / range); };
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        for (float db = 0.0f; db >= -range; db -= 3.0f)
        {
            g.setColour (db == 0.0f ? Colours::text.withAlpha (0.35f) : Colours::grid);
            g.drawHorizontalLine ((int) yOf (db), plot.getX(), plot.getRight());
            g.setColour (Colours::text.withAlpha (0.6f));
            g.drawText (juce::String (db, 0), juce::Rectangle<float> (plot.getX() - 42.0f, yOf (db) - 6.0f, 38.0f, 12.0f),
                        juce::Justification::centredRight);
        }

        const float ceiling = processor.apvts.getRawParameterValue (ParamIDs::limiterCeiling)->load();
        const int count = (int) reductionHistory.size();
        const float w = plot.getWidth() / (float) count;
        for (int i = 0; i < count; ++i)
        {
            const auto index = (size_t) ((writePosition + i) % count);
            const float x = plot.getX() + (float) i * w;
            g.setColour (Colours::text.withAlpha (0.25f));   // output peak
            g.fillRect (juce::Rectangle<float>::leftTopRightBottom (x, yOf (outputHistory[index]), x + w, plot.getBottom()));
            g.setColour (Colours::accent);                   // reduction
            g.fillRect (juce::Rectangle<float>::leftTopRightBottom (x, plot.getY(), x + w, yOf (reductionHistory[index])));
        }
        g.setColour (Colours::correlation);
        g.drawHorizontalLine ((int) yOf (ceiling), plot.getX(), plot.getRight());

        // Numbers
        const auto row = [&] (const juce::String& label, const juce::String& text, juce::Colour colour)
        {
            auto line = numbers.removeFromTop (44.0f);
            g.setColour (Colours::text.withAlpha (0.7f));
            g.setFont (juce::Font (juce::FontOptions (11.0f)));
            g.drawText (label, line.removeFromTop (16.0f), juce::Justification::centredLeft);
            auto box = line.removeFromTop (24.0f);
            g.setColour (Colours::text);
            g.fillRect (box);
            g.setColour (colour);
            g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 15.0f, juce::Font::bold)));
            g.drawText (text, box.reduced (8.0f, 0.0f), juce::Justification::centredRight);
        };
        numbers.removeFromTop (4.0f);
        const auto reductionText = [] (float db) { return (db < -0.05f ? juce::String (db, 1) : juce::String ("0.0")) + " dB"; };
        row ("GAIN REDUCTION", reductionText (current), Colours::accent);
        row ("MAX REDUCTION (click to reset)", reductionText (maximum), Colours::accent);
        row ("OUTPUT PEAK", juce::String (juce::jmax (-99.9f, peak), 1) + " dBFS", Colours::background);
        row ("CEILING", juce::String (ceiling, 2) + " dBFS", Colours::correlation);
    }

    void mouseDown (const juce::MouseEvent&) override   { maximum = 0.0f; peak = -100.0f; repaint(); }

private:
    static constexpr int rate = 50;

    void timerCallback() override
    {
        const float reduction = processor.getLimiterReductionDb(), output = processor.getLimiterOutputDb();
        reductionHistory[(size_t) writePosition] = reduction;
        outputHistory[(size_t) writePosition] = output;
        writePosition = (writePosition + 1) % (int) reductionHistory.size();
        current += (reduction < current ? 0.6f : 0.15f) * (reduction - current);
        maximum = juce::jmin (maximum, reduction);
        peak = juce::jmax (peak, output);
        if (isShowing())
            repaint();
    }

    UlMaToProcessor& processor;
    std::array<float, 4 * rate> reductionHistory, outputHistory;
    int writePosition = 0;
    float current = 0.0f, maximum = 0.0f, peak = -100.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LimiterView)
};
