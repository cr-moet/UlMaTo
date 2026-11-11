#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace Colours
{
    const juce::Colour background  { 0xffeae7de };
    const juce::Colour panel       { 0xfff3f1ea };
    const juce::Colour text        { 0xff292929 };
    const juce::Colour accent      { 0xffd52925 };
    const juce::Colour correlation { 0xffff5f1f };
    const juce::Colour display     { 0xff979797 };   // background of all graphic displays
    const juce::Colour grid        = text.withAlpha (0.16f);
}

//==============================================================================
// Hardware look:
//  - buttons (as in DevTest): flat face with a darker lower edge and a light upper edge,
//    they sink in when pressed; toggle buttons stay sunk while on and show a small LED
//    window (property "led") that lights up in the accent colour,
//  - rotary knobs: moulded black knob (flange on large / medium, finely knurled rim,
//    stippled top, groove with red insert as pointer) and a fine
//    scale of tick marks around it,
//  - value boxes: small LED displays; editing starts with an empty field.
//==============================================================================
class UlMaToLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    UlMaToLookAndFeel()
    {
        using namespace Colours;
        setColour (juce::ResizableWindow::backgroundColourId, background);
        setColour (juce::Label::textColourId, text);
        setColour (juce::TextButton::buttonColourId, buttonFace());
        setColour (juce::TextButton::buttonOnColourId, buttonFace());
        setColour (juce::TextButton::textColourOffId, text);
        setColour (juce::TextButton::textColourOnId, text);
        setColour (juce::Slider::textBoxTextColourId, accent);
        setColour (juce::Slider::textBoxBackgroundColourId, text);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxHighlightColourId, accent.withAlpha (0.4f));
        setColour (juce::Label::textWhenEditingColourId, accent);
        setColour (juce::Label::backgroundWhenEditingColourId, text);
        setColour (juce::Label::outlineWhenEditingColourId, accent);
        setColour (juce::TextEditor::textColourId, accent);
        setColour (juce::TextEditor::backgroundColourId, text);
        setColour (juce::TextEditor::highlightColourId, accent.withAlpha (0.35f));
        setColour (juce::TextEditor::highlightedTextColourId, background);
        setColour (juce::TextEditor::focusedOutlineColourId, accent);
        setColour (juce::CaretComponent::caretColourId, accent);
        setColour (juce::ComboBox::backgroundColourId, buttonFace());
        setColour (juce::ComboBox::textColourId, text);
        setColour (juce::ComboBox::outlineColourId, text.withAlpha (0.6f));
        setColour (juce::ComboBox::arrowColourId, text);
        setColour (juce::PopupMenu::backgroundColourId, background);
        setColour (juce::PopupMenu::textColourId, text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, accent);
        setColour (juce::PopupMenu::highlightedTextColourId, background);
    }

    //==========================================================================
    // Buttons and LEDs (shared with the tabs and the status LED)
    static juce::Colour buttonFace()   { return Colours::background.brighter (0.06f); }
    static constexpr float buttonDepth = 3.0f;

    static juce::Rectangle<float> buttonFaceArea (juce::Rectangle<float> bounds, bool down)
    {
        auto face = bounds.reduced (1.0f);
        return down ? face.withTrimmedTop (buttonDepth - 1.0f).withTrimmedBottom (1.0f)
                    : face.withTrimmedBottom (buttonDepth);
    }

    static void drawHardwareButton (juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour face, bool highlighted, bool down)
    {
        g.setColour (face.darker (0.45f));                    // body: the darker lower edge
        g.fillRoundedRectangle (bounds, 2.5f);

        const auto faceArea = buttonFaceArea (bounds, down);
        g.setColour (highlighted && ! down ? face.brighter (0.05f) : face);
        g.fillRoundedRectangle (faceArea, 2.0f);

        if (! down)                                           // light upper edge
        {
            g.setColour (face.brighter (0.5f).withAlpha (0.8f));
            g.drawHorizontalLine (juce::roundToInt (faceArea.getY()), faceArea.getX() + 2.0f, faceArea.getRight() - 2.0f);
        }

        g.setColour (Colours::text.withAlpha (0.55f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 2.5f, 1.0f);
    }

    static void drawLed (juce::Graphics& g, juce::Rectangle<float> area, bool on)
    {
        if (on)
        {
            g.setColour (Colours::accent.withAlpha (0.25f));
            g.fillRoundedRectangle (area.expanded (2.0f), 2.0f);   // glow
        }
        g.setColour (on ? Colours::accent : Colours::text.withAlpha (0.22f));
        g.fillRoundedRectangle (area, 1.0f);
        g.setColour (Colours::text.withAlpha (0.5f));
        g.drawRoundedRectangle (area, 1.0f, 0.6f);
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& colour, bool highlighted, bool down) override
    {
        const bool led = button.getProperties()["led"];
        const bool sunk = down || (led && button.getToggleState());
        drawHardwareButton (g, button.getLocalBounds().toFloat(), colour, highlighted, sunk);

        if (led)
        {
            // "ledInverted": the LED shows the opposite (BYPASS: lit while the plugin is active)
            const bool lit = button.getToggleState() != (bool) button.getProperties()["ledInverted"];
            const auto face = buttonFaceArea (button.getLocalBounds().toFloat(), sunk);
            drawLed (g, juce::Rectangle<float> (16.0f, 4.0f).withCentre ({ face.getCentreX(), face.getY() + 6.0f }), lit);
        }
    }

    void drawButtonText (juce::Graphics& g, juce::TextButton& button, bool, bool down) override
    {
        const bool led = button.getProperties()["led"];
        const bool sunk = down || (led && button.getToggleState());
        auto face = buttonFaceArea (button.getLocalBounds().toFloat(), sunk);
        if (led)
            face.removeFromTop (8.0f);   // text below the LED

        g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
        g.setColour (button.findColour (button.getToggleState() ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId)
                         .withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.5f));
        g.drawText (button.getButtonText(), face, juce::Justification::centred);
    }

    //==========================================================================
    void drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box) override
    {
        const auto bounds = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height);
        drawHardwareButton (g, bounds, box.findColour (juce::ComboBox::backgroundColourId), false, false);

        juce::Path arrow;
        const float cx = (float) width - 14.0f, cy = (float) height * 0.5f - 1.0f;
        arrow.addTriangle (cx - 4.0f, cy - 2.0f, cx + 4.0f, cy - 2.0f, cx, cy + 3.0f);
        g.setColour (box.findColour (juce::ComboBox::arrowColourId));
        g.fillPath (arrow);
    }

    juce::Font getComboBoxFont (juce::ComboBox&) override   { return juce::Font (juce::FontOptions (13.0f, juce::Font::bold)); }

    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds (8, 0, box.getWidth() - 28, box.getHeight() - (int) buttonDepth);
        label.setFont (getComboBoxFont (box));
    }

    //==========================================================================
    // Rotary knob: fine tick scale, shadow, flange with notch (large / medium), finely
    // knurled rim, stippled top and a groove with the red insert; the whole knob turns
    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float pos,
                           float startAngle, float endAngle, juce::Slider& slider) override
    {
        using juce::MathConstants;
        const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (4.0f);
        const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto  centre = bounds.getCentre();
        const float angle  = startAngle + pos * (endAngle - startAngle);
        const float alpha  = slider.isEnabled() ? 1.0f : 0.45f;
        const auto  dirOf  = [] (float a) { return juce::Point<float> (std::sin (a), -std::cos (a)); };
        const auto  dir    = dirOf (angle);
        const auto  circle = [centre] (float r) { return juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (centre); };

        // Milled cut (groove, notch): a floor a little darker than the material, the wall
        // facing the light casts a shadow onto the floor, the opposite wall catches the light
        const auto shadeCut = [&g, alpha] (const juce::Path& cut, float depth)
        {
            juce::Graphics::ScopedSaveState state (g);
            g.reduceClipRegion (cut);
            const auto area = cut.getBounds().expanded (depth * 4.0f);
            const auto rim = [&cut, &area] (float dx, float dy)
            {
                juce::Path p;
                p.addRectangle (area);
                p.addPath (cut, juce::AffineTransform::translation (dx, dy));
                p.setUsingNonZeroWinding (false);
                return p;
            };
            g.setColour (juce::Colours::black.withAlpha (0.4f * alpha));    // shadow below the upper left wall
            g.fillPath (rim (depth, depth));
            g.setColour (juce::Colours::black.withAlpha (0.3f * alpha));
            g.fillPath (rim (depth * 0.45f, depth * 0.45f));
            g.setColour (juce::Colours::white.withAlpha (0.3f * alpha));    // lit lower right wall
            g.fillPath (rim (-0.9f, -0.9f));
        };

        // Scale: 41 ticks, every fifth longer
        g.setColour (Colours::text.withAlpha (0.75f * alpha));
        for (int i = 0; i <= 40; ++i)
        {
            const float a = startAngle + (float) i / 40.0f * (endAngle - startAngle);
            const bool major = i % 5 == 0;
            g.drawLine ({ centre + dirOf (a) * (radius * (major ? 0.80f : 0.87f)), centre + dirOf (a) * radius }, major ? 1.4f : 0.9f);
        }

        // Three sizes as the hardware: large and medium with a flange, small without
        const float diameter = radius * 2.0f;
        const bool  flange   = diameter >= 56.0f;
        const float capR     = radius * (flange ? (diameter >= 90.0f ? 0.60f : 0.56f) : 0.74f);
        const float flangeR  = radius * 0.77f;
        const auto  black    = Colours::text;   // the knob is moulded in the panel black

        // Drop shadow (light from the top left)
        for (int i = 0; i < 4; ++i)
        {
            g.setColour (juce::Colours::black.withAlpha (0.07f * alpha));
            g.fillEllipse (circle ((flange ? flangeR : capR) + 1.0f + (float) i).translated (radius * 0.04f, radius * 0.07f));
        }

        // Flange: flat glossy disc with a notch at the pointer
        if (flange)
        {
            juce::Path disc;
            disc.addEllipse (circle (flangeR));
            juce::Path notch;
            const float notchW = flangeR * 0.07f;
            notch.addRoundedRectangle (-notchW, -flangeR - 1.0f, notchW * 2.0f, (flangeR - capR) * 0.55f + 1.0f, notchW);
            notch.applyTransform (juce::AffineTransform::rotation (angle).translated (centre));
            g.setGradientFill (juce::ColourGradient (black.brighter (0.35f).withMultipliedAlpha (alpha), circle (flangeR).getTopLeft(),
                                                     black.darker (0.6f).withMultipliedAlpha (alpha), circle (flangeR).getBottomRight(), false));
            g.fillPath (disc);
            g.setColour (juce::Colours::white.withAlpha (0.10f * alpha));   // soft gloss ring
            g.drawEllipse (circle (flangeR * 0.93f), flangeR * 0.05f);
            g.setColour (juce::Colours::black.withAlpha (0.6f * alpha));
            g.strokePath (disc, juce::PathStrokeType (0.8f));
            // the notch: milled into the flange, only inside its edge
            juce::Path flangeDisc;
            flangeDisc.addEllipse (circle (flangeR - 0.5f));
            {
                juce::Graphics::ScopedSaveState state (g);
                g.reduceClipRegion (flangeDisc);
                g.setColour (black.brighter (0.12f).withMultipliedAlpha (alpha));
                g.fillPath (notch);
                shadeCut (notch, juce::jmax (1.2f, notchW * 0.6f));
            }
        }

        // Knurled rim: many fine ridges around the cap, lit from the top left; they turn with the knob
        const int   teeth  = juce::jlimit (28, 72, juce::roundToInt (capR * 2.2f));
        const float inner  = capR * 0.86f;
        juce::Path serrated;
        for (int i = 0; i < teeth * 2; ++i)
        {
            const float a = angle + MathConstants<float>::pi * (float) i / (float) teeth;
            const auto pt = centre + dirOf (a) * (i % 2 == 0 ? capR : capR - juce::jmax (0.8f, capR * 0.035f));
            if (i == 0) serrated.startNewSubPath (pt); else serrated.lineTo (pt);
        }
        serrated.closeSubPath();
        g.setColour (black.darker (0.5f).withMultipliedAlpha (alpha));
        g.fillPath (serrated);
        for (int i = 0; i < teeth; ++i)
        {
            const float a0 = angle + MathConstants<float>::twoPi * (float) i / (float) teeth, a1 = a0 + MathConstants<float>::pi / (float) teeth;
            juce::Path ridge;
            ridge.startNewSubPath (centre + dirOf (a0) * capR);
            ridge.lineTo (centre + dirOf (a1) * capR);
            ridge.lineTo (centre + dirOf (a1) * inner);
            ridge.lineTo (centre + dirOf (a0) * inner);
            ridge.closeSubPath();
            const float light = 0.5f + 0.5f * std::cos (a0 + MathConstants<float>::pi * 0.25f);
            g.setColour (black.interpolatedWith (juce::Colour (0xff9a9a9a), 0.55f * light).withMultipliedAlpha (alpha));
            g.fillPath (ridge);
        }

        // Cap: stippled leather-like top in the panel black, turning with the knob
        const auto cap = circle (inner);
        g.setGradientFill (juce::ColourGradient (black.brighter (0.2f).withMultipliedAlpha (alpha), cap.getTopLeft(),
                                                 black.darker (0.5f).withMultipliedAlpha (alpha), cap.getBottomRight(), false));
        g.fillEllipse (cap);
        {
            juce::Graphics::ScopedSaveState state (g);
            juce::Path clip;
            clip.addEllipse (cap);
            g.reduceClipRegion (clip);
            const auto& tex = stippleTexture();
            g.setOpacity (alpha);
            g.drawImageTransformed (tex, juce::AffineTransform::translation (-0.5f * (float) tex.getWidth(), -0.5f * (float) tex.getHeight())
                                             .scaled (cap.getWidth() / (float) tex.getWidth() * 1.45f)
                                             .rotated (angle).translated (centre));
        }
        g.setColour (juce::Colours::white.withAlpha (0.18f * alpha));   // edge of the top: light top left, dark bottom right
        juce::Path rim;
        rim.addCentredArc (centre.x, centre.y, inner, inner, 0.0f, -2.6f, 0.6f, true);
        g.strokePath (rim, juce::PathStrokeType (1.0f));
        g.setColour (juce::Colours::black.withAlpha (0.6f * alpha));
        juce::Path rimDark;
        rimDark.addCentredArc (centre.x, centre.y, inner, inner, 0.0f, 0.6f, 3.68f, true);
        g.strokePath (rimDark, juce::PathStrokeType (1.0f));

        // Pointer: a wide milled groove from the front edge of the knurled rim inwards, with a
        // rounded end and the red insert; groove and insert end flush at the rim
        const float grooveW  = juce::jmax (4.5f, inner * 0.30f);
        const float grooveIn = inner * 0.30f;   // distance of the rounded end from the centre
        juce::Path groove;
        groove.addRoundedRectangle (-grooveW * 0.5f, -capR - 2.0f, grooveW, capR + 2.0f - grooveIn + grooveW * 0.5f, grooveW * 0.5f);
        groove.applyTransform (juce::AffineTransform::rotation (angle).translated (centre));
        {
            juce::Graphics::ScopedSaveState state (g);
            juce::Path clip;
            clip.addEllipse (circle (capR));
            g.reduceClipRegion (clip);
            g.setGradientFill (juce::ColourGradient (black.brighter (0.3f).withMultipliedAlpha (alpha), circle (capR).getTopLeft(),
                                                     black.darker (0.15f).withMultipliedAlpha (alpha), circle (capR).getBottomRight(), false));
            g.fillPath (groove);   // floor of the groove: the same material, a little deeper
            const auto side = juce::Point<float> (-dir.y, dir.x);
            const float insertW = juce::jmax (1.6f, grooveW * 0.42f);
            const auto from = centre + dir * grooveIn, to = centre + dir * (capR + insertW);   // the clip cuts it flush with the rim
            juce::Path insert;
            insert.startNewSubPath (from);
            insert.lineTo (to);
            g.setColour (Colours::accent.withAlpha (alpha));
            g.strokePath (insert, juce::PathStrokeType (insertW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (Colours::accent.brighter (0.4f).withAlpha (0.6f * alpha));   // highlight on the insert
            g.drawLine ({ from - side * (insertW * 0.2f), to - side * (insertW * 0.2f) }, insertW * 0.3f);
            {   // over the round edge the groove and the insert bend down the side: darker towards the
                // outside, a bright bend line where the top turns into the side
                juce::Graphics::ScopedSaveState bend (g);
                g.reduceClipRegion (groove);
                juce::Path rimBand;
                rimBand.addEllipse (circle (capR + 1.0f));
                rimBand.addEllipse (circle (inner));
                rimBand.setUsingNonZeroWinding (false);
                g.reduceClipRegion (rimBand);
                g.setGradientFill (juce::ColourGradient (juce::Colours::transparentBlack, centre + dir * inner,
                                                         juce::Colours::black.withAlpha (0.6f * alpha), centre + dir * capR, false));
                g.fillRect (circle (capR + 1.0f));
                g.setColour (Colours::accent.brighter (0.8f).withAlpha (0.7f * alpha));
                g.drawLine ({ centre + dir * (inner + 0.6f) - side * (insertW * 0.5f), centre + dir * (inner + 0.6f) + side * (insertW * 0.5f) }, 1.0f);
                g.setColour (juce::Colours::white.withAlpha (0.25f * alpha));   // the bend on the groove floor
                g.drawLine ({ centre + dir * (inner + 0.6f) - side * (grooveW * 0.5f), centre + dir * (inner + 0.6f) + side * (grooveW * 0.5f) }, 0.8f);
            }
            shadeCut (groove, juce::jmax (1.5f, grooveW * 0.22f));   // walls over the floor and the insert
        }
    }

    // Stipple texture of the knob tops (made once): small light and dark grains
    static const juce::Image& stippleTexture()
    {
        static const juce::Image texture = []
        {
            juce::Image img (juce::Image::ARGB, 160, 160, true);
            juce::Graphics tg (img);
            juce::Random random (0x909);
            for (int i = 0; i < 3000; ++i)
            {
                const float px = random.nextFloat() * 160.0f, py = random.nextFloat() * 160.0f;
                const float w = 0.8f + random.nextFloat() * 2.4f, h = 0.8f + random.nextFloat() * 1.6f;
                tg.setColour (random.nextInt (3) == 0 ? juce::Colours::white.withAlpha (0.025f + 0.03f * random.nextFloat())
                                                      : juce::Colours::black.withAlpha (0.15f + 0.2f * random.nextFloat()));
                tg.fillEllipse (px, py, w, h);
            }
            return img;
        }();
        return texture;
    }

    //==========================================================================
    // Value box: LED display; when it is edited the field starts empty, and an empty
    // entry changes nothing
    class ValueBox final : public juce::Label
    {
    public:
        // Set into the panel like a small screen: inner shadow at the top and left, dark
        // upper edge, light lower edge and a faint glass reflection
        void paint (juce::Graphics& g) override
        {
            juce::Label::paint (g);
            const auto r = getLocalBounds().toFloat();
            const auto shade = [&g] (juce::Rectangle<float> band, juce::Point<float> from, juce::Point<float> to, float alpha)
            {
                g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (alpha), from, juce::Colours::transparentBlack, to, false));
                g.fillRect (band);
            };
            shade (r.withHeight (5.0f), r.getTopLeft(), r.getTopLeft().translated (0.0f, 5.0f), 0.75f);
            shade (r.withWidth (4.0f), r.getTopLeft(), r.getTopLeft().translated (4.0f, 0.0f), 0.5f);
            g.setColour (juce::Colours::white.withAlpha (0.04f));
            g.fillRect (r.withTrimmedTop (r.getHeight() * 0.2f).withHeight (r.getHeight() * 0.3f).reduced (3.0f, 0.0f));
            g.setColour (juce::Colours::black);
            g.drawLine (r.getX(), r.getY() + 0.5f, r.getRight(), r.getY() + 0.5f, 1.0f);
            g.drawLine (r.getX() + 0.5f, r.getY(), r.getX() + 0.5f, r.getBottom(), 1.0f);
            g.setColour (juce::Colours::white.withAlpha (0.55f));
            g.drawLine (r.getX(), r.getBottom() - 0.5f, r.getRight(), r.getBottom() - 0.5f, 1.0f);
            g.drawLine (r.getRight() - 0.5f, r.getY(), r.getRight() - 0.5f, r.getBottom(), 1.0f);
        }

        void editorShown (juce::TextEditor* textEditor) override
        {
            juce::Label::editorShown (textEditor);
            textEditor->setText ({}, juce::dontSendNotification);
        }

        void editorAboutToBeHidden (juce::TextEditor* textEditor) override
        {
            if (textEditor->getText().trim().isEmpty())
                textEditor->setText (getText(), juce::dontSendNotification);
            juce::Label::editorAboutToBeHidden (textEditor);
        }
    };

    juce::Label* createSliderTextBox (juce::Slider& slider) override
    {
        auto* label = new ValueBox();
        label->setJustificationType (juce::Justification::centred);
        label->setKeyboardType (juce::TextInputTarget::decimalKeyboard);
        label->setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 15.0f, juce::Font::bold)));
        label->setColour (juce::Label::textColourId, slider.findColour (juce::Slider::textBoxTextColourId));
        label->setColour (juce::Label::backgroundColourId, slider.findColour (juce::Slider::textBoxBackgroundColourId));
        label->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
        label->setColour (juce::TextEditor::textColourId, Colours::accent);
        label->setColour (juce::TextEditor::backgroundColourId, Colours::text);
        label->setColour (juce::TextEditor::highlightColourId, Colours::accent.withAlpha (0.35f));
        return label;
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UlMaToLookAndFeel)
};
