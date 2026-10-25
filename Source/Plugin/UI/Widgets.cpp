#include "Widgets.h"

#include "Motion.h"

namespace openbfd::ui {

using bfdengine::MicKind;
using bfdengine::MicSource;

// --- names --------------------------------------------------------------------

namespace {

juce::String shortChan(juce::String name) {
    name = name.trim();
    name = name.replace("Left", "L").replace("Right", "R").replace("Bottom", "Bot");
    return name;
}

} // namespace

juce::String micCaption(const MicSource& m, const bfdmodel::Piece* piece) {
    if (m.kind == MicKind::Bleed) return "OTHER BL";
    if (m.kind == MicKind::Ambient) return shortChan(m.chanName).toUpperCase();
    juce::String chan = shortChan(m.chanName);
    if (piece) {
        const auto kind = bfdmodel::pieceKind(*piece);
        if (kind == bfdmodel::PieceKind::Kick) return chan.toUpperCase();          // KICK IN
        if (kind == bfdmodel::PieceKind::Snare) return chan.replace("Snare", "SNR").toUpperCase();
        return juce::String(bfdmodel::pieceKindCode(kind)) + (chan.equalsIgnoreCase("Direct") ? "" : " " + chan.toUpperCase());
    }
    return chan.toUpperCase();
}

juce::String micCode(const MicSource& m) {
    if (m.kind == MicKind::Bleed) return "BL";
    juce::String c = shortChan(m.chanName).toUpperCase();
    if (m.kind == MicKind::Ambient) {
        if (m.encoding == "stereoLeft") return "L";
        if (m.encoding == "stereoRight") return "R";
        if (m.encoding == "stereoMid") return "M";
        if (m.encoding == "stereoSide") return "S";
        return c.fromLastOccurrenceOf(" ", false, false).isEmpty() ? c : c.fromLastOccurrenceOf(" ", false, false);
    }
    if (c.startsWith("KICK ")) return c.substring(5);
    if (c.startsWith("SNARE ")) return c.substring(6);
    if (c == "DIRECT") return "DI";
    return c.substring(0, 3);
}

juce::String jackCode(bfdengine::Jack j) {
    if (!j.valid()) return {};
    return juce::String(j.strip + 1) + (j.side == 0 ? "L" : "R");
}

const MicSource* findMic(const std::vector<MicSource>& mics, const std::string& key) {
    for (const auto& m : mics)
        if (m.key == key) return &m;
    return nullptr;
}

// --- knob ---------------------------------------------------------------------

Knob::Knob(const juce::String& c, double defaultValue, bool bipolar) : caption(c) {
    setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    setRange(0.0, 1.0, 0.0);
    setValue(defaultValue, juce::dontSendNotification);
    setDoubleClickReturnValue(true, defaultValue);
    setRotaryParameters(juce::MathConstants<float>::pi * 1.22f, juce::MathConstants<float>::pi * 2.78f, true);
    setMouseDragSensitivity(180);
    getProperties().set("bipolar", bipolar);
}

void Knob::mouseEnter(const juce::MouseEvent& e) {
    juce::Slider::mouseEnter(e);
    if (onHint) onHint(caption + "  " + (format ? format(getValue()) : juce::String(getValue(), 2)));
}

void Knob::mouseExit(const juce::MouseEvent& e) {
    juce::Slider::mouseExit(e);
    if (onHint) onHint({});
}

void Knob::valueChanged() {
    if (onHint && isMouseOverOrDragging()) onHint(caption + "  " + (format ? format(getValue()) : juce::String(getValue(), 2)));
}

KnobCell::KnobCell(const juce::String& c, double defaultValue, bool bipolar)
    : knob(c, defaultValue, bipolar), label(c) {
    addAndMakeVisible(knob);
}

void KnobCell::resized() {
    auto r = getLocalBounds();
    r.removeFromBottom(11);
    knob.setBounds(r.withSizeKeepingCentre(std::min(r.getWidth(), r.getHeight()), std::min(r.getWidth(), r.getHeight())));
}

void KnobCell::paint(juce::Graphics& g) {
    g.setColour(pal().textDim);
    g.setFont(Theme::get().mono(8.5f));
    g.drawText(label.toUpperCase(), getLocalBounds().removeFromBottom(10), juce::Justification::centred, false);
}

// --- segment --------------------------------------------------------------------

Segment::Segment(juce::StringArray labels) : labels_(std::move(labels)) {}

void Segment::setSelected(int index, bool notify) {
    index = juce::jlimit(0, labels_.size() - 1, index);
    if (index == selected_) return;
    selected_ = index;
    repaint();
    if (notify && onChange) onChange(index);
}

int Segment::cellAt(juce::Point<int> p) const {
    if (labels_.isEmpty() || !getLocalBounds().contains(p)) return -1;
    return juce::jlimit(0, labels_.size() - 1, p.x * labels_.size() / std::max(1, getWidth()));
}

void Segment::paint(juce::Graphics& g) {
    const auto& p = pal();
    const auto r = getLocalBounds().toFloat();
    const int n = std::max(1, labels_.size());
    const float cw = r.getWidth() / float(n);
    // selection block slides between cells
    const float x = motion::follow(*this, "sel", float(selected_) * cw, motion::normal);
    g.setColour(p.text);
    g.fillRect(juce::Rectangle<float>(x, 0.0f, cw, r.getHeight()));
    g.setColour(p.ruleStrong);
    g.drawRect(r, 1.0f);
    for (int i = 1; i < n; ++i) g.fillRect(juce::Rectangle<float>(float(i) * cw, 0.0f, 1.0f, r.getHeight()));
    g.setFont(Theme::get().mono(8.5f));
    for (int i = 0; i < n; ++i) {
        const auto cell = juce::Rectangle<float>(float(i) * cw, 0.0f, cw, r.getHeight());
        const float under = juce::jlimit(0.0f, 1.0f, 1.0f - std::abs(x - float(i) * cw) / cw);
        auto ink = (i == hover_ ? p.text : p.textDim).interpolatedWith(p.panel, under);
        g.setColour(ink);
        g.drawText(labels_[i], cell, juce::Justification::centred, false);
    }
}

void Segment::mouseDown(const juce::MouseEvent& e) {
    const int c = cellAt(e.getPosition());
    if (c >= 0) setSelected(c, true);
}

void Segment::mouseMove(const juce::MouseEvent& e) {
    const int c = cellAt(e.getPosition());
    if (c != hover_) {
        hover_ = c;
        repaint();
        if (onHint) onHint(c >= 0 && c < hints.size() ? hints[c] : juce::String());
    }
}

void Segment::mouseExit(const juce::MouseEvent&) {
    hover_ = -1;
    repaint();
    if (onHint) onHint({});
}

// --- meter ----------------------------------------------------------------------

float Meter::dbToY(float db, float height) {
    // Same law as the fader (0.8 = 0 dB, cubic), so fader ticks and meter agree.
    return height * (1.0f - faderPosForDb(db));
}

float faderPosForDb(float db) {
    if (db <= -119.0f) return 0.0f;
    return juce::jlimit(0.0f, 1.0f, 0.8f * std::pow(10.0f, db / 60.0f));
}

void Meter::setLevels(float dbL, float dbR) {
    l_ = dbL;
    r_ = dbR;
    holdL_ = std::max(dbL, holdL_ - 0.6f);
    holdR_ = std::max(dbR, holdR_ - 0.6f);
    repaint();
}

void Meter::paint(juce::Graphics& g) {
    const auto& p = pal();
    if (horizontal_) {
        // L on top, R below; level grows to the right on the fader's law
        const float w = float(getWidth());
        const float bh = std::max(1.0f, (float(getHeight()) - 1.0f) * 0.5f);
        auto bar = [&](float y, float db, float hold) {
            g.setColour(p.rule);
            g.fillRect(0.0f, y, w, bh);
            const float x = w * faderPosForDb(db);
            g.setColour(db > 0.0f ? p.accent : p.text.withAlpha(0.78f));
            g.fillRect(0.0f, y, x, bh);
            if (hold > -59.0f) {
                g.setColour(hold > 0.0f ? p.accent : p.text);
                g.fillRect(w * faderPosForDb(hold) - 1.0f, y, 1.0f, bh);
            }
        };
        bar(0.0f, l_, holdL_);
        bar(bh + 1.0f, r_, holdR_);
        return;
    }
    const float h = float(getHeight());
    const float bw = std::max(2.0f, (float(getWidth()) - 1.0f) * 0.5f);
    auto bar = [&](float x, float db, float hold) {
        g.setColour(p.rule);
        g.fillRect(x, 0.0f, bw, h);
        const float y = dbToY(db, h);
        g.setColour(db > 0.0f ? p.accent : p.text.withAlpha(0.78f));
        g.fillRect(x, y, bw, h - y);
        if (hold > -59.0f) {
            const float hy = dbToY(hold, h);
            g.setColour(hold > 0.0f ? p.accent : p.text);
            g.fillRect(x, hy, bw, 1.0f);
        }
    };
    bar(0.0f, l_, holdL_);
    bar(bw + 1.0f, r_, holdR_);
}

// --- glyphs ---------------------------------------------------------------------

void drawMic(juce::Graphics& g, juce::Point<float> c, float angle, bool ambient, juce::Colour ink, juce::Colour fill) {
    const juce::PathStrokeType stroke(1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    const auto rot = juce::AffineTransform::rotation(angle).translated(c);
    if (ambient) {
        // stand: tripod circle + boom, capsule at the boom end (top view)
        juce::Path stand;
        stand.addEllipse(-7.5f, -7.5f, 15.0f, 15.0f);
        juce::Path legs;
        for (int i = 0; i < 3; ++i) {
            const float a = float(i) * juce::MathConstants<float>::twoPi / 3.0f + 0.5f;
            legs.startNewSubPath(0, 0);
            legs.lineTo(std::sin(a) * 7.5f, -std::cos(a) * 7.5f);
        }
        g.setColour(ink.withMultipliedAlpha(0.45f));
        const float dashes[] = {2.0f, 2.0f};
        juce::Path dashed;
        juce::PathStrokeType(1.0f).createDashedStroke(dashed, stand, dashes, 2, rot);
        g.fillPath(dashed);
        g.strokePath(legs, juce::PathStrokeType(1.0f), rot);
    }
    // capsule body: rounded head + handle (top view of a side-address mic)
    juce::Path body;
    body.addRoundedRectangle(-2.6f, -1.0f, 5.2f, 9.0f, 1.2f);
    juce::Path head;
    head.addEllipse(-4.2f, -8.6f, 8.4f, 8.4f);
    g.setColour(fill);
    g.fillPath(head, rot);
    g.fillPath(body, rot);
    g.setColour(ink);
    g.strokePath(head, stroke, rot);
    g.strokePath(body, stroke, rot);
    juce::Path grille;
    grille.startNewSubPath(-2.6f, -4.4f);
    grille.lineTo(2.6f, -4.4f);
    g.strokePath(grille, juce::PathStrokeType(1.0f), rot);
}

void drawBleedSource(juce::Graphics& g, juce::Point<float> c, juce::Colour ink, juce::Colour fill) {
    const auto r = juce::Rectangle<float>(18.0f, 14.0f).withCentre(c);
    g.setColour(fill);
    g.fillRect(r);
    g.setColour(ink);
    g.drawRect(r, 1.2f);
    g.drawEllipse(juce::Rectangle<float>(6.0f, 6.0f).withCentre(c), 1.2f);
    g.fillRect(juce::Rectangle<float>(1.6f, 1.6f).withCentre(c));
}

void drawPlug(juce::Graphics& g, juce::Point<float> tip, float angle, juce::Colour ink, juce::Colour fill,
              float inserted) {
    // A 1/4" plug in top view: tip + sleeve, then a barrel and strain relief.
    // Local frame: +y is the insertion direction, the socket face is y = 0 and
    // the plug's tip sits `inserted` past it.
    const auto rot = juce::AffineTransform::rotation(angle).translated(tip);
    const float o = inserted;
    juce::Path sleeve, barrel, relief, ring;
    const float sleeveEnd = std::min(0.0f, o); // the part past the face is inside the socket
    if (sleeveEnd > -9.0f + o + 0.5f) sleeve.addRectangle(-1.6f, -9.0f + o, 3.2f, sleeveEnd - (-9.0f + o));
    if (-3.0f + o < 0.0f) {
        ring.startNewSubPath(-1.6f, -3.0f + o);
        ring.lineTo(1.6f, -3.0f + o);
    }
    barrel.addRectangle(-3.6f, -21.0f + o, 7.2f, 12.0f);
    relief.addRectangle(-2.4f, -kPlugLength + o, 4.8f, 6.0f);
    g.setColour(fill);
    g.fillPath(barrel, rot);
    g.fillPath(relief, rot);
    if (!sleeve.isEmpty()) g.fillPath(sleeve, rot);
    g.setColour(ink);
    const juce::PathStrokeType stroke(1.2f);
    if (!sleeve.isEmpty()) g.strokePath(sleeve, stroke, rot);
    if (!ring.isEmpty()) g.strokePath(ring, juce::PathStrokeType(1.0f), rot);
    g.strokePath(barrel, stroke, rot);
    g.strokePath(relief, stroke, rot);
    juce::Path grip;
    for (float y = -18.0f; y <= -12.0f; y += 3.0f) {
        grip.startNewSubPath(-3.6f, y + o);
        grip.lineTo(3.6f, y + o);
    }
    g.strokePath(grip, juce::PathStrokeType(0.8f), rot);
}

void drawSocket(juce::Graphics& g, juce::Point<float> c, float radius, bool occupied, bool hot) {
    const auto& p = pal();
    const auto outer = juce::Rectangle<float>(radius * 2.0f, radius * 2.0f).withCentre(c);
    g.setColour(p.panelInset);
    g.fillEllipse(outer);
    g.setColour(hot ? p.accent : p.ruleStrong);
    g.drawEllipse(outer, hot ? 1.6f : 1.0f);
    const auto nut = outer.reduced(radius * 0.28f);
    g.setColour(p.textFaint);
    g.drawEllipse(nut, 1.0f);
    const auto hole = outer.reduced(radius * 0.62f);
    g.setColour(occupied ? p.text : p.canvas);
    g.fillEllipse(hole);
}

} // namespace openbfd::ui
