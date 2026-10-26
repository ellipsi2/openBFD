// What the panels need from the editor: the processor, the hint line and the
// cable gesture (pull a cable out of a mic or a rear jack, carry it, drop it).
#pragma once

#include <JuceHeader.h>

#include <string>

class OpenBfdProcessor;

namespace openbfd::ui {

class UiHost {
public:
    virtual ~UiHost() = default;
    virtual OpenBfdProcessor& processor() = 0;
    virtual void setHint(const juce::String& text) = 0;

    // Cable gesture. Positions are in `source` coordinates; the gesture lives
    // in the component that received the mouse-down.
    virtual void cableBegin(const std::string& micKey, juce::Component& source, juce::Point<float> plugAt) = 0;
    virtual void cableMove(juce::Component& source, juce::Point<float> pos) = 0;
    virtual void cableEnd(juce::Component& source, juce::Point<float> pos) = 0;
    virtual const std::string& cableKey() const = 0; // "" while no cable is held

    // Stage mics stop taking the mouse (and draw faint) while locked.
    virtual void setMicsLocked(bool locked) = 0;
};

} // namespace openbfd::ui
