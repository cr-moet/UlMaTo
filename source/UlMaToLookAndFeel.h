#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace Colours
{
    const juce::Colour background { 0xffeae7de };
    const juce::Colour panel      { 0xfff3f1ea };
    const juce::Colour text       { 0xff292929 };
    const juce::Colour accent     { 0xffd52925 };
    const juce::Colour correlation { 0xffff5f1f };
    const juce::Colour grid       = text.withAlpha (0.12f);
}

//==============================================================================
// Hardware-style look: flat buttons with a hint of depth, a large rotary knob with
// a dark cap and an accent arc
//==============================================================================
class UlMaToLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    UlMaToLookAndFeel()
    {
        using namespace Colours;
        setColour (juce::ResizableWindow::backgroundColourId, background);
        setColour (juce::Label::textColourId, text);
        setColour (juce::TextButton::buttonColourId, panel);
        setColour (juce::TextButton::textColourOffId, text);
        setColour (juce::TextButton::textColourOnId, text);
        setColour (juce::Slider::textBoxTextColourId, accent);
        setColour (juce::Slider::textBoxBackgroundColourId, text);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxHighlightColourId, accent.withAlpha (0.4f));
        setColour (juce::TextEditor::textColourId, background);
        setColour (juce::CaretComponent::caretColourId, accent);
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& colour,
                               bool highlighted, bool down) override
    {
        auto r = button.getLocalBounds().toFloat().reduced (0.5f);
        const auto face = colour.brighter (highlighted && ! down ? 0.06f : 0.0f).darker (down ? 0.08f : 0.0f);

        g.setColour (Colours::text.withAlpha (0.35f));                     // shadow edge
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (face);
        g.fillRoundedRectangle (r.withTrimmedBottom (down ? 1.0f : 3.0f).withTrimmedTop (down ? 2.0f : 0.0f), 3.0f);
        g.setColour (Colours::text.withAlpha (0.5f));
        g.drawRoundedRectangle (r, 3.0f, 1.0f);
    }

    juce::Font getTextButtonFont (juce::TextButton&, int) override
    {
        return juce::Font (juce::FontOptions (13.0f, juce::Font::bold));
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float pos,
                           float startAngle, float endAngle, juce::Slider&) override
    {
        const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (6.0f);
        const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto  centre = bounds.getCentre();
        const float angle  = startAngle + pos * (endAngle - startAngle);

        // Scale: tick marks every 10 %
        g.setColour (Colours::text.withAlpha (0.6f));
        for (int i = 0; i <= 10; ++i)
        {
            const float a = startAngle + (float) i / 10.0f * (endAngle - startAngle);
            const auto dir = juce::Point<float> (std::sin (a), -std::cos (a));
            g.drawLine ({ centre + dir * (radius - 2.0f), centre + dir * (radius - (i % 5 == 0 ? 11.0f : 7.0f)) },
                        i % 5 == 0 ? 1.6f : 1.0f);
        }

        // Value arc
        const float arcRadius = radius - 16.0f;
        juce::Path track, value;
        track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
        value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, angle, true);
        g.setColour (Colours::text.withAlpha (0.15f));
        g.strokePath (track, juce::PathStrokeType (5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (Colours::accent);
        g.strokePath (value, juce::PathStrokeType (5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Cap with pointer
        const float capRadius = arcRadius - 10.0f;
        const auto cap = juce::Rectangle<float> (capRadius * 2.0f, capRadius * 2.0f).withCentre (centre);
        g.setColour (Colours::text.withAlpha (0.3f));
        g.fillEllipse (cap.translated (0.0f, 3.0f));
        g.setGradientFill (juce::ColourGradient (Colours::text.brighter (0.35f), cap.getTopLeft(),
                                                 Colours::text.darker (0.4f), cap.getBottomRight(), false));
        g.fillEllipse (cap);

        const auto dir = juce::Point<float> (std::sin (angle), -std::cos (angle));
        g.setColour (Colours::accent);
        g.drawLine ({ centre + dir * (capRadius * 0.25f), centre + dir * (capRadius * 0.9f) }, 3.0f);
    }

    juce::Label* createSliderTextBox (juce::Slider& slider) override
    {
        auto* label = juce::LookAndFeel_V4::createSliderTextBox (slider);
        label->setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 15.0f, juce::Font::bold)));
        label->setJustificationType (juce::Justification::centred);
        return label;
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UlMaToLookAndFeel)
};
