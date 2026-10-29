// Keymap: every articulation of every piece in the kit, with the MIDI notes
// that play it. Notes are chips (click to unassign); "+" picks a note from a
// list, LEARN takes the next note that comes in from the host. Clicking an
// articulation's name plays it. Takes the mixer's place while it is open.
#pragma once

#include <JuceHeader.h>

#include "UiHost.h"
#include "Widgets.h"

namespace openbfd::ui {

// "D1" (middle C = C3, the drum-map convention: 36 = C1).
juce::String noteName(int note);
// Readable name for a BFD articulation code ("ClosedT" -> "Closed tip").
juce::String articulationLabel(const std::string& name);

class KeymapPanel : public juce::Component, private juce::Timer {
public:
    explicit KeymapPanel(UiHost& host);
    ~KeymapPanel() override;

    void sync();           // kit or keymap changed
    void noteIn(int note); // host MIDI note-on (message thread)
    bool learning() const;

    void paint(juce::Graphics&) override;
    void resized() override;

    struct Row {
        bool header = false;
        int slot = -1;
        int kind = -1;          // bfdmodel::PieceKind
        juce::String text, sub; // header: piece name + kind code; row: label + code
        std::string articulation;
        std::vector<int> notes;
    };

private:
    class RowList;
    void timerCallback() override;

    UiHost& host_;
    juce::TextButton reset_{"AUTO MAP"};
    juce::Viewport viewport_;
    std::unique_ptr<RowList> list_;
    juce::String lastIn_; // "38 D1 -> SNR  Rim shot"
};

} // namespace openbfd::ui
