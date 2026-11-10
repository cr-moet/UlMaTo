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

    // The reference is smoothed over frequency like the live curves, so both compare
    void setReference (const Spectrum::Profile& p)
    {
        reference = p;
        if (p.valid)
            curves (p, referenceMid, referenceSide, referenceCorrelation);
        repaint();
    }

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
                             [&] (int b) { return referenceSide[(size_t) b]; });
            Plot::drawCurve (g, plot, top, bottom, Colours::accent.withAlpha (0.6f), 1.2f, true,
                             [&] (int b) { return referenceMid[(size_t) b]; });
            Plot::drawCurve (g, plot, 1.0f, -1.0f, Colours::correlation.withAlpha (0.55f), 1.2f, true,
                             [&] (int b) { return referenceCorrelation[(size_t) b]; });
        }
        if (live.valid)
        {
            Plot::drawCurve (g, plot, top, bottom, Colours::text, 1.6f, false,
                             [&] (int b) { return shownSide[(size_t) b]; });
            Plot::drawCurve (g, plot, top, bottom, Colours::accent, 1.6f, false,
                             [&] (int b) { return shownMid[(size_t) b]; });
            Plot::drawCurve (g, plot, 1.0f, -1.0f, Colours::correlation, 1.6f, false,
                             [&] (int b) { return shownCorrelation[(size_t) b]; });
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
    // Display only (the processing is not affected): mid and side averaged over 1/3 octave,
    // the correlation over 2/3 octave; over time the correlation is calmed down more, too
    static constexpr int levelWidth = 2, correlationWidth = 4;     // bands on each side (1/12 octave each)
    static constexpr float levelSpeed = 0.35f, correlationSpeed = 0.12f;   // per update (25 per second)

    static Spectrum::Curve average (const Spectrum::Curve& in, int width)
    {
        Spectrum::Curve out {};
        for (int b = 0; b < Spectrum::numBands; ++b)
        {
            float sum = 0.0f;
            int count = 0;
            for (int i = juce::jmax (0, b - width); i <= juce::jmin (Spectrum::numBands - 1, b + width); ++i, ++count)
                sum += in[(size_t) i];
            out[(size_t) b] = sum / (float) count;
        }
        return out;
    }

    // Mid / side in dB (averaged as power) and the correlation, smoothed over frequency
    static void curves (const Spectrum::Profile& p, Spectrum::Curve& mid, Spectrum::Curve& side, Spectrum::Curve& correlation)
    {
        Spectrum::Curve c {};
        for (size_t b = 0; b < (size_t) Spectrum::numBands; ++b)
            c[b] = p.correlation (b);
        const auto m = average (p.mid, levelWidth), s = average (p.side, levelWidth);
        for (size_t b = 0; b < (size_t) Spectrum::numBands; ++b)
        {
            mid[b]  = Spectrum::toDb (m[b]);
            side[b] = Spectrum::toDb (s[b]);
        }
        correlation = average (c, correlationWidth);
    }

    void timerCallback() override
    {
        if (! isShowing())
            return;

        live = processor.getOutputProfile();
        if (live.valid)
        {
            Spectrum::Curve mid, side, correlation;
            curves (live, mid, side, correlation);
            for (size_t b = 0; b < (size_t) Spectrum::numBands; ++b)
            {
                shownMid[b]  = started ? shownMid[b]  + levelSpeed * (mid[b]  - shownMid[b])  : mid[b];
                shownSide[b] = started ? shownSide[b] + levelSpeed * (side[b] - shownSide[b]) : side[b];
                shownCorrelation[b] = started ? shownCorrelation[b] + correlationSpeed * (correlation[b] - shownCorrelation[b])
                                              : correlation[b];
            }
            started = true;
        }
        repaint();
    }

    UlMaToProcessor& processor;
    Spectrum::Profile live, reference;
    Spectrum::Curve shownMid {}, shownSide {}, shownCorrelation {};
    Spectrum::Curve referenceMid {}, referenceSide {}, referenceCorrelation {};
    bool started = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LiveView)
};

//==============================================================================
// Dynamics page in multiband compressor style: one graph over frequency with the five
// bands (crossovers marked, the output spectrum faintly behind).
//  Upper part: the current gain of each band as a bar from 0 dB (red: cut, orange: boost),
//              with a peak mark and the value.
//  Lower part: per band name, range and ratio, and the dynamic range of reference,
//              input and output as three bars (0 ... 24 dB across the band).
//==============================================================================
class DynamicsView final : public juce::Component,
                           private juce::Timer
{
public:
    explicit DynamicsView (UlMaToProcessor& p) : processor (p)  { startTimerHz (30); }

    void setReference (const Dynamics::Profile& p)   { reference = p; repaint(); }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        g.setColour (Colours::text);
        g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        auto titleRow = area.removeFromTop (18.0f);
        g.drawText ("MULTIBAND DYNAMICS", titleRow, juce::Justification::centredLeft);
        Plot::drawLegend (g, titleRow.translated (0.0f, -3.0f), { { "CUT", Colours::accent }, { "   BOOST", Colours::correlation },
                                                                  { "   RANGE: REF / IN / OUT", Colours::text.withAlpha (0.6f) } });
        area.removeFromTop (4.0f);

        const auto plot = area.withTrimmedLeft (40.0f).withTrimmedBottom (14.0f);
        Plot::drawPanel (g, plot);
        const float split = plot.getY() + plot.getHeight() * 0.66f;   // gain above, ranges below
        const auto gainArea = juce::Rectangle<float>::leftTopRightBottom (plot.getX(), plot.getY() + 34.0f, plot.getRight(), split - 6.0f);

        // Bands: alternating tint, crossover lines and frequencies
        static constexpr float edges[] { 20.0f, 120.0f, 600.0f, 1200.0f, 9000.0f, 20000.0f };
        const auto xOf = [&plot] (float f) { return plot.getX() + plot.getWidth() * std::log (f / 20.0f) / std::log (1000.0f); };
        for (int b = 0; b < Dynamics::numBands; ++b)
            if (b % 2 == 1)
            {
                g.setColour (Colours::text.withAlpha (0.035f));
                g.fillRect (juce::Rectangle<float>::leftTopRightBottom (xOf (edges[b]), plot.getY(), xOf (edges[b + 1]), plot.getBottom()));
            }

        drawSpectrum (g, plot, xOf);

        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        for (int b = 1; b < Dynamics::numBands; ++b)
        {
            const float x = xOf (edges[b]);
            g.setColour (Colours::text.withAlpha (0.45f));
            const float dashes[] { 4.0f, 3.0f };
            g.drawDashedLine ({ x, plot.getY(), x, plot.getBottom() }, dashes, 2, 1.0f);
            g.setColour (Colours::text.withAlpha (0.7f));
            g.drawText (edges[b] < 1000.0f ? juce::String ((int) edges[b]) : juce::String (edges[b] / 1000.0f, 1) + "k",
                        juce::Rectangle<float> (x - 24.0f, plot.getBottom() + 1.0f, 48.0f, 12.0f), juce::Justification::centred);
        }

        // Gain scale -12 ... +4 dB
        const auto yOf = [&gainArea] (float db) { return gainArea.getY() + gainArea.getHeight() * (4.0f - juce::jlimit (-12.0f, 4.0f, db)) / 16.0f; };
        for (float db : { 4.0f, 0.0f, -4.0f, -8.0f, -12.0f })
        {
            g.setColour (db == 0.0f ? Colours::text.withAlpha (0.5f) : Colours::grid);
            g.drawHorizontalLine ((int) yOf (db), plot.getX(), plot.getRight());
            g.setColour (Colours::text.withAlpha (0.6f));
            g.drawText ((db > 0.0f ? "+" : "") + juce::String ((int) db), juce::Rectangle<float> (plot.getX() - 42.0f, yOf (db) - 6.0f, 38.0f, 12.0f),
                        juce::Justification::centredRight);
        }
        g.setColour (Colours::text.withAlpha (0.3f));
        g.drawHorizontalLine ((int) split, plot.getX(), plot.getRight());

        for (size_t b = 0; b < (size_t) Dynamics::numBands; ++b)
        {
            const auto column = juce::Rectangle<float>::leftTopRightBottom (xOf (edges[b]), plot.getY(), xOf (edges[b + 1]), plot.getBottom()).reduced (6.0f, 0.0f);
            paintBand (g, b, column, gainArea, split, yOf);
        }

        if (! reference.valid)
        {
            g.setColour (Colours::text.withAlpha (0.6f));
            g.setFont (juce::Font (juce::FontOptions (13.0f)));
            g.drawText ("Press ANALYZE to measure the dynamics of the reference", plot.withHeight (24.0f).translated (0.0f, plot.getHeight() * 0.3f),
                        juce::Justification::centred);
        }
    }

private:
    template <typename YOf>
    void paintBand (juce::Graphics& g, size_t b, juce::Rectangle<float> column, juce::Rectangle<float> gainArea, float split, YOf yOf)
    {
        // Name and frequency range at the top
        g.setColour (Colours::text);
        g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        g.drawText (Dynamics::bandNames[b], column.withY (column.getY() + 4.0f).withHeight (14.0f), juce::Justification::centred);
        g.setColour (Colours::text.withAlpha (0.6f));
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.drawText (Dynamics::bandRanges[b], column.withY (column.getY() + 18.0f).withHeight (12.0f), juce::Justification::centred);

        // Gain bar from 0 dB, peak mark, value
        const float gain = gains[b], zero = yOf (0.0f), y = yOf (gain);
        const auto bar = juce::Rectangle<float>::leftTopRightBottom (column.getX() + 6.0f, juce::jmin (zero, y), column.getRight() - 6.0f, juce::jmax (zero, y));
        g.setColour ((gain < 0.0f ? Colours::accent : Colours::correlation).withAlpha (0.85f));
        g.fillRect (bar);
        for (float peak : { cutPeaks[b], boostPeaks[b] })
            if (std::abs (peak) > 0.05f)
            {
                g.setColour ((peak < 0.0f ? Colours::accent : Colours::correlation).withAlpha (0.9f));
                g.fillRect (bar.getX(), yOf (peak) - 1.0f, bar.getWidth(), 2.0f);
            }
        const auto valueBox = juce::Rectangle<float> (64.0f, 18.0f).withCentre ({ column.getCentreX(), gainArea.getBottom() - 8.0f });
        g.setColour (Colours::text);
        g.fillRect (valueBox);
        g.setColour (Colours::accent);
        g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::bold)));
        g.drawText ((shownGains[b] < -0.05f ? "-" : shownGains[b] > 0.05f ? "+" : " ") + juce::String (std::abs (shownGains[b]), 1) + " dB",
                    valueBox.reduced (4.0f, 0.0f), juce::Justification::centredRight);

        // Lower part: ratio and the three ranges
        auto lower = column.withTop (split + 6.0f).withTrimmedBottom (4.0f);
        const float slope = state.slope[b];
        g.setColour (Colours::text);
        g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        g.drawText ("RATIO " + (std::abs (slope - 1.0f) < 0.01f ? juce::String ("1:1")
                                : slope < 1.0f ? juce::String (1.0f / slope, 2) + ":1"
                                               : "1:" + juce::String (slope, 2) + " EXP"),
                    lower.removeFromTop (16.0f), juce::Justification::centred);
        lower.removeFromTop (4.0f);

        const auto& input = state.fromLearned ? learned : state.input;
        const float rowHeight = lower.getHeight() / 3.0f;
        const auto rangeBar = [&] (const char* label, const Dynamics::Profile& p, juce::Colour colour, bool dashed)
        {
            auto row = lower.removeFromTop (juce::jmin (30.0f, rowHeight));
            g.setColour (Colours::text.withAlpha (0.65f));
            g.setFont (juce::Font (juce::FontOptions (10.0f)));
            const bool known = p.valid && p.active[b];
            g.drawText (juce::String (label) + (known ? "  " + juce::String (p.range[b], 1) + " dB" : juce::String ("  -")),
                        row.removeFromTop (12.0f), juce::Justification::centredLeft);
            auto track = row.removeFromTop (7.0f);
            g.setColour (Colours::text.withAlpha (0.08f));
            g.fillRect (track);
            if (! known)
                return;
            const auto filled = track.withWidth (track.getWidth() * juce::jlimit (0.0f, 1.0f, p.range[b] / 24.0f));
            if (dashed)
            {
                g.setColour (colour);
                g.drawRect (filled, 1.0f);
            }
            else
            {
                g.setColour (colour);
                g.fillRect (filled);
            }
        };
        rangeBar ("REF", reference, Colours::text, true);
        rangeBar ("IN", input, Colours::text.withAlpha (0.35f), false);
        rangeBar ("OUT", state.output, Colours::accent, false);
    }

    // The processed signal behind everything (mid, faint)
    template <typename XOf>
    void drawSpectrum (juce::Graphics& g, juce::Rectangle<float> plot, XOf xOf) const
    {
        if (! output.valid)
            return;
        juce::Path area;
        area.startNewSubPath (plot.getX(), plot.getBottom());
        for (int b = 0; b < Spectrum::numBands; ++b)
        {
            const float level = juce::jlimit (0.0f, 1.0f, (Spectrum::toDb (output.mid[(size_t) b]) + 96.0f) / 96.0f);
            area.lineTo (xOf (Spectrum::bandFrequency ((float) b)), plot.getBottom() - level * plot.getHeight());
        }
        area.lineTo (plot.getRight(), plot.getBottom());
        area.closeSubPath();
        g.setColour (Colours::text.withAlpha (0.06f));
        g.fillPath (area);
    }

    void timerCallback() override
    {
        if (! isShowing())
            return;

        if (++slowTicks >= 15)   // distributions and spectrum twice a second, gains 30 times
        {
            slowTicks = 0;
            state = processor.getDynamicsState();
            learned = processor.getLearnedDynamics();
            output = processor.getOutputProfile();
        }

        // Bars: quickly towards more gain change, back slowly; peak marks fall 3 dB/s
        for (size_t b = 0; b < (size_t) Dynamics::numBands; ++b)
        {
            const float target = processor.getDynamicsGainDb ((int) b);
            gains[b] += (std::abs (target) > std::abs (gains[b]) ? 0.5f : 0.15f) * (target - gains[b]);
            cutPeaks[b]   = juce::jmin (0.0f, juce::jmin (gains[b], cutPeaks[b] + 0.1f));
            boostPeaks[b] = juce::jmax (0.0f, juce::jmax (gains[b], boostPeaks[b] - 0.1f));
        }
        if (++numberTicks >= 6)
        {
            numberTicks = 0;
            shownGains = gains;
        }
        repaint();
    }

    UlMaToProcessor& processor;
    MatchEngine::DynamicsState state;
    Dynamics::Profile reference, learned;
    Spectrum::Profile output;
    Dynamics::Values gains {}, shownGains {}, cutPeaks {}, boostPeaks {};
    int slowTicks = 15, numberTicks = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DynamicsView)
};

//==============================================================================
// Tabs of the stages in processing order (left = first, arrows = signal flow), drawn
// as hardware buttons: the selected one is pressed in, the LED shows whether the stage
// is active (a click on the LED bypasses it). Click: select the page. Drag: the tab
// follows the mouse, the others slide aside, on release it snaps into its place.
//==============================================================================
class StageTabs final : public juce::Component,
                        private juce::Timer
{
public:
    using Order = MatchEngine::Order;
    using Bypassed = std::array<bool, MatchEngine::numStages>;

    StageTabs()   { setMouseCursor (juce::MouseCursor::PointingHandCursor); }

    std::function<void (int)>          onSelect;        // page = stage
    std::function<void (const Order&)> onOrderChange;
    std::function<void (int)>          onBypassToggle;  // stage

    void setState (int page, const Order& newOrder, const Bypassed& newBypassed)
    {
        if (page != selected || newOrder != order || newBypassed != bypassed)
        {
            selected = page;
            order = newOrder;
            bypassed = newBypassed;
            animate();
        }
    }

    void resized() override
    {
        for (int slot = 0; slot < numSlots; ++slot)   // no animation when the size changes
            position[(size_t) order[(size_t) slot]] = slotX (slot);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        // Arrows between the slots
        for (int slot = 0; slot < numSlots - 1; ++slot)
        {
            const auto a = slotX (slot) + tabWidth() + arrowGap * 0.5f, y = (float) getHeight() * 0.5f - 1.0f;
            juce::Path arrow;
            arrow.addTriangle (a - 5.0f, y - 6.0f, a - 5.0f, y + 6.0f, a + 6.0f, y);
            g.setColour (Colours::text);
            g.fillPath (arrow);
        }

        for (int page : order)
            if (page != dragged)
                drawTab (g, page, tabAt (position[(size_t) page]));
        if (dragged >= 0)   // on top, where the mouse is
            drawTab (g, dragged, tabAt (position[(size_t) dragged]).translated (0.0f, -2.0f));
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        pressed = -1;
        for (int slot = 0; slot < numSlots; ++slot)
        {
            const auto tab = tabAt (slotX (slot));
            if (! tab.contains (e.position))
                continue;

            const int page = order[(size_t) slot];
            if (ledArea (tab).contains (e.position))
            {
                if (onBypassToggle != nullptr)
                    onBypassToggle (page);
                return;
            }
            pressed = page;
            grabOffset = e.position.x - tab.getX();
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (pressed < 0 || (dragged < 0 && e.getDistanceFromDragStartX() * e.getDistanceFromDragStartX() < 25))
            return;
        dragged = pressed;
        position[(size_t) dragged] = juce::jlimit (0.0f, slotX (numSlots - 1), e.position.x - grabOffset);
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        animate();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragged >= 0)
        {
            const auto newOrder = orderWhileDragging();
            dragged = -1;
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
            if (newOrder != order)
            {
                order = newOrder;
                if (onOrderChange != nullptr)
                    onOrderChange (newOrder);
            }
            animate();   // snaps into its slot
        }
        else if (pressed >= 0 && onSelect != nullptr)
            onSelect (pressed);
        pressed = -1;
    }

private:
    static constexpr int numSlots = MatchEngine::numStages;
    static constexpr float arrowGap = 22.0f;

    float tabWidth() const              { return juce::jmin (170.0f, ((float) getWidth() - (numSlots - 1) * arrowGap) / (float) numSlots); }
    float slotX (int slot) const        { return (float) slot * (tabWidth() + arrowGap); }
    juce::Rectangle<float> tabAt (float x) const   { return { x, 0.0f, tabWidth(), (float) getHeight() }; }

    // LED window at the top of the face (and the area that reacts to clicks on it)
    static juce::Rectangle<float> ledWindow (juce::Rectangle<float> tab, bool down)
    {
        const auto face = UlMaToLookAndFeel::buttonFaceArea (tab, down);
        return juce::Rectangle<float> (16.0f, 4.0f).withCentre ({ face.getCentreX(), face.getY() + 6.0f });
    }
    static juce::Rectangle<float> ledArea (juce::Rectangle<float> tab)   { return ledWindow (tab, false).expanded (14.0f, 6.0f); }

    // Order while a tab is dragged: it goes to the slot nearest to where it is now
    Order orderWhileDragging() const
    {
        if (dragged < 0)
            return order;
        const int target = juce::jlimit (0, numSlots - 1, juce::roundToInt (position[(size_t) dragged] / (tabWidth() + arrowGap)));
        Order result {};
        int n = 0;
        for (int slot = 0; slot < numSlots; ++slot)
        {
            if (slot == target)
                result[(size_t) slot] = dragged;
            else
            {
                while (order[(size_t) n] == dragged)
                    ++n;
                result[(size_t) slot] = order[(size_t) n++];
            }
        }
        return result;
    }

    void animate()   { if (! isTimerRunning()) startTimerHz (60); repaint(); }

    // Every tab glides towards its slot (the dragged one stays at the mouse)
    void timerCallback() override
    {
        const auto target = orderWhileDragging();
        bool moving = dragged >= 0;
        for (int slot = 0; slot < numSlots; ++slot)
        {
            const int page = target[(size_t) slot];
            if (page == dragged)
                continue;
            auto& x = position[(size_t) page];
            const float goal = slotX (slot);
            x += 0.3f * (goal - x);
            if (std::abs (goal - x) < 0.5f)
                x = goal;
            else
                moving = true;
        }
        repaint();
        if (! moving)
            stopTimer();
    }

    void drawTab (juce::Graphics& g, int page, juce::Rectangle<float> r) const
    {
        const bool down = page == selected, off = bypassed[(size_t) page];
        UlMaToLookAndFeel::drawHardwareButton (g, r, UlMaToLookAndFeel::buttonFace(), false, down);
        UlMaToLookAndFeel::drawLed (g, ledWindow (r, down), ! off);

        auto face = UlMaToLookAndFeel::buttonFaceArea (r, down);
        face.removeFromTop (9.0f);

        // Grip: the tab can be moved
        g.setColour (Colours::text.withAlpha (0.35f));
        for (int i = 0; i < 3; ++i)
            g.fillRect (face.getX() + 8.0f + (float) i * 3.0f, face.getCentreY() - 5.0f, 1.2f, 10.0f);

        static constexpr const char* names[] { "EQ MATCH", "DYNAMICS", "ANALOG HEAT", "PHASEQ", "GRAPHIC EQ", "LIMITER" };
        g.setColour ((down ? Colours::accent : Colours::text).withAlpha (off ? 0.4f : 1.0f));
        g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
        g.drawText (names[page], face.withTrimmedLeft (16.0f), juce::Justification::centred);
    }

    Order order = MatchEngine::defaultOrder();
    Bypassed bypassed {};
    std::array<float, MatchEngine::numStages> position {};   // current x of each page's tab
    int selected = 0, pressed = -1, dragged = -1;
    float grabOffset = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StageTabs)
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
        const bool lit = state == State::on || (state == State::blinking && blinkOn);
        UlMaToLookAndFeel::drawLed (g, getLocalBounds().toFloat().withSizeKeepingCentre (16.0f, 5.0f), lit);
    }

private:
    void timerCallback() override  { blinkOn = ! blinkOn; repaint(); }

    State state = State::off;
    bool blinkOn = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Led)
};
