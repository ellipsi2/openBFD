// Small shared controls and glyphs: knob, 3-way segment, stereo meter, mic
// and jack glyphs, plus the naming helpers used by the stage and the mixer.
#pragma once

#include <JuceHeader.h>

#include "../../Engine/Mixer.h"
#include "Theme.h"

namespace openbfd::ui {

// --- names --------------------------------------------------------------------

// Non-ASCII glyphs must go through UTF-8 explicitly (juce::String(const char*) is ASCII).
inline juce::String dashText() { return juce::String::fromUTF8("\xe2\x80\x94"); }
inline juce::String arrowText() { return juce::String::fromUTF8("  \xe2\x86\x92  "); }

// Mic caption as an instrument label ("KICK IN", "OH L", "SNR TOP").
juce::String micCaption(const bfdengine::MicSource& m, const bfdmodel::Piece* piece);
// Short code drawn next to a mic glyph on the stage ("IN", "OUT", "TOP", "L").
juce::String micCode(const bfdengine::MicSource& m);
// "3L" / "12R" for a jack, "" when unpatched.
juce::String jackCode(bfdengine::Jack j);
const bfdengine::MicSource* findMic(const std::vector<bfdengine::MicSource>& mics, const std::string& key);

// --- knob ---------------------------------------------------------------------

// Rotary knob with a caption underneath. Double-click returns to `defaultValue`.
class Knob : public juce::Slider {
public:
    Knob(const juce::String& caption, double defaultValue, bool bipolar = false);
    juce::String caption;
    std::function<juce::String(double)> format; // readout while under the hand
    std::function<void(const juce::String&)> onHint;

    void mouseEnter(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void valueChanged() override;
};

// Knob + caption laid out as one cell.
class KnobCell : public juce::Component {
public:
    KnobCell(const juce::String& label, double defaultValue, bool bipolar = false);
    Knob knob;       // knob.caption = the hint name
    juce::String label; // drawn under the knob
    void resized() override;
    void paint(juce::Graphics&) override;
};

// --- 3-way segment (OFF / DIR / ON) -------------------------------------------

class Segment : public juce::Component {
public:
    explicit Segment(juce::StringArray labels);
    void setSelected(int index, bool notify = false);
    int selected() const { return selected_; }
    std::function<void(int)> onChange;
    std::function<void(const juce::String&)> onHint;
    juce::StringArray hints;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;

private:
    int cellAt(juce::Point<int>) const;
    juce::StringArray labels_;
    int selected_ = 0, hover_ = -1;
};

// --- meter ----------------------------------------------------------------------

// Fader position (0..1) for a level, BFD fader law (inverse of faderDb).
float faderPosForDb(float db);

// Two bars (L, R), vertical or horizontal. Levels in dBFS; only clipping turns accent.
class Meter : public juce::Component {
public:
    explicit Meter(bool horizontal = false) : horizontal_(horizontal) {}
    void setLevels(float dbL, float dbR); // smoothed by the caller
    void paint(juce::Graphics&) override;
    static float dbToY(float db, float height); // shared with fader scales

private:
    bool horizontal_;
    float l_ = -120.0f, r_ = -120.0f, holdL_ = -120.0f, holdR_ = -120.0f;
};

// --- glyphs ---------------------------------------------------------------------

// Mic glyph centred at `c` (16 grid, 1.4 px strokes). `ambient` draws the
// stand-mounted variant. `angle` points the capsule (radians, 0 = up).
void drawMic(juce::Graphics&, juce::Point<float> c, float angle, bool ambient, juce::Colour ink, juce::Colour fill);
// Bleed source glyph (a small box with a jack socket).
void drawBleedSource(juce::Graphics&, juce::Point<float> c, juce::Colour ink, juce::Colour fill);
// Jack plug drawn at `tip`, pointing along `angle` (radians, direction of insertion).
// `inserted` (px) pushes it past `tip` into a socket face: what passes the face
// is not drawn. kPlugSeated hides the whole sleeve and seats the barrel.
constexpr float kPlugSeated = 11.5f;
constexpr float kPlugLength = 27.0f; // tip to the end of the strain relief
void drawPlug(juce::Graphics&, juce::Point<float> tip, float angle, juce::Colour ink, juce::Colour fill,
              float inserted = 0.0f);
// Input socket on the mixer's rear panel.
void drawSocket(juce::Graphics&, juce::Point<float> c, float radius, bool occupied, bool hot);

} // namespace openbfd::ui
