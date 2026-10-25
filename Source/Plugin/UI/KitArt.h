// Top-down drawings of kit pieces, drawn from geometry (inches) in the
// instrument-ui line language: 1 px outlines on the sheet, dashed hidden
// lines for what sits underneath (stands, bottom mics), no fills but the sheet.
#pragma once

#include <JuceHeader.h>

#include "../../Model/Model.h"

namespace openbfd::ui {

struct PieceInk {
    juce::Colour line;  // outlines
    juce::Colour soft;  // grooves, lugs, hidden lines
    juce::Colour fill;  // head / cymbal face (the sheet)
};

// Kick drums are drawn side-on from above: a shell `diameter` wide and
// `kickDepth(diameter)` deep. Everything else is a circle.
float kickDepth(float diameterIn);

// Draw `kind` centred at `c`. `ppi` = pixels per inch, `diameterIn` the
// piece's size. Cheap enough to call every frame.
void drawPiece(juce::Graphics&, bfdmodel::PieceKind kind, juce::Point<float> c, float diameterIn, float ppi,
               const PieceInk& ink);

// Bounding box (pixels) of the drawing above, without stands.
juce::Rectangle<float> pieceBounds(bfdmodel::PieceKind kind, juce::Point<float> c, float diameterIn, float ppi);

// True when `p` lies on the piece (circle, or the kick's shell rectangle).
bool pieceContains(bfdmodel::PieceKind kind, juce::Point<float> c, float diameterIn, float ppi, juce::Point<float> p);

// Small browser-row glyph (fits a `size` square).
void drawPieceIcon(juce::Graphics&, bfdmodel::PieceKind kind, juce::Rectangle<float> box, const PieceInk& ink);

// Drawing order: drums under cymbals, larger cymbals under smaller ones.
int pieceLayer(bfdmodel::PieceKind kind);
bool isCymbal(bfdmodel::PieceKind kind);

} // namespace openbfd::ui
