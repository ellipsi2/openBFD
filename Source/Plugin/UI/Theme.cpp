#include "Theme.h"

#include "Motion.h"
#include "OpenBfdFonts.h"

namespace openbfd::ui {

Theme& Theme::get() {
    static Theme t;
    return t;
}

Theme::Theme() {
    regular = juce::Typeface::createSystemTypefaceFor(OpenBfdFonts::JetBrainsMonoRegular_ttf,
                                                      OpenBfdFonts::JetBrainsMonoRegular_ttfSize);
    medium = juce::Typeface::createSystemTypefaceFor(OpenBfdFonts::JetBrainsMonoMedium_ttf,
                                                     OpenBfdFonts::JetBrainsMonoMedium_ttfSize);
    apply();
}

void Theme::setMode(ThemeMode m) {
    if (mode == m) return;
    mode = m;
    apply();
}

void Theme::apply() {
    auto& p = current;
    p.dark = (mode == ThemeMode::dark);
    if (!p.dark) {
        p.canvas = juce::Colour(0xffe7e7e4);
        p.panel = juce::Colour(0xfff7f7f5);
        p.panelInset = juce::Colour(0xffefefec);
        p.rule = juce::Colour(0xffd9d9d4);
        p.ruleStrong = juce::Colour(0xffb7b7b1);
        p.text = juce::Colour(0xff262624);
        p.textDim = juce::Colour(0xff7c7c77);
        p.textFaint = juce::Colour(0xffaaaaa4);
        p.accent = juce::Colour(0xfff0531c);
        p.accentSoft = juce::Colour(0xfffbdccf);
        p.accentText = juce::Colour(0xffffffff);
    } else {
        p.canvas = juce::Colour(0xff0e0f11);
        p.panel = juce::Colour(0xff17181b);
        p.panelInset = juce::Colour(0xff1d1e22);
        p.rule = juce::Colour(0xff2a2c31);
        p.ruleStrong = juce::Colour(0xff42454c);
        p.text = juce::Colour(0xffe3e3df);
        p.textDim = juce::Colour(0xff858991);
        p.textFaint = juce::Colour(0xff53565d);
        p.accent = juce::Colour(0xffff6430);
        p.accentSoft = juce::Colour(0xff4b2416);
        p.accentText = juce::Colour(0xffffffff);
    }
    p.dotField = p.text.withAlpha(p.dark ? 0.16f : 0.13f);
    p.cable = p.text.withAlpha(p.dark ? 0.62f : 0.72f);
}

juce::Font Theme::mono(float size, bool isMedium) const {
    return juce::Font(juce::FontOptions(isMedium ? medium : regular).withHeight(size));
}

//==============================================================================
LookAndFeel::LookAndFeel() { refreshColours(); }

void LookAndFeel::refreshColours() {
    const auto& p = pal();
    setColour(juce::ResizableWindow::backgroundColourId, p.canvas);
    setColour(juce::TextButton::buttonColourId, p.panel);
    setColour(juce::TextButton::buttonOnColourId, p.accent);
    setColour(juce::TextButton::textColourOffId, p.text);
    setColour(juce::TextButton::textColourOnId, p.accentText);
    setColour(juce::Slider::textBoxTextColourId, p.text);
    setColour(juce::Label::textColourId, p.text);
    setColour(juce::TextEditor::backgroundColourId, p.panel);
    setColour(juce::TextEditor::textColourId, p.text);
    setColour(juce::TextEditor::highlightColourId, p.accentSoft);
    setColour(juce::TextEditor::highlightedTextColourId, p.text);
    setColour(juce::TextEditor::outlineColourId, p.ruleStrong);
    setColour(juce::TextEditor::focusedOutlineColourId, p.accent);
    setColour(juce::CaretComponent::caretColourId, p.accent);
    setColour(juce::ScrollBar::thumbColourId, p.ruleStrong);
    setColour(juce::PopupMenu::backgroundColourId, p.panel);
    setColour(juce::PopupMenu::textColourId, p.text);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, p.text);
    setColour(juce::PopupMenu::highlightedTextColourId, p.panel);
    setColour(juce::TooltipWindow::backgroundColourId, p.text);
    setColour(juce::TooltipWindow::textColourId, p.panel);
}

juce::Typeface::Ptr LookAndFeel::getTypefaceForFont(const juce::Font& f) {
    return Theme::get().mono(f.getHeight(), f.isBold()).getTypefacePtr();
}

void LookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& b, const juce::Colour&, bool highlighted,
                                       bool down) {
    const auto& p = pal();
    auto r = b.getLocalBounds().toFloat().reduced(0.5f);
    if (b.isConnectedOnLeft()) r = r.withLeft(r.getX() - 1.0f);
    const float on = motion::follow(b, "on", b.getToggleState() ? 1.0f : 0.0f, motion::quick);
    const float hover = motion::follow(b, "hover", highlighted ? 1.0f : 0.0f, motion::quick);
    auto fill = p.panel.interpolatedWith(p.accent, on).overlaidWith(p.text.withAlpha(down ? 0.10f : 0.05f * hover));
    if (!b.isEnabled()) fill = fill.withMultipliedAlpha(0.5f);
    g.setColour(fill);
    g.fillRect(r);
    // quiet button: on hover only the border turns accent
    g.setColour(p.ruleStrong.interpolatedWith(p.accent, std::max(on, hover)));
    g.drawRect(r, 1.0f);
}

juce::Font LookAndFeel::getTextButtonFont(juce::TextButton&, int h) {
    return Theme::get().mono(std::min(11.5f, float(h) * 0.48f));
}

void LookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& b, bool, bool) {
    const auto& p = pal();
    const float on = motion::current(b, "on", b.getToggleState() ? 1.0f : 0.0f);
    g.setColour(p.text.interpolatedWith(p.accentText, on).withMultipliedAlpha(b.isEnabled() ? 1.0f : 0.45f));
    g.setFont(getTextButtonFont(b, b.getHeight()));
    g.drawFittedText(b.getButtonText().toUpperCase(), b.getLocalBounds().reduced(4, 0), juce::Justification::centred, 1);
}

// Vertical: the channel fader (1 px track, square thumb). Horizontal: univo slider.
void LookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                                   juce::Slider::SliderStyle style, juce::Slider& s) {
    const auto& p = pal();
    const bool hot = s.isMouseOverOrDragging();
    const float hover = motion::follow(s, "hover", hot ? 1.0f : 0.0f, motion::quick);
    if (style == juce::Slider::LinearVertical) {
        const float cx = float(x) + float(w) * 0.5f;
        g.setColour(p.ruleStrong);
        g.fillRect(cx - 0.5f, float(y), 1.0f, float(h));
        const juce::Rectangle<float> thumb(cx - 9.0f, pos - 4.0f, 18.0f, 8.0f);
        g.setColour(p.panel);
        g.fillRect(thumb);
        g.setColour(p.text.interpolatedWith(p.accent, hover));
        g.fillRect(thumb.reduced(1.0f));
        g.setColour(p.panel);
        g.fillRect(thumb.getX() + 3.0f, thumb.getCentreY() - 0.5f, thumb.getWidth() - 6.0f, 1.0f);
        return;
    }
    const float cy = float(y) + float(h) * 0.5f;
    if ((bool) s.getProperties()["fader"]) {
        // horizontal channel fader: 1 px track, square thumb with a grip line
        g.setColour(p.ruleStrong);
        g.fillRect(float(x), cy - 0.5f, float(w), 1.0f);
        const juce::Rectangle<float> thumb(pos - 4.0f, cy - 9.0f, 8.0f, 18.0f);
        g.setColour(p.panel);
        g.fillRect(thumb);
        g.setColour(p.text.interpolatedWith(p.accent, hover));
        g.fillRect(thumb.reduced(1.0f));
        g.setColour(p.panel);
        g.fillRect(thumb.getCentreX() - 0.5f, thumb.getY() + 3.0f, 1.0f, thumb.getHeight() - 6.0f);
        return;
    }
    g.setColour(p.ruleStrong);
    g.fillRect(float(x), cy - 0.5f, float(w), 1.0f);
    g.setColour(p.text);
    g.fillRect(float(x), cy - 1.0f, pos - float(x), 2.0f);
    g.setColour(p.text.interpolatedWith(p.accent, hover));
    g.fillRect(juce::Rectangle<float>(pos - 3.0f, cy - 7.0f, 6.0f, 14.0f));
}

// Knob: 1 px ring, value arc in text (2 px), pointer tick. Accent only while
// the knob is under the hand.
void LookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h, float pos, float a0, float a1,
                                   juce::Slider& s) {
    const auto& p = pal();
    const float hover = motion::follow(s, "hover", s.isMouseOverOrDragging() ? 1.0f : 0.0f, motion::quick);
    const float d = float(std::min(w, h)) - 8.0f; // room for the value arc outside the ring
    const auto c = juce::Rectangle<float>(float(x), float(y), float(w), float(h)).getCentre();
    const auto ring = juce::Rectangle<float>(d, d).withCentre(c);
    g.setColour(p.panel);
    g.fillEllipse(ring);
    g.setColour(p.ruleStrong);
    g.drawEllipse(ring, 1.0f);
    // bipolar knobs (pan, centred defaults) draw their arc from the top
    const bool bipolar = (bool) s.getProperties()["bipolar"];
    const float angle = a0 + pos * (a1 - a0);
    const float from = bipolar ? (a0 + a1) * 0.5f : a0;
    juce::Path arc;
    arc.addCentredArc(c.x, c.y, d * 0.5f + 2.5f, d * 0.5f + 2.5f, 0.0f, std::min(from, angle), std::max(from, angle), true);
    g.setColour(p.text.withAlpha(0.85f));
    g.strokePath(arc, juce::PathStrokeType(1.5f));
    const auto tip = c.getPointOnCircumference(d * 0.5f - 1.0f, angle);
    const auto base = c.getPointOnCircumference(d * 0.12f, angle);
    g.setColour(p.text.interpolatedWith(p.accent, hover));
    g.drawLine({base, tip}, 1.5f);
}

void LookAndFeel::fillTextEditorBackground(juce::Graphics& g, int w, int h, juce::TextEditor&) {
    g.setColour(pal().panel);
    g.fillRect(0, 0, w, h);
}

void LookAndFeel::drawTextEditorOutline(juce::Graphics& g, int w, int h, juce::TextEditor& e) {
    g.setColour(e.hasKeyboardFocus(true) ? pal().accent : pal().ruleStrong);
    g.drawRect(0, 0, w, h, 1);
}

void LookAndFeel::drawScrollbar(juce::Graphics& g, juce::ScrollBar&, int x, int y, int w, int h, bool vertical,
                                int thumbStart, int thumbSize, bool over, bool down) {
    const auto& p = pal();
    juce::Rectangle<int> thumb = vertical ? juce::Rectangle<int>(x + w / 2 - 1, thumbStart, 3, thumbSize)
                                          : juce::Rectangle<int>(thumbStart, y + h / 2 - 1, thumbSize, 3);
    g.setColour(over || down ? p.textDim : p.ruleStrong);
    g.fillRect(thumb);
}

void LookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int w, int h) {
    const auto& p = pal();
    g.fillAll(p.panel);
    g.setColour(p.ruleStrong);
    g.drawRect(0, 0, w, h, 1);
    g.setColour(p.accent);
    g.fillRect(0, 0, w, 2); // menus carry a 2 px accent rule on top
}

void LookAndFeel::drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator,
                                    bool isActive, bool isHighlighted, bool isTicked, bool, const juce::String& text,
                                    const juce::String& shortcut, const juce::Drawable*, const juce::Colour*) {
    const auto& p = pal();
    if (isSeparator) {
        g.setColour(p.rule);
        g.fillRect(area.reduced(8, 0).withHeight(1).withY(area.getCentreY()));
        return;
    }
    auto r = area.reduced(1, 0);
    if (isHighlighted && isActive) {
        g.setColour(p.text);
        g.fillRect(r);
    }
    g.setColour(isHighlighted && isActive ? p.panel : (isActive ? p.text : p.textFaint));
    g.setFont(Theme::get().mono(11.5f));
    if (isTicked) g.fillRect(r.getX() + 8, r.getCentreY() - 3, 6, 6);
    g.drawFittedText(text.toUpperCase(), r.withTrimmedLeft(22).withTrimmedRight(10), juce::Justification::centredLeft, 1);
    if (shortcut.isNotEmpty()) {
        g.setColour((isHighlighted ? p.panel : p.textDim).withAlpha(0.6f));
        g.drawText(shortcut, r.withTrimmedRight(10), juce::Justification::centredRight);
    }
}

juce::Font LookAndFeel::getPopupMenuFont() { return Theme::get().mono(11.5f); }

juce::Rectangle<int> LookAndFeel::getTooltipBounds(const juce::String& text, juce::Point<int> pos,
                                                   juce::Rectangle<int> parent) {
    const int w = int(juce::GlyphArrangement::getStringWidth(Theme::get().mono(11.0f), text)) + 14;
    const int h = 22;
    return juce::Rectangle<int>(pos.x > parent.getCentreX() ? pos.x - w - 6 : pos.x + 12,
                                pos.y > parent.getCentreY() ? pos.y - h - 6 : pos.y + 14, w, h)
        .constrainedWithin(parent);
}

void LookAndFeel::drawTooltip(juce::Graphics& g, const juce::String& text, int w, int h) {
    const auto& p = pal();
    g.fillAll(p.text);
    g.setColour(p.panel);
    g.setFont(Theme::get().mono(11.0f));
    g.drawText(text, juce::Rectangle<int>(0, 0, w, h).reduced(7, 0), juce::Justification::centredLeft);
}

//==============================================================================
namespace draw {

void bracketTitle(juce::Graphics& g, juce::Rectangle<int> r, const juce::String& title, float size) {
    g.setColour(pal().text);
    g.setFont(Theme::get().mono(size));
    g.drawText("[" + title.toUpperCase() + "]", r, juce::Justification::centredLeft, true);
}

void caption(juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text, juce::Justification j, float size) {
    g.setColour(pal().textDim);
    g.setFont(Theme::get().mono(size));
    g.drawFittedText(text.toUpperCase(), r, j, 1, 1.0f);
}

void cornerMarks(juce::Graphics& g, juce::Rectangle<float> r, bool allFour) {
    g.setColour(pal().textFaint);
    const float s = 4.5f;
    auto mark = [&](juce::Point<float> pt) {
        g.fillRect(pt.x - s, pt.y - 0.5f, s * 2.0f, 1.0f);
        g.fillRect(pt.x - 0.5f, pt.y - s, 1.0f, s * 2.0f);
    };
    mark(r.getTopLeft());
    mark(r.getBottomRight());
    if (allFour) {
        mark(r.getTopRight());
        mark(r.getBottomLeft());
    }
}

void hatch(juce::Graphics& g, juce::Rectangle<int> r, juce::Colour colour, int spacing) {
    juce::Graphics::ScopedSaveState state(g);
    g.reduceClipRegion(r);
    g.setColour(colour);
    for (int x = r.getX() - r.getHeight(); x < r.getRight(); x += spacing)
        g.drawLine(float(x), float(r.getBottom()), float(x + r.getHeight()), float(r.getY()), 1.0f);
}

void dotField(juce::Graphics& g, juce::Rectangle<int> r, int spacing) {
    g.setColour(pal().dotField);
    for (int y = r.getY() + spacing / 2; y < r.getBottom(); y += spacing)
        for (int x = r.getX() + spacing / 2; x < r.getRight(); x += spacing) g.fillRect(x, y, 1, 1);
}

void panel(juce::Graphics& g, juce::Rectangle<int> r) {
    g.setColour(pal().panel);
    g.fillRect(r);
    g.setColour(pal().ruleStrong);
    g.drawRect(r, 1);
}

void statusChip(juce::Graphics& g, juce::Rectangle<int> r, const juce::String& label, bool problem, bool working) {
    const auto& p = pal();
    if (problem) {
        g.setColour(p.accent);
        g.fillRect(r);
    }
    g.setColour(problem ? p.accent : (working ? p.accent : p.ruleStrong));
    g.drawRect(r, 1);
    g.setColour(problem ? p.accentText : (working ? p.accent : p.textDim));
    g.fillRect(r.getX() + 7, r.getCentreY() - 3, 6, 6);
    g.setFont(Theme::get().mono(10.5f));
    g.drawText(label.toUpperCase(), r.withTrimmedLeft(19).withTrimmedRight(6), juce::Justification::centredLeft);
}

void tag(juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text, juce::Colour colour) {
    g.setColour(colour);
    g.drawRect(r, 1);
    g.setFont(Theme::get().mono(9.0f));
    g.drawText(text.toUpperCase(), r, juce::Justification::centred);
}

void dashedEllipse(juce::Graphics& g, juce::Rectangle<float> r, float thickness, float dash, float gap) {
    juce::Path e, d;
    e.addEllipse(r);
    const float pattern[] = {dash, gap};
    juce::PathStrokeType(thickness).createDashedStroke(d, e, pattern, 2);
    g.fillPath(d);
}

void dashedRect(juce::Graphics& g, juce::Rectangle<float> r, float dash, float gap) {
    juce::Path e, d;
    e.addRectangle(r);
    const float pattern[] = {dash, gap};
    juce::PathStrokeType(1.0f).createDashedStroke(d, e, pattern, 2);
    g.fillPath(d);
}

void reticle(juce::Graphics& g, juce::Rectangle<float> r, float legFraction) {
    const float lx = r.getWidth() * legFraction, ly = r.getHeight() * legFraction;
    const float t = 1.25f;
    auto corner = [&](float x, float y, float sx, float sy) {
        g.fillRect(juce::Rectangle<float>(sx > 0 ? x : x - lx, y - t * 0.5f, lx, t));
        g.fillRect(juce::Rectangle<float>(x - t * 0.5f, sy > 0 ? y : y - ly, t, ly));
    };
    corner(r.getX(), r.getY(), 1, 1);
    corner(r.getRight(), r.getY(), -1, 1);
    corner(r.getX(), r.getBottom(), 1, -1);
    corner(r.getRight(), r.getBottom(), -1, -1);
}

} // namespace draw
} // namespace openbfd::ui
