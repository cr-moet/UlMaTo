#pragma once

#include "Views.h"

//==============================================================================
// Graph of nodes over log frequency (base of PhaseQ and the graphic EQ). Drag a node:
// frequency (x) and value (y); mouse wheel: Q. The selected node is reported (onSelect).
// The curves are calculated only when a value they depend on has changed (collectState)
// and kept as ready-to-fill paths; the graph repaints only then (or always, if 'animated').
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

        refreshCurves();
        for (const auto& curve : curves)
        {
            g.setColour (curve.colour);
            g.fillPath (curve.path);
        }

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
    virtual int numNodes() const = 0;
    virtual bool isActive (int n) const = 0;
    virtual const char* freqId (int n) const = 0;
    virtual const char* valueId (int n) const = 0;
    virtual const char* widthId (int n) const = 0;
    virtual juce::String describe (int n) const = 0;
    virtual void drawNode (juce::Graphics& g, int n, juce::Point<float> centre, bool on) = 0;
    virtual void drawBackground (juce::Graphics&) {}

    // The values the curves depend on, and the curves themselves (faint ones per node,
    // the total strongly) - added with addCurve
    virtual void collectState (std::vector<float>& values) const = 0;
    virtual void buildCurves() = 0;

    float value (const char* id) const   { return processor.apvts.getRawParameterValue (id)->load(); }

    // One curve (value over frequency) as a stroke, kept until the state changes
    template <typename Fn>
    void addCurve (Fn curve, juce::Colour colour, float thickness, bool dashed = false)
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
        Curve c { {}, colour };
        if (dashed)
        {
            const float dashes[] { 7.0f, 5.0f };
            juce::PathStrokeType (thickness).createDashedStroke (c.path, path, dashes, 2);
        }
        else
            juce::PathStrokeType (thickness, juce::PathStrokeType::curved).createStrokedPath (c.path, path);
        curves.push_back (std::move (c));
    }

    bool animated = false;   // repaint with every tick (e.g. for a spectrum behind the curves)



    float xOf (float f) const   { return plot.getX() + plot.getWidth() * std::log (f / 20.0f) / std::log (1000.0f); }
    float yOf (float v) const   { return plot.getY() + plot.getHeight() * juce::jlimit (0.0f, 1.0f, (top - v) / (top - bottom)); }

    UlMaToProcessor& processor;
    juce::String title, hint;
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
        if (isShowing() && (refreshCurves() || animated))
            repaint();
    }

    // Rebuilds the curves when their state (or the size) has changed; true if it did
    bool refreshCurves()
    {
        scratch.clear();
        collectState (scratch);
        scratch.push_back (plot.getWidth());
        scratch.push_back (plot.getHeight());
        if (scratch == state && ! curves.empty())
            return false;
        state.swap (scratch);
        curves.clear();
        buildCurves();
        return true;
    }

    struct Curve { juce::Path path; juce::Colour colour; };
    std::vector<Curve> curves;
    std::vector<float> state, scratch;
    int selected = 0, hovered = -1, dragNode = -1;
    bool cycleOnClick = false;
};

//==============================================================================
// Parameters of a list of nodes that are created by double-click (PhaseQ, graphic EQ)
struct NodeSet
{
    int count;
    const char* const* on;
    const char* const* bypass;
    const char* const* shape;      // choice: bell, low shelf, high shelf
    const char* const* side;       // choice left / right, nullptr: no channel choice
    const char* const* freq;
    const char* const* value;      // phase in degrees / gain in dB
    const char* const* q;
    const char* valueName;         // "PHASE" / "GAIN"
};

// Names: PhaseQ the channel and the number among the nodes of that channel (L1, L2, R1 ...,
// a node switched to the right becomes the next free R number); EQ the number among its bands
inline juce::String nodeName (const juce::AudioProcessorValueTreeState& state, const NodeSet& set, int node)
{
    const auto value = [&state] (const char* id) { return state.getRawParameterValue (id)->load(); };
    const auto sideOf = [&] (int n) { return set.side != nullptr && value (set.side[n]) > 0.5f; };
    int number = 0;
    for (int n = 0; n <= node; ++n)
        if ((value (set.on[n]) > 0.5f || n == node) && sideOf (n) == sideOf (node))
            ++number;
    return (set.side == nullptr ? juce::String() : juce::String (sideOf (node) ? "R" : "L")) + juce::String (number);
}

//==============================================================================
// Node strip: a thin raised row below the display with the controls of the selected node -
// name, LEFT / RIGHT (PhaseQ), shape (bell, low shelf, high shelf), BYPASS, FREQ, PHASE or
// GAIN, Q (small knobs) and X (remove) on the right
class NodeStrip final : public juce::Component,
                        private juce::Timer
{
public:
    NodeStrip (juce::AudioProcessorValueTreeState& s, const NodeSet& nodeSet) : state (s), set (nodeSet)
    {
        for (size_t i = 0; i < knobs.size(); ++i)
        {
            knobs[i].setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            knobs[i].setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 20);
            knobs[i].setColour (juce::Slider::textBoxBackgroundColourId, Colours::text);
            knobs[i].setColour (juce::Slider::textBoxTextColourId, Colours::accent);
            labels[i].setText (std::array { "FREQ", set.valueName, "Q" }[i], juce::dontSendNotification);
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
            const char* ids[] { set.freq[node], set.value[node], set.q[node] };
            for (size_t i = 0; i < knobs.size(); ++i)
                attachments[i] = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (state, ids[i], knobs[i]);
            shapeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, set.shape[node], shapeBox);
            bypassAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (state, set.bypass[node], bypassButton);
        }
        for (auto* c : controls())
            c->setVisible (node >= 0 && (set.side != nullptr || (c != &leftButton && c != &rightButton)));
        refresh();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        Plot::drawScrewedPanel (g, getLocalBounds().toFloat().withTrimmedRight (4.0f).withTrimmedBottom (5.0f), 4.5f);
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
            g.drawText ("no node selected  -  double-click into the display to create one", getLocalBounds().reduced (26, 0), juce::Justification::centredLeft);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().withTrimmedRight (4).withTrimmedBottom (5).reduced (22, 5);
        const auto gap = [&r] (int w) { r.removeFromLeft (w); };
        const auto centred = [] (juce::Rectangle<int> c) { return c.withSizeKeepingCentre (c.getWidth(), 30); };
        removeButton.setBounds (r.removeFromRight (32).withSizeKeepingCentre (30, 30));
        nameArea = r.removeFromLeft (44);
        gap (6);
        if (set.side != nullptr)
        {
            leftButton.setBounds (centred (r.removeFromLeft (62)));
            gap (4);
            rightButton.setBounds (centred (r.removeFromLeft (62)));
            gap (10);
        }
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
        auto* p = state.getParameter (set.side[node]);
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 ((float) side));
        p->endChangeGesture();
        refresh();
    }

    void refresh()
    {
        if (node < 0)
            return;
        if (set.side != nullptr)
        {
            const bool right = state.getRawParameterValue (set.side[node])->load() > 0.5f;
            leftButton.setToggleState (! right, juce::dontSendNotification);
            rightButton.setToggleState (right, juce::dontSendNotification);
        }
        if (const auto newName = nodeName (state, set, node); newName != name)
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
    const NodeSet& set;
    std::array<juce::Slider, 3> knobs;   // freq, phase / gain, Q
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
// Graph of nodes created by double-click (on a node: removes it); the strip below the
// display edits the selected node
class NodeListView : public NodeEditor
{
public:
    NodeListView (UlMaToProcessor& p, const juce::String& titleText, float bottomValue, float topValue, float gridStep, const NodeSet& nodeSet)
        : NodeEditor (p, titleText, bottomValue, topValue, gridStep), set (nodeSet), strip (p.apvts, set)
    {
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
        strip.setBounds (getLocalBounds().removeFromBottom (66).withTrimmedLeft ((int) plot.getX()));
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (const int n = nodeAt (e.position); n >= 0)
            remove (n);
        else if (plot.contains (e.position))
            create (e.position);
    }

protected:
    int numNodes() const override              { return set.count; }
    bool isActive (int n) const override       { return value (set.on[n]) > 0.5f; }
    const char* freqId (int n) const override   { return set.freq[n]; }
    const char* valueId (int n) const override  { return set.value[n]; }
    const char* widthId (int n) const override  { return set.q[n]; }

    bool isBypassed (int n) const    { return value (set.bypass[n]) > 0.5f; }

    void collectState (std::vector<float>& values) const override
    {
        for (int n = 0; n < set.count; ++n)
            for (auto* ids : { set.on, set.bypass, set.shape, set.side, set.freq, set.value, set.q })
                if (ids != nullptr)
                    values.push_back (value (ids[n]));
    }
    int shapeIndex (int n) const     { return juce::roundToInt (value (set.shape[n])); }
    juce::String nameOf (int n) const   { return nodeName (processor.apvts, set, n); }

    juce::String shapeName (int n) const
    {
        static constexpr const char* shapes[] { "BELL", "LOW SHELF", "HIGH SHELF" };
        return shapes[juce::jlimit (0, 2, shapeIndex (n))];
    }

    // Node marker: rounded (bell) or square (shelves), dashed outline for the right channel,
    // grey while bypassed
    void drawMarker (juce::Graphics& g, int n, juce::Point<float> c, bool on, bool dashed)
    {
        const auto r = juce::Rectangle<float> (26.0f, 16.0f).withCentre (c);
        const bool bypassed = isBypassed (n);
        const float corner = shapeIndex (n) == 0 ? 8.0f : 2.0f;
        const auto fill = bypassed ? Colours::display.brighter (0.25f) : curveColour;
        g.setColour (on ? fill : (dashed ? Colours::display : fill.withAlpha (0.45f)));
        g.fillRoundedRectangle (r, corner);
        g.setColour (on && ! bypassed ? Colours::background : Colours::text.withAlpha (bypassed ? 0.55f : 1.0f));
        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
        g.drawText (nameOf (n), r, juce::Justification::centred);
        g.setColour (Colours::text.withAlpha (bypassed ? 0.55f : 1.0f));
        juce::Path outline;
        outline.addRoundedRectangle (r, corner);
        if (dashed)
        {
            juce::Path dashes;
            const float pattern[] { 3.0f, 2.0f };
            juce::PathStrokeType (1.2f).createDashedStroke (dashes, outline, pattern, 2);
            g.fillPath (dashes);
        }
        else
            g.strokePath (outline, juce::PathStrokeType (1.2f));
    }

    void timerCallback() override
    {
        NodeEditor::timerCallback();
        updateStrip();   // nodes can also change from the host (automation, undo, presets)
    }

    const NodeSet& set;

private:
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
                setWithGesture (set.freq[n], 20.0f * std::pow (1000.0f, x));
                setWithGesture (set.value[n], juce::jlimit (bottom, top, top - (p.y - plot.getY()) / plot.getHeight() * (top - bottom)));
                setWithGesture (set.q[n], 1.0f);
                if (set.side != nullptr)
                    setWithGesture (set.side[n], 0.0f);
                setWithGesture (set.shape[n], 0.0f);
                setWithGesture (set.bypass[n], 0.0f);
                setWithGesture (set.on[n], 1.0f);
                select (n, true);
                return;
            }
    }

    // Remove a node; the selection moves on to another node (or none is left)
    void remove (int n)
    {
        if (n < 0)
            return;
        setWithGesture (set.on[n], 0.0f);
        setWithGesture (set.value[n], 0.0f);
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

    NodeStrip strip;
};

//==============================================================================
inline const NodeSet eqNodeSet { Tone::numBands, ParamIDs::eqOn, ParamIDs::eqBypass, ParamIDs::eqType, nullptr,
                                 ParamIDs::eqFreq, ParamIDs::eqGain, ParamIDs::eqQ, "GAIN" };
inline const NodeSet phaseNodeSet { Tone::numPhaseNodes, ParamIDs::phaseOn, ParamIDs::phaseBypass, ParamIDs::phaseShape, ParamIDs::phaseSide,
                                    ParamIDs::phaseFreq, ParamIDs::phaseAmount, ParamIDs::phaseQ, "PHASE" };

//==============================================================================
// Graphic EQ: bands created by double-click, linear phase; the processed signal faintly
// behind the curve
class GraphicEqView final : public NodeListView
{
public:
    explicit GraphicEqView (UlMaToProcessor& p)
        : NodeListView (p, "GRAPHIC EQ  [dB]  (linear phase: only the level changes)", -18.0f, 18.0f, 6.0f, eqNodeSet)
    {
        hint = "double-click: new band / remove band  -  drag a band  -  wheel: Q";
        animated = true;   // the spectrum behind the curves moves all the time
    }

protected:
    Tone::EqBand band (int n, bool withBypassed) const
    {
        return { isActive (n) && (withBypassed || ! isBypassed (n)), (Tone::EqType) shapeIndex (n),
                 value (ParamIDs::eqFreq[n]), value (ParamIDs::eqGain[n]), value (ParamIDs::eqQ[n]) };
    }

    // Each band faintly, the sum (without the bypassed bands) in the curve colour
    void buildCurves() override
    {
        Tone::EqSettings total;
        for (int n = 0; n < Tone::numBands; ++n)
        {
            total.band[(size_t) n] = band (n, false);
            if (isActive (n))
            {
                const auto b = band (n, true);
                addCurve ([&b] (float f) { return juce::Decibels::gainToDecibels (Tone::bandMagnitude (b.type, b.freq, b.gain, b.q, f)); },
                          Colours::text.withAlpha (0.2f), 1.0f);
            }
        }
        addCurve ([&total] (float f) { return juce::Decibels::gainToDecibels (Tone::eqMagnitude (total, f)); }, curveColour, 2.4f);
    }

    void drawNode (juce::Graphics& g, int n, juce::Point<float> c, bool on) override   { drawMarker (g, n, c, on, false); }

    juce::String describe (int n) const override
    {
        const float f = value (ParamIDs::eqFreq[n]), gain = value (ParamIDs::eqGain[n]);
        return nameOf (n) + "  " + shapeName (n) + (isBypassed (n) ? " (BYPASSED)   " : "   ")
             + (f < 1000.0f ? juce::String (f, 0) + " Hz" : juce::String (f / 1000.0f, 2) + " kHz") + "   "
             + (gain > 0.0f ? "+" : "") + juce::String (gain, 1) + " dB   Q " + juce::String (value (ParamIDs::eqQ[n]), 2);
    }

    void drawBackground (juce::Graphics& g) override
    {
        spectrum.draw (g, plot, [this] (float f) { return xOf (f); }, 0.07f);
    }

    void timerCallback() override
    {
        NodeListView::timerCallback();
        if (isShowing())
            spectrum.update (processor.getOutputProfile(), 0.15f);
    }

private:
    Plot::BackgroundSpectrum spectrum;
};

//==============================================================================
// PhaseQ: both channels in one graph - left solid, right dashed (both orange)
class PhaseQView final : public NodeListView
{
public:
    explicit PhaseQView (UlMaToProcessor& p)
        : NodeListView (p, juce::String (juce::CharPointer_UTF8 ("PHASEQ  [\xc2\xb0]  -  LEFT solid, RIGHT dashed  (only the phase changes, the level stays flat)")),
                        -180.0f, 180.0f, 45.0f, phaseNodeSet)
    {
        curveColour = Colours::correlation;
        hint = "double-click: new node / remove node  -  drag a node  -  wheel: Q";
    }

protected:
    int channelOf (int n) const   { return value (ParamIDs::phaseSide[n]) > 0.5f ? 1 : 0; }

    // withBypassed: bypassed nodes count too (for their own faint curve)
    Tone::PhaseSettings settings (bool withBypassed = false) const
    {
        Tone::PhaseSettings s;
        for (int n = 0; n < Tone::numPhaseNodes; ++n)
            s.node[(size_t) n] = { isActive (n) && (withBypassed || ! isBypassed (n)), channelOf (n), (Tone::PhaseShape) shapeIndex (n),
                                   value (ParamIDs::phaseFreq[n]), value (ParamIDs::phaseAmount[n]), value (ParamIDs::phaseQ[n]) };
        return s;
    }

    // Each node faintly, then right (dashed) and left (solid)
    void buildCurves() override
    {
        const auto all = settings (true), active = settings();
        for (int n = 0; n < Tone::numPhaseNodes; ++n)
            if (isActive (n))
                addCurve ([&all, n, c = channelOf (n)] (float f) { return juce::radiansToDegrees (Tone::phaseAt (all, c, f, n)); },
                          Colours::text.withAlpha (0.2f), 1.0f);
        addCurve ([&active] (float f) { return juce::radiansToDegrees (Tone::phaseAt (active, 1, f)); }, curveColour.darker (0.35f), 2.2f, true);
        addCurve ([&active] (float f) { return juce::radiansToDegrees (Tone::phaseAt (active, 0, f)); }, curveColour, 2.4f);
    }

    void drawNode (juce::Graphics& g, int n, juce::Point<float> c, bool on) override   { drawMarker (g, n, c, on, channelOf (n) == 1); }

    juce::String describe (int n) const override
    {
        const float f = value (ParamIDs::phaseFreq[n]), amount = value (ParamIDs::phaseAmount[n]);
        return nameOf (n) + "  " + shapeName (n) + (isBypassed (n) ? " (BYPASSED)   " : "   ")
             + (f < 1000.0f ? juce::String (f, 0) + " Hz" : juce::String (f / 1000.0f, 2) + " kHz") + "   "
             + (amount > 0.0f ? "+" : "") + juce::String (amount, 0) + juce::String (juce::CharPointer_UTF8 ("\xc2\xb0"))
             + "   Q " + juce::String (value (ParamIDs::phaseQ[n]), 2);
    }
};

//==============================================================================
// Limiter: scrolling history (input, output, gain reduction from the top), level meters
// and the loudness of the output
//==============================================================================
class LimiterView final : public juce::Component,
                          private juce::Timer
{
public:
    explicit LimiterView (UlMaToProcessor& p) : processor (p)
    {
        history.fill ({});
        startTimerHz (60);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours::text);
        g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        g.drawText ("LIMITER  -  input (light), output (dark) and gain reduction (red, from the top)", r.removeFromTop (18.0f), juce::Justification::centredLeft);

        auto side = r.removeFromRight (300.0f);
        r.removeFromRight (14.0f);
        plot = r.withTrimmedLeft (34.0f).withTrimmedBottom (2.0f);
        Plot::drawDisplay (g, plot);
        drawHistory (g);
        drawMeters (g, side);
    }

    // Click on the loudness numbers: reset (integrated, range, true peak)
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (numbersArea.contains (e.position))
        {
            processor.resetLimiterLoudness();
            peakHold = {};
        }
    }

private:
    static constexpr float range = 30.0f;   // dB shown: 0 ... -30
    static constexpr int numColumns = 2048;

    float yOf (float db) const   { return plot.getY() + plot.getHeight() * juce::jlimit (0.0f, 1.0f, -db / range); }

    void drawHistory (juce::Graphics& g)
    {
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        for (float db = 0.0f; db >= -range; db -= 3.0f)
        {
            g.setColour (juce::exactlyEqual (db, 0.0f) ? Colours::text.withAlpha (0.35f) : Colours::grid);
            g.drawHorizontalLine ((int) yOf (db), plot.getX(), plot.getRight());
            g.setColour (Colours::text.withAlpha (0.6f));
            g.drawText (juce::String (db, 0), juce::Rectangle<float> (plot.getX() - 36.0f, yOf (db) - 6.0f, 32.0f, 12.0f), juce::Justification::centredRight);
        }

        // One column per pixel, newest on the right; areas as paths (no gaps, no steps)
        const int count = juce::jmin (numColumns, (int) plot.getWidth());
        const auto columnAt = [&] (int i) -> const Limit::Column& { return history[(size_t) ((written - count + i + numColumns * 2) % numColumns)]; };
        const auto db = [] (float v) { return juce::Decibels::gainToDecibels (v, -100.0f); };
        const auto area = [&] (auto level, bool fromTop)
        {
            juce::Path p;
            const float base = fromTop ? plot.getY() : plot.getBottom();
            p.startNewSubPath (plot.getRight() - (float) count, base);
            for (int i = 0; i < count; ++i)
                p.lineTo (plot.getRight() - (float) (count - i) + 0.5f, yOf (level (columnAt (i))));
            p.lineTo (plot.getRight(), base);
            p.closeSubPath();
            return p;
        };
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (plot.toNearestInt());
        g.setColour (Colours::display.brighter (0.45f));                               // input
        g.fillPath (area ([&] (const Limit::Column& c) { return db (c.input); }, false));
        g.setColour (Colours::text.withAlpha (0.55f));                                // output
        g.fillPath (area ([&] (const Limit::Column& c) { return db (c.output); }, false));
        g.setColour (Colours::accent.withAlpha (0.85f));                              // gain reduction, from the top
        g.fillPath (area ([] (const Limit::Column& c) { return c.reduction; }, true));
        g.setColour (Colours::accent.darker (0.3f));
        g.strokePath (area ([] (const Limit::Column& c) { return c.reduction; }, true), juce::PathStrokeType (1.0f));

        const float ceiling = processor.apvts.getRawParameterValue (ParamIDs::limiterCeiling)->load();
        juce::Path line;
        line.startNewSubPath (plot.getX(), yOf (ceiling));
        line.lineTo (plot.getRight(), yOf (ceiling));
        juce::Path dashed;
        const float dashes[] { 6.0f, 4.0f };
        juce::PathStrokeType (1.2f).createDashedStroke (dashed, line, dashes, 2);
        g.setColour (Colours::correlation);
        g.fillPath (dashed);
        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
        g.drawText ("CEILING " + juce::String (ceiling, 1), juce::Rectangle<float> (plot.getX() + 6.0f, yOf (ceiling) + 2.0f, 120.0f, 12.0f), juce::Justification::centredLeft);
    }

    // Level meters (input L / R, reduction, output L / R) and the loudness numbers
    void drawMeters (juce::Graphics& g, juce::Rectangle<float> r)
    {
        const float ceilingDb = processor.apvts.getRawParameterValue (ParamIDs::limiterCeiling)->load();
        const bool truePeakOn = processor.apvts.getRawParameterValue (ParamIDs::limiterTruePeak)->load() > 0.5f;
        auto bars = r.removeFromTop (r.getHeight() * 0.55f);
        const auto scale = bars.removeFromLeft (26.0f);
        auto meterArea = bars.withTrimmedBottom (16.0f);
        Plot::drawDisplay (g, meterArea);
        const auto yIn = [&] (float d) { return meterArea.getY() + meterArea.getHeight() * juce::jlimit (0.0f, 1.0f, -d / 48.0f); };
        g.setFont (juce::Font (juce::FontOptions (9.5f)));
        for (float d = 0.0f; d >= -48.0f; d -= 6.0f)
        {
            g.setColour (Colours::text.withAlpha (0.6f));
            g.drawText (juce::String (d, 0), juce::Rectangle<float> (scale.getX(), yIn (d) - 6.0f, 22.0f, 12.0f), juce::Justification::centredRight);
            g.setColour (Colours::grid);
            g.drawHorizontalLine ((int) yIn (d), meterArea.getX(), meterArea.getRight());
        }

        // OUT L / R: true peak (light) and RMS (dark) in the same bar; GR from the top; LUFS:
        // momentary bar, short term mark, integrated line
        const float w = meterArea.getWidth() / 4.0f;
        const auto cellOf = [&] (int i) { return juce::Rectangle<float> (meterArea.getX() + (float) i * w, meterArea.getY(), w, meterArea.getHeight()).reduced (w * 0.2f, 2.0f); };
        const float peaks[] { shown.truePeakL, shown.truePeakR }, rms[] { shown.rmsL, shown.rmsR };
        for (int i = 0; i < 2; ++i)
        {
            const auto cell = cellOf (i);
            g.setColour (Colours::display.brighter (0.45f));
            g.fillRect (cell.withTop (yIn (peaks[i])));
            g.setColour (Colours::text.withAlpha (0.75f));
            g.fillRect (cell.reduced (cell.getWidth() * 0.18f, 0.0f).withTop (yIn (rms[i])));
            g.setColour (peakHold[(size_t) i] > ceilingDb + 0.05f ? Colours::correlation : Colours::text);
            g.fillRect (juce::Rectangle<float> (cell.getX(), yIn (peakHold[(size_t) i]) - 1.0f, cell.getWidth(), 2.0f));
        }
        g.setColour (Colours::accent);
        g.fillRect (cellOf (2).withBottom (yIn (shown.reduction)));
        {
            const auto cell = cellOf (3);
            g.setColour (Colours::accent.withAlpha (0.55f));
            g.fillRect (cell.withTop (yIn (shown.momentary)));
            g.setColour (Colours::accent);
            g.fillRect (juce::Rectangle<float> (cell.getX(), yIn (shown.shortTerm) - 1.0f, cell.getWidth(), 2.0f));
            if (shown.integrated > -70.0f)
            {
                g.setColour (Colours::correlation);
                g.fillRect (juce::Rectangle<float> (cell.getX() - 3.0f, yIn (shown.integrated) - 1.5f, cell.getWidth() + 6.0f, 3.0f));
            }
        }
        const char* names[] { "L", "R", "GR", "LUFS" };
        g.setColour (Colours::text);
        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
        for (int i = 0; i < 4; ++i)
            g.drawText (names[i], juce::Rectangle<float> (meterArea.getX() + (float) i * w, meterArea.getBottom() + 2.0f, w, 12.0f), juce::Justification::centred);
        g.setFont (juce::Font (juce::FontOptions (9.5f)));
        g.setColour (Colours::text.withAlpha (0.6f));
        g.drawText (truePeakOn ? "OUT  TP / RMS" : "OUT  PEAK / RMS", juce::Rectangle<float> (meterArea.getX(), bars.getBottom() - 2.0f, w * 2.0f, 10.0f), juce::Justification::centred);

        // Loudness (output): momentary, short term, integrated, range, true peak
        r.removeFromTop (12.0f);
        numbersArea = r;
        const auto lufs = [] (float v) { return v < -69.9f ? juce::String ("--.-") : juce::String (v, 1); };
        const std::pair<juce::String, juce::String> rows[] {
            { "MOMENTARY", lufs (shown.momentary) + " LUFS" }, { "SHORT TERM", lufs (shown.shortTerm) + " LUFS" },
            { "INTEGRATED", lufs (shown.integrated) + " LUFS" }, { "RANGE", juce::String (shown.range, 1) + " LU" },
            truePeakOn ? std::pair<juce::String, juce::String> { "TRUE PEAK", (shown.truePeak < -99.0f ? juce::String ("--.-") : juce::String (shown.truePeak, 1)) + " dBTP" }
                       : std::pair<juce::String, juce::String> { "RMS", (juce::jmax (shown.rmsL, shown.rmsR) < -99.0f ? juce::String ("--.-") : juce::String (juce::jmax (shown.rmsL, shown.rmsR), 1)) + " dB" } };
        const float h = r.getHeight() / 5.0f;
        for (const auto& [label, text] : rows)
        {
            auto line = r.removeFromTop (h).reduced (0.0f, 2.0f);
            g.setColour (Colours::text.withAlpha (0.75f));
            g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
            g.drawText (label, line.removeFromLeft (100.0f), juce::Justification::centredLeft);
            auto box = line.withSizeKeepingCentre (line.getWidth(), juce::jmin (line.getHeight(), 24.0f));
            g.setColour (Colours::text);
            g.fillRect (box);
            g.setColour (label == "TRUE PEAK" && shown.truePeak > ceilingDb + 0.05f
                             ? Colours::correlation : Colours::accent);
            g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 14.0f, juce::Font::bold)));
            g.drawText (text, box.reduced (8.0f, 0.0f), juce::Justification::centredRight);
            g.setColour (juce::Colours::black.withAlpha (0.6f));   // set in like a small screen
            g.drawLine (box.getX(), box.getY() + 0.5f, box.getRight(), box.getY() + 0.5f, 1.0f);
            g.setColour (juce::Colours::white.withAlpha (0.5f));
            g.drawLine (box.getX(), box.getBottom() - 0.5f, box.getRight(), box.getBottom() - 0.5f, 1.0f);
        }
        g.setColour (Colours::text.withAlpha (0.5f));
        g.setFont (juce::Font (juce::FontOptions (9.5f)));
        g.drawText ("click: reset", numbersArea.withHeight (10.0f).translated (0.0f, -11.0f), juce::Justification::centredRight);
    }

    void timerCallback() override
    {
        std::array<Limit::Column, 256> fresh;
        bool changed = false;
        for (int n; (n = processor.readLimiterColumns (fresh.data(), (int) fresh.size())) > 0; changed = true)
            for (int i = 0; i < n; ++i)
            {
                history[(size_t) written] = fresh[(size_t) i];
                written = (written + 1) % numColumns;
            }

        // Meters: fast up, slower down; peak marks fall 10 dB/s after 1 s
        const auto m = processor.getLimiterMeters();
        const auto before = std::array { shown.truePeakL, shown.truePeakR, shown.rmsL, shown.rmsR, shown.reduction, peakHold[0], peakHold[1],
                                         shown.momentary, shown.shortTerm, shown.integrated, shown.range, shown.truePeak };
        const auto follow = [] (float& v, float target) { v += (target > v ? 0.6f : 0.12f) * (target - v); };
        follow (shown.truePeakL, m.truePeakL); follow (shown.truePeakR, m.truePeakR);
        follow (shown.rmsL, m.rmsL);           follow (shown.rmsR, m.rmsR);
        shown.reduction += (m.reduction < shown.reduction ? 0.6f : 0.12f) * (m.reduction - shown.reduction);
        const float levels[] { m.truePeakL, m.truePeakR };
        for (size_t i = 0; i < 2; ++i)
        {
            if (levels[i] >= peakHold[i] || holdTicks[i] <= 0)
            {
                if (levels[i] >= peakHold[i]) holdTicks[i] = 60;
                peakHold[i] = juce::jmax (levels[i], peakHold[i] - 10.0f / 60.0f);
            }
            --holdTicks[i];
        }
        shown.momentary = m.momentary; shown.shortTerm = m.shortTerm; shown.integrated = m.integrated;
        shown.range = m.range; shown.truePeak = m.truePeak;

        // Repaint only when something visible moved (a silent, stopped limiter costs nothing)
        const auto after = std::array { shown.truePeakL, shown.truePeakR, shown.rmsL, shown.rmsR, shown.reduction, peakHold[0], peakHold[1],
                                        shown.momentary, shown.shortTerm, shown.integrated, shown.range, shown.truePeak };
        for (size_t i = 0; i < before.size() && ! changed; ++i)
            changed = std::abs (after[i] - before[i]) > 0.01f;
        if (changed && isShowing())
            repaint();
    }

    UlMaToProcessor& processor;
    std::array<Limit::Column, numColumns> history;
    int written = 0;
    juce::Rectangle<float> plot, numbersArea;
    Limit::Meters shown;
    std::array<float, 2> peakHold { -100.0f, -100.0f };
    std::array<int, 2> holdTicks {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LimiterView)
};
