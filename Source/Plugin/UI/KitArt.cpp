#include "KitArt.h"

#include "Theme.h"

namespace openbfd::ui {

using bfdmodel::PieceKind;

namespace {

constexpr float kTwoPi = juce::MathConstants<float>::twoPi;

void dashedCircle(juce::Graphics& g, juce::Point<float> c, float r, float t, float dash, float gap) {
    draw::dashedEllipse(g, juce::Rectangle<float>(r * 2.0f, r * 2.0f).withCentre(c), t, dash, gap);
}

void circle(juce::Graphics& g, juce::Point<float> c, float r, float t) {
    g.drawEllipse(juce::Rectangle<float>(r * 2.0f, r * 2.0f).withCentre(c), t);
}

void disc(juce::Graphics& g, juce::Point<float> c, float r) {
    g.fillEllipse(juce::Rectangle<float>(r * 2.0f, r * 2.0f).withCentre(c));
}

// Tension lugs as short radial ticks just outside the hoop.
void lugs(juce::Graphics& g, juce::Point<float> c, float r, int n, float len, float phase = 0.0f) {
    for (int i = 0; i < n; ++i) {
        const float a = phase + float(i) * kTwoPi / float(n);
        g.drawLine({c.getPointOnCircumference(r, a), c.getPointOnCircumference(r + len, a)}, 1.0f);
    }
}

// Stand legs (hidden under the piece): dashed radial lines + feet.
void tripod(juce::Graphics& g, juce::Point<float> c, float reach, float phase) {
    const float dashes[] = {2.5f, 2.0f};
    for (int i = 0; i < 3; ++i) {
        const float a = phase + float(i) * kTwoPi / 3.0f;
        juce::Path leg, d;
        leg.startNewSubPath(c);
        leg.lineTo(c.getPointOnCircumference(reach, a));
        juce::PathStrokeType(1.0f).createDashedStroke(d, leg, dashes, 2);
        g.fillPath(d);
        disc(g, c.getPointOnCircumference(reach, a), 1.4f);
    }
}

void drum(juce::Graphics& g, juce::Point<float> c, float r, float ppi, const PieceInk& ink, int lugCount,
          bool floorLegs, bool snare) {
    if (floorLegs) {
        g.setColour(ink.soft);
        for (int i = 0; i < 3; ++i) {
            const float a = 0.6f + float(i) * kTwoPi / 3.0f;
            const auto foot = c.getPointOnCircumference(r + 3.2f * ppi, a);
            g.drawLine({c.getPointOnCircumference(r, a), foot}, 1.0f);
            disc(g, foot, 1.6f);
        }
    }
    g.setColour(ink.fill);
    disc(g, c, r);
    g.setColour(ink.soft);
    lugs(g, c, r, lugCount, std::max(2.0f, 0.55f * ppi), kTwoPi / float(lugCount * 2));
    g.setColour(ink.line);
    circle(g, c, r, 1.25f);                        // hoop
    g.setColour(ink.soft);
    circle(g, c, r - std::max(1.5f, 0.45f * ppi), 1.0f); // head edge inside the hoop
    if (snare) {
        // strainer + butt plate on the shell, snare wires as a hidden band
        const float s = std::max(2.0f, 0.6f * ppi);
        g.setColour(ink.line);
        g.drawRect(juce::Rectangle<float>(s * 1.6f, s).withCentre(c.translated(0.0f, -r - s * 0.2f)), 1.0f);
        const float dashes[] = {2.0f, 2.5f};
        for (float dx : {-0.22f, 0.0f, 0.22f}) {
            juce::Path w, d;
            w.startNewSubPath(c.translated(dx * r, -r * 0.82f));
            w.lineTo(c.translated(dx * r, r * 0.82f));
            juce::PathStrokeType(1.0f).createDashedStroke(d, w, dashes, 2);
            g.setColour(ink.soft);
            g.fillPath(d);
        }
    }
}

void cymbal(juce::Graphics& g, juce::Point<float> c, float r, float ppi, const PieceInk& ink, bool china, bool hat) {
    g.setColour(ink.soft);
    tripod(g, c, std::max(r * 0.95f, 8.0f * ppi), 0.35f);
    g.setColour(ink.fill);
    disc(g, c, r);
    if (hat) {
        // bottom cymbal peeks out on the drummer's side
        g.setColour(ink.soft);
        circle(g, c.translated(0.0f, 0.55f * ppi), r, 1.0f);
        g.setColour(ink.fill);
        disc(g, c, r - 0.5f);
    }
    g.setColour(ink.line);
    circle(g, c, r, 1.25f);
    if (china) circle(g, c, r - std::max(1.6f, 0.7f * ppi), 1.0f); // upturned flange
    // lathe grooves
    g.setColour(ink.soft);
    for (float f : {0.78f, 0.58f}) dashedCircle(g, c, r * f, 1.0f, 1.5f, 3.0f);
    // bell + stand tip
    const float bell = std::max(2.5f, r * (china ? 0.24f : hat ? 0.3f : 0.22f));
    g.setColour(ink.line);
    circle(g, c, bell, 1.0f);
    disc(g, c, 1.4f);
}

void kick(juce::Graphics& g, juce::Point<float> c, float d, float ppi, const PieceInk& ink) {
    const float w = d * ppi, h = kickDepth(d) * ppi;
    const auto shell = juce::Rectangle<float>(w, h).withCentre(c);
    const float hoop = std::max(2.0f, 1.0f * ppi);
    // spurs (hidden) and the pedal on the drummer's side
    g.setColour(ink.soft);
    for (float sx : {-1.0f, 1.0f}) {
        const auto a = juce::Point<float>(c.x + sx * w * 0.5f, shell.getY() + hoop * 1.5f);
        const auto b = a.translated(sx * 3.5f * ppi, -2.5f * ppi);
        g.drawLine({a, b}, 1.0f);
        disc(g, b, 1.6f);
    }
    const auto pedal = juce::Rectangle<float>(3.4f * ppi, 9.0f * ppi)
                           .withCentre({c.x, shell.getBottom() + 4.2f * ppi});
    g.drawRect(pedal, 1.0f);
    g.drawLine(pedal.getCentreX(), shell.getBottom(), pedal.getCentreX(), pedal.getY() + 2.0f * ppi, 1.0f);
    disc(g, {pedal.getCentreX(), shell.getBottom() + 1.2f * ppi}, std::max(2.0f, 1.0f * ppi)); // beater
    g.setColour(ink.fill);
    g.fillRect(shell);
    g.setColour(ink.line);
    g.drawRect(shell, 1.25f);
    // hoops front (top) and batter (bottom)
    g.drawRect(shell.withHeight(hoop), 1.0f);
    g.drawRect(shell.withTop(shell.getBottom() - hoop), 1.0f);
    // tension rods along the shell
    g.setColour(ink.soft);
    const int rods = 5;
    for (int i = 0; i < rods; ++i) {
        const float x = shell.getX() + w * (float(i) + 0.5f) / float(rods);
        g.drawLine(x, shell.getY() + hoop, x, shell.getY() + hoop + 1.4f * ppi, 1.0f);
        g.drawLine(x, shell.getBottom() - hoop, x, shell.getBottom() - hoop - 1.4f * ppi, 1.0f);
    }
    // port hole in the front head (seen through the shell as a hidden line)
    g.setColour(ink.soft);
    g.drawLine(c.x - 2.5f * ppi, shell.getY() + hoop * 0.5f, c.x + 2.5f * ppi, shell.getY() + hoop * 0.5f, 1.0f);
}

void perc(juce::Graphics& g, juce::Point<float> c, float d, float ppi, const PieceInk& ink) {
    const auto box = juce::Rectangle<float>(d * ppi, d * ppi).withCentre(c);
    g.setColour(ink.fill);
    g.fillRect(box);
    draw::hatch(g, box.toNearestInt(), ink.soft, 5);
    g.setColour(ink.line);
    g.drawRect(box, 1.25f);
}

} // namespace

float kickDepth(float diameterIn) { return juce::jlimit(12.0f, 20.0f, diameterIn * 0.74f); }

bool isCymbal(PieceKind k) {
    return k == PieceKind::Hihat || k == PieceKind::Crash || k == PieceKind::Ride || k == PieceKind::Splash ||
           k == PieceKind::China;
}

int pieceLayer(PieceKind k) {
    switch (k) {
        case PieceKind::Kick: return 0;
        case PieceKind::FloorTom: return 1;
        case PieceKind::Tom: return 2;
        case PieceKind::Snare: return 3;
        case PieceKind::Perc: return 4;
        case PieceKind::Ride: return 5;
        case PieceKind::China: return 6;
        case PieceKind::Crash: return 7;
        case PieceKind::Hihat: return 8;
        case PieceKind::Splash: return 9;
    }
    return 4;
}

void drawPiece(juce::Graphics& g, PieceKind kind, juce::Point<float> c, float d, float ppi, const PieceInk& ink) {
    const float r = d * 0.5f * ppi;
    switch (kind) {
        case PieceKind::Kick: kick(g, c, d, ppi, ink); break;
        case PieceKind::Snare: drum(g, c, r, ppi, ink, 10, false, true); break;
        case PieceKind::Tom: drum(g, c, r, ppi, ink, d >= 13.0f ? 8 : 6, false, false); break;
        case PieceKind::FloorTom: drum(g, c, r, ppi, ink, 8, true, false); break;
        case PieceKind::Hihat: cymbal(g, c, r, ppi, ink, false, true); break;
        case PieceKind::China: cymbal(g, c, r, ppi, ink, true, false); break;
        case PieceKind::Crash:
        case PieceKind::Ride:
        case PieceKind::Splash: cymbal(g, c, r, ppi, ink, false, false); break;
        case PieceKind::Perc: perc(g, c, d, ppi, ink); break;
    }
}

juce::Rectangle<float> pieceBounds(PieceKind kind, juce::Point<float> c, float d, float ppi) {
    if (kind == PieceKind::Kick) return juce::Rectangle<float>(d * ppi, kickDepth(d) * ppi).withCentre(c);
    return juce::Rectangle<float>(d * ppi, d * ppi).withCentre(c);
}

bool pieceContains(PieceKind kind, juce::Point<float> c, float d, float ppi, juce::Point<float> p) {
    if (kind == PieceKind::Kick || kind == PieceKind::Perc) return pieceBounds(kind, c, d, ppi).contains(p);
    return c.getDistanceFrom(p) <= d * 0.5f * ppi;
}

void drawPieceIcon(juce::Graphics& g, PieceKind kind, juce::Rectangle<float> box, const PieceInk& ink) {
    const auto c = box.getCentre();
    const float r = box.getWidth() * 0.5f - 1.0f;
    g.setColour(ink.line);
    switch (kind) {
        case PieceKind::Kick: {
            const auto shell = juce::Rectangle<float>(r * 2.0f, r * 1.45f).withCentre(c);
            g.drawRect(shell, 1.0f);
            g.drawLine(shell.getX(), shell.getY() + 2.0f, shell.getRight(), shell.getY() + 2.0f, 1.0f);
            g.drawLine(shell.getX(), shell.getBottom() - 2.0f, shell.getRight(), shell.getBottom() - 2.0f, 1.0f);
            break;
        }
        case PieceKind::Snare:
        case PieceKind::Tom:
        case PieceKind::FloorTom:
            circle(g, c, r, 1.0f);
            g.setColour(ink.soft);
            circle(g, c, r - 2.0f, 1.0f);
            if (kind == PieceKind::Snare) g.drawLine(c.x - r * 0.5f, c.y, c.x + r * 0.5f, c.y, 1.0f);
            if (kind == PieceKind::FloorTom) disc(g, c, 1.2f);
            break;
        case PieceKind::Perc:
            g.drawRect(box.reduced(2.0f), 1.0f);
            draw::hatch(g, box.reduced(3.0f).toNearestInt(), ink.soft, 4);
            break;
        default:
            circle(g, c, r, 1.0f);
            g.setColour(ink.soft);
            dashedCircle(g, c, r * 0.62f, 1.0f, 1.2f, 1.8f);
            g.setColour(ink.line);
            circle(g, c, std::max(1.5f, r * 0.24f), 1.0f);
            if (kind == PieceKind::China) circle(g, c, r - 1.6f, 1.0f);
            if (kind == PieceKind::Hihat) {
                g.setColour(ink.soft);
                g.drawLine(c.x - r, c.y + r + 1.0f, c.x + r, c.y + r + 1.0f, 1.0f);
            }
            break;
    }
}

} // namespace openbfd::ui
