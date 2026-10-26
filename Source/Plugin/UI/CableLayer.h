// Mic cables drawn over the whole editor. Each cable is a verlet rope hanging
// under gravity between its mic and its plug, with some slack so it sags and
// swings; the bottom of the editor is the floor. Pulling the plug pays cable
// out of the mic; extra slack is reeled back in. A plug that is in its jack is
// pushed all the way home. A cable dropped anywhere but a jack retracts into
// its mic. Plugged cables show while the mixer faces its rear; one whose jack
// is scrolled away still runs to the edge of the list, faded, and comes up to
// full strength as the jack scrolls into view. Never takes the mouse.
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
    // false: plugged cables are hidden (a focused one still shows).
    void setEnabled(bool enabled);
    // Pick out one plugged cable ("" = none): it takes the accent, the rest step back.
    void setFocus(const std::string& key);

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
        float sight = 1.0f;    // eases to `shown`
        float focus = 0.0f;    // eases to 1 while this cable is picked out
        float retract = -1.0f; // >= 0 while reeling back in
    };
    static constexpr float kSeg = 10.0f;

    void timerCallback() override;
    static Rope makeRope(P anchor, P plug, bool lying);
    static void step(Rope& r, float desired, float floor);
    void drawRope(juce::Graphics&, const Rope&, juce::Colour, float alpha, bool held, float weight = 0.0f) const;
    bool fading() const;
    bool busy() const;

    std::map<std::string, Rope> plugged_;
    std::optional<Rope> held_;
    std::string heldKey_;
    std::vector<Rope> retracting_;
    float reveal_ = 0.0f;
    bool enabled_ = true;
    std::string focusKey_;
    float others_ = 0.0f; // eases to 1 while some cable is picked out
    bool moving_ = true;  // plugged cables still swinging
};

} // namespace openbfd::ui
