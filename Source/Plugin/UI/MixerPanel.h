// Right panel: the channel mixer, one row per channel plus the master. Front =
// levels (pan, fader, meter, mute / solo, output number). Rear = the patch bay
// and the settings: every row's L and R input jacks on its side face, the mics
// plugged into them and the row's bleed controls; the free channels follow.
// The panel turns around on its own while a cable is in the hand.
#pragma once

#include <JuceHeader.h>

#include "../../Engine/Engine.h"
#include "UiHost.h"
#include "Widgets.h"

namespace openbfd::ui {

juce::String formatDb(float db);

// One channel as a horizontal row: number / mode / mic | pan | fader + meter |
// dB | M S | output. Turned around, the same row shows its L and R input jacks
// on its side face (towards the stage, plugs go in sideways), what is plugged
// into each, and the bleed of the piece whose mic it carries: kick and snare
// bleed (level, Off / Dir / On), send, return and input trim. The master's
// rear carries the three bleed masters.
class ChannelStrip : public juce::Component {
public:
    ChannelStrip(UiHost& host, int strip); // strip < 0 = master
    void sync();
    void setMeter(float peakL, float peakR); // linear peaks since the last call
    int preferredHeight() const;
    void paint(juce::Graphics&) override;
    void resized() override;
    std::function<void()> onKitEdited;
    const int strip;
    static constexpr int kMainH = 50, kInfoW = 150, kFreeH = 30, kSideW = 34;

    // rear face
    void setBack(bool back);
    bool isActive() const { return active_; } // something is patched into it
    int jackAt(juce::Point<float> local) const; // side (0 = L, 1 = R), -1 = none
    juce::Point<float> socketCentre(int side) const;
    void setHotSide(int side);
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;

private:
    void editPiece(const std::function<void(bfdmodel::KitPieceState&)>& fn);
    void editMixer(const std::function<void(bfdengine::MixerState&)>& fn);
    bool isMaster() const { return strip < 0; }
    void updateVisibility(); // front: level controls; rear: bleed controls
    void paintBack(juce::Graphics&);

    UiHost& host_;
    Knob kickBleed_{"Kick bleed level", 1.0}, snareBleed_{"Snare bleed level", 1.0}, send_{"Bleed send", 1.0},
        ret_{"Bleed return", 1.0}, in_{"Bleed input trim", 0.8};
    Knob mKick_{"Kick bleed master", 0.8}, mSnare_{"Snare bleed master", 0.8}, mOther_{"Other bleed master", 0.8};
    Segment kickDst_{juce::StringArray{"OFF", "DIR", "ON"}}, snareDst_{juce::StringArray{"OFF", "DIR", "ON"}};
    Knob pan_{"Pan", 0.5, true};
    juce::Slider fader_;
    juce::TextButton mute_{"M"}, solo_{"S"};
    Meter meter_{true};

    int pieceSlot_ = -1;
    bool kickPiece_ = false, snarePiece_ = false;
    bool back_ = false, active_ = false, carrying_ = false;
    int hotSide_ = -1, hoverSide_ = -1;
    juce::String micL_, micR_; // what is plugged into L / R
    juce::String mode_, caption_, sub_;
    float dbL_ = -120.0f, dbR_ = -120.0f;
    // layout
    juce::Rectangle<int> info_, readout_, out_;
    // rear: names column, bleed cells (label on top, control under it)
    juce::Rectangle<int> names_, rearOut_, bleedArea_;
    juce::Rectangle<int> kGroup_, sGroup_, sendCell_, retCell_, inCell_;
    juce::Rectangle<int> mkCell_, msCell_, moCell_;
};

class MixerPanel : public juce::Component, private juce::Timer {
public:
    explicit MixerPanel(UiHost& host);
    ~MixerPanel() override;

    void sync();                                // patch / mixer / kit changed
    void updateMeters(bfdengine::Engine& engine); // ~30 Hz
    void flipTo(bool back);
    bool showingBack() const { return target_; }
    float backAmount() const; // 0 = front facing, 1 = rear facing (cables fade with it)
    bool pinned() const { return flip_.getToggleState(); }

    // Rear jacks, in this panel's coordinates; mid-turn they follow the face that is showing.
    bfdengine::Jack jackAt(juce::Point<float> local) const;
    juce::Point<float> socketCentre(bfdengine::Jack) const; // clamped to the visible rows
    bool jackInView(bfdengine::Jack) const;                 // on the rear and not scrolled away
    void setHotJack(bfdengine::Jack);

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    struct Socket {
        juce::Point<float> at;
        bool inView = false;
    };
    using Sockets = std::array<Socket, size_t(bfdengine::kNumStrips) * 2>;

    void timerCallback() override;
    Socket liveSocket(bfdengine::Jack) const; // laid out face, no turn
    Socket socket(bfdengine::Jack) const;
    Sockets allSockets() const;
    void applyFace(bool back);
    void layoutRows();
    void showCables(bool show);
    void lockMics(bool locked);
    void updateHeadButtons(); // cables on the rear, mic lock on the front
    juce::Rectangle<int> body() const;

    UiHost& host_;
    juce::TextButton autoPatch_{"AUTO"}, flip_{"REAR"}, cables_{"HIDE CABLES"}, micLock_{"LOCK MICS"};
    juce::Viewport viewport_;
    juce::Component holder_;
    std::array<std::unique_ptr<ChannelStrip>, bfdengine::kNumStrips> rows_;
    std::unique_ptr<ChannelStrip> master_;
    std::vector<int> order_; // visible rows, top to bottom
    bool face_ = false;      // face currently laid out (true = rear)
    bfdengine::Jack hot_;

    // flip animation: angle 0 (front) .. 1 (rear)
    bool target_ = false;
    float angle_ = 0.0f;
    double lastTick_ = 0.0;
    juce::Image frontShot_, backShot_;
    Sockets frontSockets_, backSockets_; // where each face had its jacks when the turn began
};

} // namespace openbfd::ui
