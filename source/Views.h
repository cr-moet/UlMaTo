#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"
#include "UlMaToLookAndFeel.h"

//==============================================================================
// Shared drawing helpers: frequency axis (bands are evenly spaced = log frequency)
//==============================================================================
namespace Plot
{
    inline float xOfBand (float band, juce::Rectangle<float> r)
    {
        return r.getX() + r.getWidth() * band / (float) (Spectrum::numBands - 1);
    }

    inline float yOf (float value, float top, float bottom, juce::Rectangle<float> r)
    {
        return r.getY() + r.getHeight() * juce::jlimit (0.0f, 1.0f, (top - value) / (top - bottom));
    }

    inline void drawPanel (juce::Graphics& g, juce::Rectangle<float> r)
    {
        g.setColour (Colours::panel);
        g.fillRect (r);
        g.setColour (Colours::text.withAlpha (0.5f));
        g.drawRect (r, 1.0f);
    }

    // Frequency lines (labels below the plot if 'labels') and level lines (labels left)
    inline void drawGrid (juce::Graphics& g, juce::Rectangle<float> r, float top, float bottom, float step,
                          bool labels, const juce::String& unit = {})
    {
        g.setFont (juce::Font (juce::FontOptions (10.0f)));

        for (float hz : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
        {
            const float x = xOfBand (Spectrum::bandOfFrequency (hz), r);
            g.setColour (Colours::grid);
            g.drawVerticalLine ((int) x, r.getY(), r.getBottom());
            if (labels)
            {
                g.setColour (Colours::text.withAlpha (0.6f));
                g.drawText (hz < 1000.0f ? juce::String ((int) hz) : juce::String ((int) hz / 1000) + "k",
                            juce::Rectangle<float> (x - 20.0f, r.getBottom() + 1.0f, 40.0f, 12.0f), juce::Justification::centred);
            }
        }

        for (float v = top; v >= bottom - 0.001f; v -= step)
        {
            const float y = yOf (v, top, bottom, r);
            g.setColour (std::abs (v) < 0.001f ? Colours::text.withAlpha (0.3f) : Colours::grid);
            g.drawHorizontalLine ((int) y, r.getX(), r.getRight());
            g.setColour (Colours::text.withAlpha (0.6f));
            g.drawText ((v > 0.0f ? "+" : "") + juce::String (v, step < 1.0f ? 1 : 0) + unit,
                        juce::Rectangle<float> (r.getX() - 42.0f, y - 6.0f, 38.0f, 12.0f), juce::Justification::centredRight);
        }
    }

    // Curve over all bands; value (b) returns the value of band b
    template <typename Fn>
    void drawCurve (juce::Graphics& g, juce::Rectangle<float> r, float top, float bottom,
                    juce::Colour colour, float thickness, bool dashed, Fn value)
    {
        juce::Path path;
        for (int b = 0; b < Spectrum::numBands; ++b)
        {
            const juce::Point<float> p { xOfBand ((float) b, r), yOf (value (b), top, bottom, r) };
            if (b == 0) path.startNewSubPath (p); else path.lineTo (p);
        }

        g.setColour (colour);
        const juce::PathStrokeType stroke (thickness, juce::PathStrokeType::curved);
        if (dashed)
        {
            const float dashes[] { 5.0f, 4.0f };
            juce::Path dashedPath;
            stroke.createDashedStroke (dashedPath, path, dashes, 2);
            g.fillPath (dashedPath);
        }
        else
            g.strokePath (path, stroke);
    }

    inline void drawLegend (juce::Graphics& g, juce::Rectangle<float> r,
                            std::initializer_list<std::pair<juce::String, juce::Colour>> items)
    {
        g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        float x = r.getRight() - 6.0f;
        for (auto it = std::rbegin (items); it != std::rend (items); ++it)
        {
            const float w = (float) it->first.length() * 7.0f + 6.0f;
            x -= w;
            g.setColour (it->second);
            g.drawText (it->first, juce::Rectangle<float> (x, r.getY() + 3.0f, w, 14.0f), juce::Justification::centredRight);
        }
    }
}

//==============================================================================
// Reference song: drop target, waveform and the two range markers
//==============================================================================
class WaveformView final : public juce::Component,
                           public juce::FileDragAndDropTarget,
                           private juce::ChangeListener
{
public:
    explicit WaveformView (UlMaToProcessor& p) : processor (p)
    {
        thumbnail.addChangeListener (this);
        loadFile (processor.getReferenceFile());
    }

    ~WaveformView() override  { thumbnail.removeChangeListener (this); }

    std::function<void()> onFileChanged;

    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat();
        Plot::drawPanel (g, r);

        const double length = thumbnail.getTotalLength();
        if (length <= 0.0)
        {
            g.setColour (Colours::text.withAlpha (dragOver ? 1.0f : 0.55f));
            g.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
            g.drawText ("DROP A SONG HERE", r, juce::Justification::centred);
            g.setFont (juce::Font (juce::FontOptions (12.0f)));
            g.drawText ("WAV, AIFF, FLAC, MP3, Ogg", r.withTrimmedTop (40.0f), juce::Justification::centred);
            return;
        }

        const auto wave = r.reduced (1.0f).withTrimmedTop (18.0f).withTrimmedBottom (16.0f);
        g.setColour (Colours::text.withAlpha (0.75f));
        thumbnail.drawChannels (g, wave.toNearestInt(), 0.0, length, 1.0f);

        // Outside the selection: shaded; markers in the accent colour
        const auto sel = selection();
        const float x1 = xOf (sel.getStart()), x2 = xOf (sel.getEnd());
        g.setColour (Colours::text.withAlpha (0.18f));
        g.fillRect (juce::Rectangle<float> (r.getX(), wave.getY(), x1 - r.getX(), wave.getHeight()));
        g.fillRect (juce::Rectangle<float> (x2, wave.getY(), r.getRight() - x2, wave.getHeight()));

        g.setColour (Colours::accent);
        for (float x : { x1, x2 })
        {
            g.fillRect (x - 1.0f, wave.getY(), 2.0f, wave.getHeight());
            g.fillRect (x - 5.0f, wave.getY(), 10.0f, 6.0f);   // handle
        }

        g.setFont (juce::Font (juce::FontOptions (11.0f)));
        g.setColour (Colours::text);
        g.drawText (processor.getReferenceFile().getFileName(), r.reduced (6.0f, 2.0f).removeFromTop (16.0f),
                    juce::Justification::centredLeft);
        g.drawText ("Selection " + time (sel.getStart()) + " - " + time (sel.getEnd()) + "  (" + time (sel.getLength())
                        + ")      double-click: whole song",
                    r.reduced (6.0f, 1.0f).removeFromBottom (15.0f), juce::Justification::centredLeft);
        g.drawText ("Length " + time (length), r.reduced (6.0f, 2.0f).removeFromTop (16.0f), juce::Justification::centredRight);
    }

    //==========================================================================
    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        return files.size() == 1 && processor.getFormatManager().findFormatForFileExtension (juce::File (files[0]).getFileExtension()) != nullptr;
    }

    void fileDragEnter (const juce::StringArray&, int, int) override  { dragOver = true;  repaint(); }
    void fileDragExit (const juce::StringArray&) override              { dragOver = false; repaint(); }

    void filesDropped (const juce::StringArray& files, int, int) override
    {
        dragOver = false;
        const juce::File file (files[0]);
        processor.setReferenceFile (file);
        loadFile (file);
        if (onFileChanged != nullptr)
            onFileChanged();
    }

    //==========================================================================
    // Markers: drag the nearest one (a click moves it there); double-click = whole song
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (thumbnail.getTotalLength() <= 0.0)
            return;
        const auto sel = selection();
        draggingStart = std::abs (e.position.x - xOf (sel.getStart())) < std::abs (e.position.x - xOf (sel.getEnd()));
        mouseDrag (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (thumbnail.getTotalLength() <= 0.0)
            return;
        auto sel = selection();
        const double t = juce::jlimit (0.0, thumbnail.getTotalLength(), timeOf (e.position.x));
        if (draggingStart) sel.setStart (juce::jmin (t, sel.getEnd() - minSelection));
        else               sel.setEnd   (juce::jmax (t, sel.getStart() + minSelection));
        processor.setReferenceRange (sel.getStart(), sel.getEnd());
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        processor.setReferenceRange (0.0, 0.0);
        repaint();
    }

private:
    static constexpr double minSelection = 1.0;   // seconds

    void loadFile (const juce::File& file)
    {
        if (file.existsAsFile())
            thumbnail.setSource (new juce::FileInputSource (file));
        else
            thumbnail.clear();
        repaint();
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override  { repaint(); }

    // Selected range; 0 ... 0 (or invalid) = whole song
    juce::Range<double> selection() const
    {
        const auto r = processor.getReferenceRange();
        const double length = thumbnail.getTotalLength();
        return r.getLength() > 0.0 && r.getEnd() <= length ? r : juce::Range<double> (0.0, length);
    }

    float xOf (double t) const      { return (float) (t / juce::jmax (0.001, thumbnail.getTotalLength())) * (float) getWidth(); }
    double timeOf (float x) const   { return (double) x / juce::jmax (1, getWidth()) * thumbnail.getTotalLength(); }

    static juce::String time (double s)
    {
        return juce::String ((int) s / 60) + ":" + juce::String (std::fmod (s, 60.0), 1).paddedLeft ('0', 4);
    }

    UlMaToProcessor& processor;
    juce::AudioThumbnailCache cache { 2 };
    juce::AudioThumbnail thumbnail { 512, processor.getFormatManager(), cache };
    bool dragOver = false, draggingStart = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformView)
};

//==============================================================================
// Result of the reference analysis: stereo (L/R), mid/side or phase correlation;
// the learned input (LEARN) is drawn dashed for comparison
//==============================================================================
class ProfileView final : public juce::Component
{
public:
    enum class Kind { stereo, midSide, phase };

    ProfileView (Kind k, juce::String t) : kind (k), title (std::move (t)) {}

    void setProfiles (const Spectrum::Profile& ref, const Spectrum::Profile& learnedInput)
    {
        profile = ref;
        learned = learnedInput;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours::text);
        g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        g.drawText (title, r.removeFromTop (18.0f), juce::Justification::centredLeft);

        const auto plot = r.withTrimmedLeft (40.0f).withTrimmedBottom (14.0f);
        Plot::drawPanel (g, plot);

        const bool phase = kind == Kind::phase;
        const float top = phase ? 1.0f : 0.0f, bottom = phase ? -1.0f : -96.0f;
        Plot::drawGrid (g, plot, top, bottom, phase ? 0.5f : 12.0f, true);

        if (! profile.valid && ! learned.valid)
        {
            g.setColour (Colours::text.withAlpha (0.45f));
            g.drawText ("Not analysed yet", plot, juce::Justification::centred);
            return;
        }

        // Learned input first (dashed, lighter), then the reference on top
        for (const auto* p : { &learned, &profile })
        {
            if (! p->valid)
                continue;

            const bool dashed = p == &learned;
            const float alpha = dashed ? 0.6f : 1.0f, width = dashed ? 1.2f : 1.5f;
            const auto db = [] (const Spectrum::Curve& c) { return [&c] (int b) { return Spectrum::toDb (c[(size_t) b]); }; };

            switch (kind)
            {
                case Kind::stereo:
                    Plot::drawCurve (g, plot, top, bottom, Colours::text.withAlpha (alpha), width, dashed, db (p->right));
                    Plot::drawCurve (g, plot, top, bottom, Colours::accent.withAlpha (alpha), width, dashed, db (p->left));
                    break;
                case Kind::midSide:
                    Plot::drawCurve (g, plot, top, bottom, Colours::text.withAlpha (alpha), width, dashed, db (p->side));
                    Plot::drawCurve (g, plot, top, bottom, Colours::accent.withAlpha (alpha), width, dashed, db (p->mid));
                    break;
                case Kind::phase:
                    Plot::drawCurve (g, plot, top, bottom, Colours::accent.withAlpha (alpha), width, dashed,
                                     [p] (int b) { return p->correlation ((size_t) b); });
                    break;
            }
        }

        const auto inputHint = juce::String (learned.valid ? "   - - INPUT" : "");
        switch (kind)
        {
            case Kind::stereo:  Plot::drawLegend (g, plot, { { "L", Colours::accent }, { "R", Colours::text }, { inputHint, Colours::text.withAlpha (0.6f) } }); break;
            case Kind::midSide: Plot::drawLegend (g, plot, { { "MID", Colours::accent }, { "SIDE", Colours::text }, { inputHint, Colours::text.withAlpha (0.6f) } }); break;
            case Kind::phase:
                Plot::drawLegend (g, plot, { { inputHint, Colours::text.withAlpha (0.6f) } });
                g.setColour (Colours::text.withAlpha (0.55f));
                g.setFont (juce::Font (juce::FontOptions (10.0f)));
                g.drawText ("+1 mono    0 wide    -1 out of phase", plot.reduced (6.0f, 3.0f), juce::Justification::bottomLeft);
                break;
        }
    }

private:
    Kind kind;
    juce::String title;
    Spectrum::Profile profile, learned;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProfileView)
};

//==============================================================================
// Spectrum of the processed signal (after the MATCH knob) in dBFS; the reference is
// drawn dashed at its own level (static)
//==============================================================================
class LiveView final : public juce::Component,
                       private juce::Timer
{
public:
    explicit LiveView (UlMaToProcessor& p) : processor (p)  { startTimerHz (25); }

    void setReference (const Spectrum::Profile& p)  { reference = p; repaint(); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours::text);
        g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        g.drawText ("OUTPUT SPECTRUM  [dBFS]  +  PHASE CORRELATION", r.removeFromTop (18.0f), juce::Justification::centredLeft);

        const auto plot = r.withTrimmedLeft (40.0f).withTrimmedRight (34.0f).withTrimmedBottom (14.0f);
        constexpr float top = 0.0f, bottom = -96.0f;
        Plot::drawPanel (g, plot);
        Plot::drawGrid (g, plot, top, bottom, 12.0f, true);

        // Phase correlation on its own scale (right): +1 at the top, -1 at the bottom
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.setColour (Colours::correlation);
        for (float v : { 1.0f, 0.5f, 0.0f, -0.5f, -1.0f })
            g.drawText ((v > 0.0f ? "+" : "") + juce::String (v, 1),
                        juce::Rectangle<float> (plot.getRight() + 4.0f, Plot::yOf (v, 1.0f, -1.0f, plot) - 6.0f, 30.0f, 12.0f),
                        juce::Justification::centredLeft);

        if (reference.valid)
        {
            Plot::drawCurve (g, plot, top, bottom, Colours::text.withAlpha (0.55f), 1.2f, true,
                             [&] (int b) { return Spectrum::toDb (reference.side[(size_t) b]); });
            Plot::drawCurve (g, plot, top, bottom, Colours::accent.withAlpha (0.6f), 1.2f, true,
                             [&] (int b) { return Spectrum::toDb (reference.mid[(size_t) b]); });
            Plot::drawCurve (g, plot, 1.0f, -1.0f, Colours::correlation.withAlpha (0.55f), 1.2f, true,
                             [&] (int b) { return reference.correlation ((size_t) b); });
        }
        if (live.valid)
        {
            Plot::drawCurve (g, plot, top, bottom, Colours::text, 1.6f, false,
                             [&] (int b) { return Spectrum::toDb (live.side[(size_t) b]); });
            Plot::drawCurve (g, plot, top, bottom, Colours::accent, 1.6f, false,
                             [&] (int b) { return Spectrum::toDb (live.mid[(size_t) b]); });
            Plot::drawCurve (g, plot, 1.0f, -1.0f, Colours::correlation, 1.6f, false,
                             [&] (int b) { return live.correlation ((size_t) b); });
        }
        Plot::drawLegend (g, plot, { { "MID", Colours::accent }, { "SIDE", Colours::text }, { "CORRELATION", Colours::correlation },
                                     { "   - - REFERENCE", Colours::text.withAlpha (0.6f) } });

        if (! live.valid || ! reference.valid)
        {
            g.setColour (Colours::text.withAlpha (0.5f));
            g.drawText (! reference.valid ? "Analyse a reference song first" : "Waiting for signal",
                        plot, juce::Justification::centred);
        }
    }

private:
    void timerCallback() override
    {
        if (isShowing())
        {
            live = processor.getOutputProfile();
            repaint();
        }
    }

    UlMaToProcessor& processor;
    Spectrum::Profile live, reference;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LiveView)
};

//==============================================================================
// Dynamics page: one column per band with the level distributions (relative to their
// own median, so only the shape - the dynamics - is compared), the dynamic ranges,
// the ratio and the current gain of the multiband stage
//==============================================================================
class DynamicsView final : public juce::Component,
                           private juce::Timer
{
public:
    explicit DynamicsView (UlMaToProcessor& p) : processor (p)  { startTimerHz (20); }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();

        // Legend
        auto legend = area.removeFromTop (18.0f);
        g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        g.setColour (Colours::text);
        g.drawText ("DYNAMICS PER BAND  (level distribution around the median)", legend, juce::Justification::centredLeft);
        Plot::drawLegend (g, legend.translated (0.0f, -3.0f), { { "OUTPUT", Colours::accent }, { "   INPUT", Colours::text.withAlpha (0.45f) },
                                                                { "   - - REFERENCE", Colours::text } });
        area.removeFromTop (6.0f);

        const float gap = 12.0f, w = (area.getWidth() - gap * (Dynamics::numBands - 1)) / (float) Dynamics::numBands;
        for (size_t b = 0; b < (size_t) Dynamics::numBands; ++b)
        {
            paintBand (g, b, area.removeFromLeft (w));
            area.removeFromLeft (gap);
        }

        if (! reference.valid)
        {
            g.setColour (Colours::text.withAlpha (0.6f));
            g.setFont (juce::Font (juce::FontOptions (13.0f)));
            g.drawText ("Press ANALYZE to measure the dynamics of the reference", getLocalBounds().withTrimmedTop (60).removeFromTop (20),
                        juce::Justification::centred);
        }
    }

    void setReference (const Dynamics::Profile& p)   { reference = p; repaint(); }

private:
    static constexpr float lowest = -30.0f, highest = 18.0f;   // dB around the median

    void paintBand (juce::Graphics& g, size_t b, juce::Rectangle<float> r)
    {
        g.setColour (Colours::text);
        g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        g.drawText (Dynamics::bandNames[b], r.removeFromTop (16.0f), juce::Justification::centredLeft);
        g.setColour (Colours::text.withAlpha (0.6f));
        g.setFont (juce::Font (juce::FontOptions (11.0f)));
        g.drawText (Dynamics::bandRanges[b], r.removeFromTop (14.0f), juce::Justification::centredLeft);
        r.removeFromTop (4.0f);

        // Distributions
        auto info = r.removeFromBottom (92.0f);
        const auto plot = r.withTrimmedBottom (14.0f);
        Plot::drawPanel (g, plot);
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        for (float db : { -24.0f, -12.0f, 0.0f, 12.0f })
        {
            const float x = plot.getX() + plot.getWidth() * (db - lowest) / (highest - lowest);
            g.setColour (db == 0.0f ? Colours::text.withAlpha (0.3f) : Colours::grid);
            g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
            g.setColour (Colours::text.withAlpha (0.6f));
            g.drawText ((db > 0.0f ? "+" : "") + juce::String ((int) db), juce::Rectangle<float> (x - 15.0f, plot.getBottom() + 1.0f, 30.0f, 12.0f),
                        juce::Justification::centred);
        }

        const auto& input = state.fromLearned ? learned : state.input;
        drawDistribution (g, plot, input, b, Colours::text.withAlpha (0.12f), true, false);
        drawDistribution (g, plot, state.output, b, Colours::accent, false, false);
        drawDistribution (g, plot, reference, b, Colours::text, false, true);

        // Ranges, ratio, gain
        info.removeFromTop (6.0f);
        const auto row = [&] (const juce::String& label, const juce::String& value, juce::Colour colour)
        {
            auto line = info.removeFromTop (16.0f);
            g.setColour (Colours::text.withAlpha (0.7f));
            g.setFont (juce::Font (juce::FontOptions (11.0f)));
            g.drawText (label, line, juce::Justification::centredLeft);
            g.setColour (colour);
            g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
            g.drawText (value, line, juce::Justification::centredRight);
        };
        const auto range = [b] (const Dynamics::Profile& p)
        {
            return p.valid && p.active[b] ? juce::String (p.range[b], 1) + " dB" : juce::String ("-");
        };
        row ("RANGE REFERENCE", range (reference), Colours::text);
        row (state.fromLearned ? "RANGE INPUT (LEARNED)" : "RANGE INPUT", range (input), Colours::text.withAlpha (0.7f));
        row ("RANGE OUTPUT", range (state.output), Colours::accent);

        const float slope = state.slope[b];
        row ("RATIO", std::abs (slope - 1.0f) < 0.01f ? juce::String ("1 : 1")
                      : slope < 1.0f ? juce::String (1.0f / slope, 2) + " : 1"
                                     : "1 : " + juce::String (slope, 2) + "  EXP", Colours::text);

        // Gain meter: -12 ... +12 dB from the centre
        info.removeFromTop (6.0f);
        auto meter = info.removeFromTop (14.0f);
        g.setColour (Colours::panel);
        g.fillRect (meter);
        g.setColour (Colours::text.withAlpha (0.5f));
        g.drawRect (meter, 1.0f);
        const float centre = meter.getCentreX(), x = centre + meter.getWidth() * 0.5f * juce::jlimit (-1.0f, 1.0f, gains[b] / 12.0f);
        g.setColour (Colours::accent);
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (juce::jmin (centre, x), meter.getY() + 2.0f,
                                                                juce::jmax (centre, x), meter.getBottom() - 2.0f));
        g.setColour (Colours::text);
        g.drawVerticalLine ((int) centre, meter.getY(), meter.getBottom());
        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
        g.drawText ((gains[b] >= 0.0f ? "+" : "") + juce::String (gains[b], 1) + " dB", meter.reduced (4.0f, 0.0f),
                    gains[b] < 0.0f ? juce::Justification::centredRight : juce::Justification::centredLeft);
    }

    // Histogram of one band, shifted by its median, slightly smoothed, scaled to its maximum
    static void drawDistribution (juce::Graphics& g, juce::Rectangle<float> plot, const Dynamics::Profile& p, size_t b,
                                  juce::Colour colour, bool filled, bool dashed)
    {
        if (! p.valid || ! p.active[b])
            return;

        const auto& h = p.histogram[b];
        Dynamics::Histogram smoothed {};
        float peak = 0.0f;
        for (int i = 0; i < Dynamics::histogramSize; ++i)
        {
            float sum = 0.0f;
            for (int k = juce::jmax (0, i - 2); k <= juce::jmin (Dynamics::histogramSize - 1, i + 2); ++k)
                sum += h[(size_t) k];
            peak = juce::jmax (peak, smoothed[(size_t) i] = sum);
        }
        if (peak <= 0.0f)
            return;

        juce::Path path;
        bool started = false;
        for (int i = 0; i < Dynamics::histogramSize; ++i)
        {
            const float db = Dynamics::binLevel ((float) i) - p.median[b];
            if (db < lowest || db > highest)
                continue;
            const juce::Point<float> pt { plot.getX() + plot.getWidth() * (db - lowest) / (highest - lowest),
                                          plot.getBottom() - (plot.getHeight() - 6.0f) * smoothed[(size_t) i] / peak };
            if (! started) { path.startNewSubPath (pt); started = true; } else path.lineTo (pt);
        }
        if (! started)
            return;

        g.setColour (colour);
        if (filled)
        {
            const auto end = path.getCurrentPosition();
            const auto bounds = path.getBounds();
            path.lineTo (end.x, plot.getBottom());
            path.lineTo (bounds.getX(), plot.getBottom());
            path.closeSubPath();
            g.fillPath (path);
        }
        else if (dashed)
        {
            const float dashes[] { 5.0f, 4.0f };
            juce::Path d;
            juce::PathStrokeType (1.4f).createDashedStroke (d, path, dashes, 2);
            g.fillPath (d);
        }
        else
            g.strokePath (path, juce::PathStrokeType (1.8f));
    }

    void timerCallback() override
    {
        if (! isShowing())
            return;

        if (++slowTicks >= 10)   // distributions twice a second, gains 20 times
        {
            slowTicks = 0;
            state = processor.getDynamicsState();
            learned = processor.getLearnedDynamics();
        }
        for (size_t b = 0; b < (size_t) Dynamics::numBands; ++b)
            gains[b] += 0.4f * (processor.getDynamicsGainDb ((int) b) - gains[b]);
        repaint();
    }

    UlMaToProcessor& processor;
    MatchEngine::DynamicsState state;
    Dynamics::Profile reference, learned;
    Dynamics::Values gains {};
    int slowTicks = 10;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DynamicsView)
};

//==============================================================================
// Status LED: off, blinking (busy) or on
//==============================================================================
class Led final : public juce::Component,
                  private juce::Timer
{
public:
    Led() = default;

    enum class State { off, blinking, on };

    void setState (State s)
    {
        if (s == state)
            return;
        state = s;
        if (state == State::blinking) startTimerHz (3); else stopTimer();
        blinkOn = true;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat().withSizeKeepingCentre (14.0f, 14.0f);
        const bool lit = state == State::on || (state == State::blinking && blinkOn);
        g.setColour (Colours::text);
        g.fillEllipse (r);
        g.setColour (lit ? Colours::accent : Colours::text.brighter (0.3f));
        g.fillEllipse (r.reduced (2.5f));
        if (lit)
        {
            g.setColour (juce::Colours::white.withAlpha (0.45f));
            g.fillEllipse (r.reduced (5.0f).translated (-1.0f, -1.0f));
        }
    }

private:
    void timerCallback() override  { blinkOn = ! blinkOn; repaint(); }

    State state = State::off;
    bool blinkOn = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Led)
};
