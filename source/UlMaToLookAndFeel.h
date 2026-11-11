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
//  - rotary knobs: knurled polygonal skirt, flat brushed top, red pointer line and a fine
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
    // Rotary knob: fine tick scale, shadow, knurled polygonal skirt, flat brushed top with
    // a hard edge, red pointer line
    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float pos,
                           float startAngle, float endAngle, juce::Slider& slider) override
    {
        const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (4.0f);
        const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto  centre = bounds.getCentre();
        const float angle  = startAngle + pos * (endAngle - startAngle);
        const float alpha  = slider.isEnabled() ? 1.0f : 0.45f;

        // Scale: 41 ticks, every fifth longer
        g.setColour (Colours::text.withAlpha (0.75f * alpha));
        for (int i = 0; i <= 40; ++i)
        {
            const float a = startAngle + (float) i / 40.0f * (endAngle - startAngle);
            const auto dir = juce::Point<float> (std::sin (a), -std::cos (a));
            const bool major = i % 5 == 0;
            g.drawLine ({ centre + dir * (radius * (major ? 0.80f : 0.87f)), centre + dir * radius }, major ? 1.4f : 0.9f);
        }

        // Polygon around the centre (corners: number of edges)
        const auto polygon = [centre] (float r, int corners, float rotation, juce::Point<float> offset = {})
        {
            juce::Path p;
            for (int i = 0; i < corners; ++i)
            {
                const float a = rotation + juce::MathConstants<float>::twoPi * (float) i / (float) corners;
                const juce::Point<float> pt = centre + offset + juce::Point<float> (std::sin (a), -std::cos (a)) * r;
                if (i == 0) p.startNewSubPath (pt); else p.lineTo (pt);
            }
            p.closeSubPath();
            return p;
        };

        // Shadow and skirt: 16 edges with knurled ridges (light / dark facets, dark notches)
        const float skirt = radius * 0.70f;
        constexpr int edges = 16;
        g.setColour (juce::Colours::black.withAlpha (0.25f * alpha));
        g.fillPath (polygon (skirt + 1.0f, edges, angle, { skirt * 0.05f, skirt * 0.12f }));
        g.setColour (juce::Colour (0xff2a2a2a));
        g.fillPath (polygon (skirt, edges, angle));
        for (int i = 0; i < edges; ++i)   // the whole knob turns with the value: the facets move, the light stays
        {
            const float a0 = angle + juce::MathConstants<float>::twoPi * (float) i / (float) edges, a1 = a0 + juce::MathConstants<float>::twoPi / (float) edges;
            const auto p0 = centre + juce::Point<float> (std::sin (a0), -std::cos (a0)) * skirt;
            const auto p1 = centre + juce::Point<float> (std::sin (a1), -std::cos (a1)) * skirt;
            const auto q0 = centre + juce::Point<float> (std::sin (a0), -std::cos (a0)) * (skirt * 0.78f);
            const auto q1 = centre + juce::Point<float> (std::sin (a1), -std::cos (a1)) * (skirt * 0.78f);
            juce::Path facet;
            facet.startNewSubPath (p0); facet.lineTo (p1); facet.lineTo (q1); facet.lineTo (q0); facet.closeSubPath();
            const float light = 0.5f + 0.5f * std::cos (0.5f * (a0 + a1) + juce::MathConstants<float>::pi * 0.25f);   // lit from the top left
            g.setColour (juce::Colour (0xff333333).interpolatedWith (juce::Colour (0xff8c8c8c), (i % 2 == 0 ? 0.85f : 0.35f) * light));
            g.fillPath (facet);
            g.setColour (juce::Colours::black.withAlpha (0.7f));   // notch between the facets
            g.drawLine ({ p0, q0 }, 1.2f);
        }
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.strokePath (polygon (skirt, edges, angle), juce::PathStrokeType (1.0f));

        // Flat top with a hard edge and faint brushed rings
        const float topRadius = skirt * 0.76f;
        const auto top = juce::Rectangle<float> (topRadius * 2.0f, topRadius * 2.0f).withCentre (centre);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff767676), top.getTopLeft(), juce::Colour (0xff585858), top.getBottomRight(), false));
        g.fillEllipse (top);
        for (float r = topRadius - 1.5f; r > 2.0f; r -= 2.0f)
        {
            g.setColour ((((int) r) % 2 == 0 ? juce::Colours::white : juce::Colours::black).withAlpha (0.05f));
            g.drawEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (centre), 1.0f);
        }
        g.setColour (juce::Colours::white.withAlpha (0.35f));   // hard edge: light top left, dark bottom right
        juce::Path rim;
        rim.addCentredArc (centre.x, centre.y, topRadius, topRadius, 0.0f, -2.6f, 0.6f, true);
        g.strokePath (rim, juce::PathStrokeType (1.2f));
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        juce::Path rimDark;
        rimDark.addCentredArc (centre.x, centre.y, topRadius, topRadius, 0.0f, 0.6f, 3.68f, true);
        g.strokePath (rimDark, juce::PathStrokeType (1.2f));

        // Pointer: red line from near the centre to the edge of the top
        const auto dir = juce::Point<float> (std::sin (angle), -std::cos (angle));
        g.setColour (Colours::accent.withAlpha (alpha));
        g.drawLine ({ centre + dir * (topRadius * 0.30f), centre + dir * (topRadius * 0.97f) }, juce::jmax (2.0f, topRadius * 0.09f));
    }

    //==========================================================================
    // Value box: LED display; when it is edited the field starts empty, and an empty
    // entry changes nothing
    class ValueBox final : public juce::Label
    {
    public:
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
