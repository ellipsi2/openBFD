// Centre panel: the kit seen from above on a 120 x 90 inch riser, drummer at
// the bottom. Pieces are drawn from their kind and size (KitArt), every mic
// gets a glyph you can pull a cable out of, and pieces dragged in from the
// browser land where they are dropped.
#pragma once

#include <JuceHeader.h>

#include "../../Engine/Mixer.h"
#include "UiHost.h"

namespace openbfd::ui {

class KitStage : public juce::Component, public juce::DragAndDropTarget, private juce::Timer {
public:
    explicit KitStage(UiHost& host);

    void refresh();                 // kit, mics or patch changed
    void hit(int slot, float velocity);
    // Where the cable of `micKey` leaves its mic (stage coordinates).
    bool micPoint(const std::string& micKey, juce::Point<float>& out) const;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;

    bool isInterestedInDragSource(const SourceDetails&) override;
    void itemDragEnter(const SourceDetails&) override;
    void itemDragMove(const SourceDetails&) override;
    void itemDragExit(const SourceDetails&) override;
    void itemDropped(const SourceDetails&) override;

    // Stage geometry: inches, x in [-52, 52] (drummer's left to right),
    // y in [-40, 40] (audience to drummer). Stored in the kit as 0..1.
    static constexpr float kHalfW = 52.0f, kHalfH = 40.0f;

private:
    struct PieceView {
        int slot = -1;
        bfdmodel::PieceKind kind = bfdmodel::PieceKind::Perc;
        float diameter = 12.0f;
        juce::Point<float> pos; // inches
        bool missing = false;
        juce::String name, code;
    };
    struct MicView {
        std::string key;
        juce::Point<float> pos; // inches
        float angle = 0.0f;     // glyph rotation, capsule towards the source
        juce::Point<float> out; // unit vector for the badge (screen space)
        bfdengine::MicKind kind = bfdengine::MicKind::Direct;
        bool hidden = false;    // under the drum (snare bottom)
        int badgeRow = 0;       // ambient rail: badges alternate between two rows
        int slot = -1;
        juce::String code, caption;
    };

    void timerCallback() override;
    void layoutKit();
    juce::Point<float> toPx(juce::Point<float> in) const { return origin_ + in * ppi_; }
    juce::Point<float> toIn(juce::Point<float> px) const { return (px - origin_) / ppi_; }
    const PieceView* pieceAt(juce::Point<float> px) const;
    const MicView* micAt(juce::Point<float> px) const;
    juce::Point<float> piecePos(const PieceView&) const; // with a drag in progress
    void paintChrome(juce::Graphics&);
    void paintPieces(juce::Graphics&);
    void paintMics(juce::Graphics&);
    void paintInspector(juce::Graphics&);
    void paintDropGhost(juce::Graphics&);
    void showMenu(const PieceView&);
    void commitPosition(int slot, juce::Point<float> in);

    UiHost& host_;
    std::vector<PieceView> pieces_; // drawing order
    std::vector<MicView> mics_;
    juce::Rectangle<float> frame_;
    juce::Point<float> origin_;
    float ppi_ = 5.0f;

    int selected_ = -1, hoverSlot_ = -1;
    std::string hoverMic_;
    // gestures
    enum class Gesture { None, Piece, Cable } gesture_ = Gesture::None;
    int dragSlot_ = -1;
    juce::Point<float> dragOffsetIn_, dragPosIn_;
    bool dragMoved_ = false;
    // drop from the browser
    bool dropActive_ = false;
    juce::Point<float> dropPx_;
    int dropTarget_ = -1;
    juce::String dropName_;
    // hit flashes
    std::map<int, std::pair<float, double>> flashes_; // slot -> (velocity, start ms)
};

} // namespace openbfd::ui
