#pragma once

#include "Views.h"

//==============================================================================
// Editors for PhaseQ and the graphic EQ: six nodes over log frequency. Drag a node:
// frequency (x) and value (y); mouse wheel: width / Q; double-click: back to 0.
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

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours::text);
        g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        g.drawText (title, r.removeFromTop (18.0f), juce::Justification::centredLeft);

        plot = r.withTrimmedLeft (40.0f).withTrimmedBottom (14.0f).withTrimmedRight (4.0f);
        Plot::drawPanel (g, plot);
        drawFrequencyGrid (g);
        for (float v = top; v >= bottom - 0.001f; v -= step)
        {
            const float y = yOf (v);
            g.setColour (std::abs (v) < 0.001f ? Colours::text.withAlpha (0.35f) : Colours::grid);
            g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
            g.setColour (Colours::text.withAlpha (0.6f));
            g.setFont (juce::Font (juce::FontOptions (10.0f)));
            g.drawText ((v > 0.0f ? "+" : "") + juce::String (v, 0), juce::Rectangle<float> (plot.getX() - 42.0f, y - 6.0f, 38.0f, 12.0f),
                        juce::Justification::centredRight);
        }

        drawBackground (g);

        // The individual nodes faintly, the sum strongly
        for (int n = 0; n < Tone::numBands; ++n)
            drawCurve (g, [this, n] (float f) { return nodeCurve (n, f); }, Colours::text.withAlpha (0.18f), 1.0f);
        drawCurve (g, [this] (float f) { return totalCurve (f); }, curveColour, 2.4f);

        for (int n = 0; n < Tone::numBands; ++n)
        {
            const auto c = nodePoint (n);
            const bool on = n == selected || n == hovered;
            g.setColour (on ? curveColour : Colours::panel);
            g.fillEllipse (juce::Rectangle<float> (16.0f, 16.0f).withCentre (c));
            g.setColour (on ? Colours::background : Colours::text);
            g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
            g.drawText (juce::String (n + 1), juce::Rectangle<float> (16.0f, 16.0f).withCentre (c), juce::Justification::centred);
            g.setColour (Colours::text);
            g.drawEllipse (juce::Rectangle<float> (16.0f, 16.0f).withCentre (c), 1.2f);
        }

        // Values of the selected (or hovered) node
        const int shown = hovered >= 0 ? hovered : selected;
        if (shown >= 0)
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

    void mouseDown (const juce::MouseEvent& e) override
    {
        selected = nodeAt (e.position);
        if (selected >= 0)
            for (auto* id : { freqId (selected), valueId (selected) })
                parameter (id)->beginChangeGesture();
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (selected < 0)
            return;
        const float x = juce::jlimit (0.0f, 1.0f, (e.position.x - plot.getX()) / plot.getWidth());
        set (freqId (selected), 20.0f * std::pow (1000.0f, x));
        set (valueId (selected), juce::jlimit (bottom, top, top - (e.position.y - plot.getY()) / plot.getHeight() * (top - bottom)));
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (selected >= 0)
            for (auto* id : { freqId (selected), valueId (selected) })
                parameter (id)->endChangeGesture();
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
    virtual const char* freqId (int n) const = 0;
    virtual const char* valueId (int n) const = 0;
    virtual const char* widthId (int n) const = 0;
    virtual float nodeCurve (int n, float f) const = 0;    // curve of one node (display units)
    virtual float totalCurve (float f) const = 0;
    virtual juce::String describe (int n) const = 0;
    virtual void drawBackground (juce::Graphics&) {}

    float value (const char* id) const   { return processor.apvts.getRawParameterValue (id)->load(); }

    float xOf (float f) const   { return plot.getX() + plot.getWidth() * std::log (f / 20.0f) / std::log (1000.0f); }
    float yOf (float v) const   { return plot.getY() + plot.getHeight() * juce::jlimit (0.0f, 1.0f, (top - v) / (top - bottom)); }

    UlMaToProcessor& processor;
    juce::String title, hint { "drag a node  -  wheel: width  -  double-click: reset" };
    juce::Colour curveColour = Colours::accent;
    juce::Rectangle<float> plot;
    float bottom, top, step;

private:
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

    int nodeAt (juce::Point<float> p) const
    {
        int best = -1;
        float distance = 14.0f;
        for (int n = 0; n < Tone::numBands; ++n)
            if (const float d = nodePoint (n).getDistanceFrom (p); d < distance)
            {
                distance = d;
                best = n;
            }
        return best;
    }

    template <typename Fn>
    void drawCurve (juce::Graphics& g, Fn curve, juce::Colour colour, float thickness) const
    {
        juce::Path path;
        for (float x = plot.getX(); x <= plot.getRight(); x += 2.0f)
        {
            const float f = 20.0f * std::pow (1000.0f, (x - plot.getX()) / plot.getWidth());
            const juce::Point<float> pt { x, yOf (curve (f)) };
            if (x == plot.getX()) path.startNewSubPath (pt); else path.lineTo (pt);
        }
        g.setColour (colour);
        g.strokePath (path, juce::PathStrokeType (thickness, juce::PathStrokeType::curved));
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

    int selected = -1, hovered = -1;
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
class PhaseQView final : public NodeEditor
{
public:
    explicit PhaseQView (UlMaToProcessor& p)
        : NodeEditor (p, juce::String (juce::CharPointer_UTF8 ("PHASEQ  [\xc2\xb0]  (only the phase changes, the level stays flat)")),
                      -180.0f, 180.0f, 45.0f)
    {
        curveColour = Colours::correlation;
        hint = juce::String (juce::CharPointer_UTF8 ("drag a node  -  wheel: width  -  double-click: 0\xc2\xb0"));
    }

protected:
    const char* freqId (int n) const override   { return ParamIDs::phaseFreq[n]; }
    const char* valueId (int n) const override  { return ParamIDs::phaseAmount[n]; }
    const char* widthId (int n) const override  { return ParamIDs::phaseWidth[n]; }

    Tone::PhaseSettings settings (bool withRotation, int only = -1) const
    {
        Tone::PhaseSettings s;
        for (size_t n = 0; n < (size_t) Tone::numBands; ++n)
        {
            s.freq[n]   = value (ParamIDs::phaseFreq[n]);
            s.amount[n] = only < 0 || only == (int) n ? value (ParamIDs::phaseAmount[n]) : 0.0f;
            s.width[n]  = value (ParamIDs::phaseWidth[n]);
        }
        s.rotation = withRotation ? value (ParamIDs::phaseRotation) : 0.0f;
        return s;
    }

    float nodeCurve (int n, float f) const override   { return juce::radiansToDegrees (Tone::phaseAt (settings (false, n), f)); }
    float totalCurve (float f) const override         { return juce::radiansToDegrees (Tone::phaseAt (settings (true), f)); }

    juce::String describe (int n) const override
    {
        const float f = value (ParamIDs::phaseFreq[n]), amount = value (ParamIDs::phaseAmount[n]);
        return juce::String (n + 1) + "   " + (f < 1000.0f ? juce::String (f, 0) + " Hz" : juce::String (f / 1000.0f, 2) + " kHz") + "   "
             + (amount > 0.0f ? "+" : "") + juce::String (amount, 0) + juce::String (juce::CharPointer_UTF8 ("\xc2\xb0"))
             + "   width " + juce::String (value (ParamIDs::phaseWidth[n]), 2) + " oct";
    }
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
        Plot::drawPanel (g, plot);

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
