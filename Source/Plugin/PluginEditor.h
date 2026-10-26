// openbfd editor: library browser | kit stage | channel mixer, with mic cables
// drawn over everything. Instrument-ui: sheet on ground, hairlines, mono labels.
#pragma once

#include <JuceHeader.h>

#include "PluginProcessor.h"
#include "UI/Browser.h"
#include "UI/CableLayer.h"
#include "UI/KitStage.h"
#include "UI/MixerPanel.h"
#include "UI/Theme.h"
#include "UI/UiHost.h"

class OpenBfdEditor : public juce::AudioProcessorEditor,
                      public juce::DragAndDropContainer,
                      public openbfd::ui::UiHost,
                      private juce::ChangeListener,
                      private juce::Timer {
public:
    explicit OpenBfdEditor(OpenBfdProcessor&);
    ~OpenBfdEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;

    // UiHost
    OpenBfdProcessor& processor() override { return proc_; }
    void setHint(const juce::String& text) override;
    void cableBegin(const std::string& micKey, juce::Component& source, juce::Point<float> plugAt) override;
    void cableMove(juce::Component& source, juce::Point<float> pos) override;
    void cableEnd(juce::Component& source, juce::Point<float> pos) override;
    const std::string& cableKey() const override { return cableKey_; }
    void setMicsLocked(bool locked) override;

    // Tools (ui_shot): sync panels with the processor right now; panel access.
    void syncNow();
    openbfd::ui::KitStage& stage() { return stage_; }
    openbfd::ui::MixerPanel& mixerPanel() { return mixer_; }

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void applyTheme(bool dark);
    void updateCables();
    void updateCableFocus();
    std::string cableKeyAt(juce::Point<float> editorPos) const;
    void chooseKitFile(bool save);
    void updateUndoButtons();
    bfdengine::Jack jackUnder(juce::Point<float> editorPos) const;

    OpenBfdProcessor& proc_;
    openbfd::ui::LookAndFeel lnf_;
    juce::TooltipWindow tooltips_{this, 600};

    openbfd::ui::Browser browser_{*this};
    openbfd::ui::KitStage stage_{*this};
    openbfd::ui::MixerPanel mixer_{*this};
    openbfd::ui::CableLayer cables_;
    juce::TextButton undo_{"UNDO"}, redo_{"REDO"}, open_{"OPEN"}, save_{"SAVE"}, theme_{"DARK"};
    std::unique_ptr<juce::FileChooser> chooser_;

    juce::String hint_;
    std::string cableKey_;
    bfdengine::Jack hotJack_;
    std::string hoverCable_; // plugged cable whose mic / jack / plug is under the mouse
    double hoverSince_ = 0.0;
    int flipBackToken_ = 0;
    int voices_ = 0, layersReady_ = 0, layersTotal_ = 0, layersDone_ = 0;
    bool cacheFull_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OpenBfdEditor)
};
