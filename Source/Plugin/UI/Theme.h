// openbfd visual language — instrument-panel UI (reference regime).
// Structure follows the instrument-ui design system used by univo: ground /
// sheet / hairlines, square corners, one accent (#F0531C) for on · selected ·
// working · problem, JetBrains Mono everywhere, uppercase instrument labels,
// "+" registration marks, hatching, dot fields, quiet exponential motion.
// Colours live in the Palette; components never hard-code Colour(0x...).
#pragma once

#include <JuceHeader.h>

namespace openbfd::ui {

struct Palette {
    juce::Colour canvas;     // ground
    juce::Colour panel;      // sheet
    juce::Colour panelInset; // inset: selected rows, pressed controls
    juce::Colour rule;       // line-soft: cell / row separators, hatching
    juce::Colour ruleStrong; // line: frames, control borders, marks
    juce::Colour text, textDim, textFaint;
    juce::Colour accent, accentSoft, accentText;
    juce::Colour dotField;
    juce::Colour cable;      // mic cables lying on the stage floor
    bool dark = false;
};

enum class ThemeMode { light, dark };

class Theme {
public:
    static Theme& get();

    const Palette& palette() const { return current; }
    ThemeMode getMode() const { return mode; }
    void setMode(ThemeMode m);

    // JetBrains Mono (embedded). `medium` selects the 500 cut.
    juce::Font mono(float size, bool medium = false) const;

private:
    Theme();
    void apply();
    ThemeMode mode = ThemeMode::light;
    Palette current;
    juce::Typeface::Ptr regular, medium;
};

inline const Palette& pal() { return Theme::get().palette(); }

class LookAndFeel final : public juce::LookAndFeel_V4 {
public:
    LookAndFeel();
    void refreshColours();

    juce::Typeface::Ptr getTypefaceForFont(const juce::Font&) override;
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&, bool, bool) override;
    void drawButtonText(juce::Graphics&, juce::TextButton&, bool, bool) override;
    juce::Font getTextButtonFont(juce::TextButton&, int) override;
    void drawLinearSlider(juce::Graphics&, int, int, int, int, float, float, float,
                          juce::Slider::SliderStyle, juce::Slider&) override;
    void drawRotarySlider(juce::Graphics&, int, int, int, int, float, float, float, juce::Slider&) override;
    void fillTextEditorBackground(juce::Graphics&, int, int, juce::TextEditor&) override;
    void drawTextEditorOutline(juce::Graphics&, int, int, juce::TextEditor&) override;
    void drawScrollbar(juce::Graphics&, juce::ScrollBar&, int, int, int, int, bool, int, int, bool, bool) override;
    int getDefaultScrollbarWidth() override { return 8; }
    void drawPopupMenuBackground(juce::Graphics&, int, int) override;
    void drawPopupMenuItem(juce::Graphics&, const juce::Rectangle<int>&, bool, bool, bool, bool, bool,
                           const juce::String&, const juce::String&, const juce::Drawable*, const juce::Colour*) override;
    juce::Font getPopupMenuFont() override;
    void drawTooltip(juce::Graphics&, const juce::String&, int, int) override;
    juce::Rectangle<int> getTooltipBounds(const juce::String&, juce::Point<int>, juce::Rectangle<int>) override;
};

// Drawing helpers shared by panels.
namespace draw {
void bracketTitle(juce::Graphics&, juce::Rectangle<int>, const juce::String&, float size = 14.0f);
void caption(juce::Graphics&, juce::Rectangle<int>, const juce::String&,
             juce::Justification = juce::Justification::centredLeft, float size = 10.5f);
// "+" registration marks centred on the rectangle's corners (9 px, faint).
void cornerMarks(juce::Graphics&, juce::Rectangle<float>, bool allFour = true);
void hatch(juce::Graphics&, juce::Rectangle<int>, juce::Colour, int spacing = 6);
void dotField(juce::Graphics&, juce::Rectangle<int>, int spacing = 8);
void panel(juce::Graphics&, juce::Rectangle<int>);
// Outlined status chip: 6x6 square dot + caption. `working` = accent outline,
// `problem` = accent fill.
void statusChip(juce::Graphics&, juce::Rectangle<int>, const juce::String&, bool problem, bool working);
// 1 px outlined tag (9.5 px caps).
void tag(juce::Graphics&, juce::Rectangle<int>, const juce::String&, juce::Colour);
// Dashed outline of a circle / rectangle.
void dashedEllipse(juce::Graphics&, juce::Rectangle<float>, float thickness, float dash, float gap);
void dashedRect(juce::Graphics&, juce::Rectangle<float>, float dash, float gap);
// Reticle corner brackets around a target (reference "target lock").
void reticle(juce::Graphics&, juce::Rectangle<float>, float legFraction = 0.18f);
} // namespace draw

} // namespace openbfd::ui
