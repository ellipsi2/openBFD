// Mic cables drawn over the whole editor. Each cable is a verlet rope hanging
// under gravity between its mic and its plug, with some slack so it sags and
// swings; the bottom of the editor is the floor. Pulling the plug pays cable
// out of the mic; extra slack is reeled back in. A plug that is in its jack is
// pushed all the way home. A cable dropped anywhere but a jack retracts into
// its mic. Never takes the mouse.
#pragma once

#include <JuceHeader.h>

#include <map>
#include <string>

namespace openbfd::ui {

class CableLayer : public juce::Component, private juce::Timer {
public:
    using P = juce::Point<float>;

    CableLayer();

    struct Ends {
        std::string key;
        P anchor, jack;
        bool shown = true; // false: the jack is scrolled out of view
    };
    // Plugged cables this frame (editor coordinates). Cables that are gone retract.
    void setPlugged(const std::vector<Ends>& ends);
    // How much the plugged cables show (follows the mixer's flip).
    void setReveal(float amount);

    void beginDrag(const std::string& key, P anchor, P plug);
    void dragTo(P anchor, P plug, bool onJack);
    // `plugged`: the cable stays in its jack; otherwise it retracts.
    void endDrag(bool plugged);
    bool holding() const { return held_.has_value(); }

    void paint(juce::Graphics&) override;
    bool hitTest(int, int) override { return false; }

private:
    struct Rope {
        std::vector<P> pts, prev;
        float payout = 0.0f; // rest length of the first segment (cable leaving the mic)
        P anchor, plug;
        bool onJack = false; // plug at a socket: leaves it horizontally
        bool held = false;   // in the hand (not pushed home yet)
        float seat = 0.0f;   // 0 = tip at the socket face .. 1 = fully inserted
        bool shown = true;
        float retract = -1.0f; // >= 0 while reeling back in
    };
    static constexpr float kSeg = 10.0f;

    void timerCallback() override;
    static Rope makeRope(P anchor, P plug, bool lying);
    static void step(Rope& r, float desired, float floor);
    void drawRope(juce::Graphics&, const Rope&, juce::Colour, float alpha, bool held) const;
    bool busy() const;

    std::map<std::string, Rope> plugged_;
    std::optional<Rope> held_;
    std::string heldKey_;
    std::vector<Rope> retracting_;
    float reveal_ = 0.0f;
};

} // namespace openbfd::ui
