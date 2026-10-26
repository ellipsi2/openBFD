// openbfd — BFD3-compatible drum instrument (VST3).
// Hosts the BFDLAC codec, content-library model, drum engine and mixer.
#pragma once

#include <JuceHeader.h>

#include <functional>
#include <thread>

#include "../Engine/Engine.h"

class OpenBfdProcessor : public juce::AudioProcessor, public juce::ChangeBroadcaster {
public:
    OpenBfdProcessor();
    ~OpenBfdProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "openbfd"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // --- libraries (message thread) ---
    // Synchronous scan (tools); the editor path is scanLibrariesAsync.
    void addLibraryRoot(const juce::String& root);
    // Scan the given roots on a background thread; each finished library is
    // published on the message thread (sendChangeMessage). Empty = BFD3's own
    // DataPaths.xml roots.
    void scanLibrariesAsync(juce::StringArray roots = {});
    bool scanning() const { return scanning_.load(); }
    juce::StringArray libraryRoots() const { return libraryRoots_; }
    const std::vector<bfdmodel::Library>& libraries() const { return engine_.libraries(); }
    const std::vector<bfdmodel::KitFileEntry>& kitFiles() const { return kitFiles_; }

    // --- kit (message thread) ---
    // Load a kit (.bfd2kit/.bfd3kit) or a program preset (.bfd3/.bfd2). A
    // .bfd3 brings its MIDI map and mixer along, and a save round-trips the
    // groove/song state we do not model.
    bool loadKitFile(const juce::String& path);
    bool saveKitFile(const juce::String& path);
    const bfdmodel::Kit& kit() const { return engine_.kit(); }
    bfdengine::PieceLookup pieces() const { return engine_.pieceLookup(); }
    // Edit the kit. New or removed pieces re-run the auto patch for their mics.
    void mutateKit(const std::function<void(bfdmodel::Kit&)>& fn);
    // Put `slotRef` into `slot` (-1 = first free slot) at a stage position; returns the slot.
    int placePiece(int slot, const std::string& slotRef, float stageX, float stageY);
    void removePiece(int slot);
    juce::String kitName() const;
    juce::String loadedPath() const { return loadedPath_; } // last kit / preset file loaded

    // --- mixer (message thread) ---
    const bfdengine::MixerState& mixer() const { return engine_.mixer(); }
    const std::vector<bfdengine::MicSource>& mics() const { return mics_; }
    void mutateMixer(const std::function<void(bfdengine::MixerState&)>& fn);
    void autoPatchAll(bool fresh); // fresh = forget every cable first

    // --- undo (message thread) ---
    // Every kit / mixer edit and every kit load is one step; a drag or a wheel
    // spin on one control is one step however many edits it makes.
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    void undo();
    void redo();

    // --- audition (message thread -> audio thread) ---
    void audition(int slot, float velocity);
    int noteForSlot(int slot) const; // first keymap note playing `slot`, -1 = none

    bfdengine::Engine& engine() { return engine_; }
    juce::String statusText() const { return status_; }
    bool statusIsProblem() const { return statusProblem_; }
    const bfdmodel::Preset& preset() const { return preset_; }

    // Streaming control (disk -> decode ring).
    void setStreamingEnabled(bool on);
    bool streamingEnabled() const;
    void setStreamLookaheadFrames(int frames);

    // UI preferences carried in the plugin state.
    bool darkTheme = false;
    bool showCables = true; // plugged cables drawn while the mixer faces backwards
    bool micsLocked = false; // stage mics ignore the mouse (pieces under them stay reachable)

private:
    static BusesProperties makeBuses();
    void refreshMics(bool patchNew);
    void setStatus(const juce::String& text, bool problem);
    void publishLibrary(bfdmodel::Library lib, std::vector<bfdmodel::KitFileEntry> files);

    struct Snapshot {
        bfdmodel::Kit kit;
        bfdengine::MixerState mixer;
        std::vector<bfdmodel::KeymapEntry> keymap; // explicit map (empty = automatic)
        bfdmodel::Preset preset;
        juce::String loadedPath;
    };
    Snapshot snapshot() const;
    void restore(Snapshot s);
    void recordUndo(); // call before an edit, under modelLock_

    bfdengine::Engine engine_;
    juce::StringArray libraryRoots_;
    std::vector<bfdmodel::KitFileEntry> kitFiles_;
    std::vector<bfdengine::MicSource> mics_;
    juce::String status_;
    juce::String loadedPath_;
    bool statusProblem_ = false;
    juce::CriticalSection modelLock_;
    bfdmodel::Preset preset_; // full .bfd3 document (when loaded)

    static constexpr size_t kUndoDepth = 64;
    std::vector<Snapshot> undo_, redo_;
    juce::int64 lastEditDown_ = -1; // mouse-down time of the gesture that made the last step
    double lastEditMs_ = 0.0;

    std::thread scanThread_;
    std::atomic<bool> scanning_{false}, quitScan_{false};
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);

    // UI audition events: single producer (message thread), single consumer (audio thread)
    std::array<bfdengine::NoteEvent, 64> uiEvents_{};
    std::atomic<uint32_t> uiHead_{0}, uiTail_{0};

    std::array<float*, bfdengine::Engine::kNumOuts> outPtrs_{};
    std::vector<float> scratchL_, scratchR_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OpenBfdProcessor)
};
