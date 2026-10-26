// Left sidebar: kits, presets and pieces of every scanned library. Kits and
// presets load on click; pieces are drag sources ("piece:<slotRef>") for the
// stage, or go to their default spot on double-click.
#pragma once

#include <JuceHeader.h>

#include "UiHost.h"
#include "Widgets.h"

namespace openbfd::ui {

class Browser : public juce::Component, private juce::TextEditor::Listener {
public:
    explicit Browser(UiHost& host);
    ~Browser() override;

    void refresh(); // libraries, kit files or the loaded kit changed
    void paint(juce::Graphics&) override;
    void paintOverChildren(juce::Graphics&) override;
    void resized() override;

    struct Row {
        bool header = false;
        juce::String text, sub, right;
        std::string payload; // kit path or piece slot ref
        int kind = -1;       // bfdmodel::PieceKind for piece rows
        bool loaded = false;
    };

private:
    class RowList;
    // The field keeps the command keys it edits with (select all, copy, cut,
    // paste, undo); every other command shortcut (Cmd/Ctrl+S) goes on to the host.
    class SearchField : public juce::TextEditor {
    public:
        bool keyPressed(const juce::KeyPress& key) override {
            const auto c = juce::CharacterFunctions::toLowerCase(juce::juce_wchar(key.getKeyCode()));
            if (key.getModifiers().isCommandDown() && c >= 'a' && c <= 'z' && juce::String("acvxzy").indexOfChar(c) < 0)
                return false;
            return juce::TextEditor::keyPressed(key);
        }
    };
    void textEditorTextChanged(juce::TextEditor&) override;
    void rebuildRows();
    void chooseFolder();

    UiHost& host_;
    Segment tabs_{juce::StringArray{"KITS", "PRESETS", "PIECES"}};
    SearchField search_;
    juce::TextButton addFolder_{"+ FOLDER"};
    juce::Viewport viewport_;
    std::unique_ptr<RowList> list_;
    std::unique_ptr<juce::FileChooser> chooser_;
    int libraryCount_ = 0, pieceCount_ = 0;
};

} // namespace openbfd::ui
