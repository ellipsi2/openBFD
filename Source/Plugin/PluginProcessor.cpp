#include "PluginProcessor.h"

#include <cmath>
#include "PluginEditor.h"

#include "../Model/Model.h"

namespace {
constexpr int kNumStrips = bfdengine::kNumStrips;

juce::String extensionOf(const juce::String& path) { return juce::File(path).getFileExtension().toLowerCase(); }

// BFD's conventional slot for the first piece of each kind (0 kick, 1 snare,
// 2 hats, 3 floor tom, 4/5 toms, 6/7 crashes, 8 ride, 9+ percussion).
std::vector<int> preferredSlots(bfdmodel::PieceKind kind) {
    using K = bfdmodel::PieceKind;
    switch (kind) {
        case K::Kick: return {0};
        case K::Snare: return {1};
        case K::Hihat: return {2};
        case K::FloorTom: return {3, 12};
        case K::Tom: return {5, 4, 13, 14};
        case K::Crash: return {6, 7};
        case K::Splash: return {16};
        case K::China: return {7};
        case K::Ride: return {8};
        case K::Perc: return {9};
    }
    return {};
}
} // namespace

OpenBfdProcessor::OpenBfdProcessor() : AudioProcessor(makeBuses()) {
    // compressed layers stay resident: a quarter of the RAM, 2 to 16 GB
    const auto ramMb = size_t(std::max(0, juce::SystemStats::getMemorySizeInMegabytes()));
    engine_.cache().setBudget(std::clamp<size_t>(ramMb / 4, 2048, 16384) << 20);
    engine_.setKit(bfdmodel::Kit{});
    engine_.setMixer(bfdengine::MixerState{});
}

OpenBfdProcessor::~OpenBfdProcessor() {
    alive_->store(false);
    quitScan_.store(true);
    if (scanThread_.joinable()) scanThread_.join();
}

OpenBfdProcessor::BusesProperties OpenBfdProcessor::makeBuses() {
    BusesProperties p;
    p = p.withOutput("Main", juce::AudioChannelSet::stereo(), true);
    for (int s = 1; s <= kNumStrips; ++s)
        p = p.withOutput("Ch " + juce::String(s), juce::AudioChannelSet::stereo(), false);
    return p;
}

bool OpenBfdProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo()) return false;
    for (int i = 1; i < layouts.outputBuses.size(); ++i)
        if (layouts.getChannelSet(false, i) != juce::AudioChannelSet::stereo() &&
            !layouts.getChannelSet(false, i).isDisabled())
            return false;
    return true;
}

void OpenBfdProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    engine_.setSampleRate(sampleRate);
    scratchL_.assign(size_t(std::max(samplesPerBlock, 1024)), 0.0f);
    scratchR_.assign(size_t(std::max(samplesPerBlock, 1024)), 0.0f);
}

void OpenBfdProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();

    std::array<bfdengine::NoteEvent, 256> events;
    int numEvents = 0;
    // UI auditions first (offset 0), then host MIDI in time order.
    for (uint32_t t = uiTail_.load(std::memory_order_relaxed); t != uiHead_.load(std::memory_order_acquire); ++t) {
        if (numEvents < 32) events[size_t(numEvents++)] = uiEvents_[t % uiEvents_.size()];
        uiTail_.store(t + 1, std::memory_order_release);
    }
    for (const auto metadata : midi) {
        const auto msg = metadata.getMessage();
        if (numEvents >= int(events.size())) break;
        const int offset = juce::jlimit(0, juce::jmax(0, n - 1), metadata.samplePosition);
        if (msg.isNoteOn()) events[size_t(numEvents++)] = {msg.getNoteNumber(), msg.getFloatVelocity(), true, offset};
    }
    midi.clear();

    if (int(scratchL_.size()) < n) { // hosts may exceed the announced block size
        scratchL_.assign(size_t(n), 0.0f);
        scratchR_.assign(size_t(n), 0.0f);
    }
    auto main = getBusBuffer(buffer, false, 0);
    outPtrs_[0] = main.getNumChannels() > 0 ? main.getWritePointer(0) : scratchL_.data();
    outPtrs_[1] = main.getNumChannels() > 1 ? main.getWritePointer(1) : scratchR_.data();
    const int buses = getBusCount(false);
    for (int s = 0; s < kNumStrips; ++s) {
        float* l = nullptr;
        float* r = nullptr;
        if (s + 1 < buses) {
            auto bus = getBusBuffer(buffer, false, s + 1);
            if (bus.getNumChannels() >= 2) {
                l = bus.getWritePointer(0);
                r = bus.getWritePointer(1);
            }
        }
        outPtrs_[size_t(2 + s * 2)] = l;
        outPtrs_[size_t(3 + s * 2)] = r;
    }
    // The engine is lock-free on the audio thread: kit and routing changes
    // are handed over without blocking and all disk I/O happens on its loader
    // thread. The master safety clip is transparent below -2 dBFS.
    engine_.process(events.data(), numEvents, outPtrs_.data(), n);
}

juce::AudioProcessorEditor* OpenBfdProcessor::createEditor() { return new OpenBfdEditor(*this); }

//==============================================================================
// libraries

void OpenBfdProcessor::setStatus(const juce::String& text, bool problem) {
    status_ = text;
    statusProblem_ = problem;
}

void OpenBfdProcessor::addLibraryRoot(const juce::String& root) {
    bfdmodel::Library lib;
    std::string err;
    if (!bfdmodel::scanLibrary(root.toStdString(), &lib, &err)) {
        setStatus(juce::String("Scan failed: ") + err.c_str(), true);
        return;
    }
    publishLibrary(std::move(lib), bfdmodel::listKitFiles(root.toStdString()));
}

void OpenBfdProcessor::publishLibrary(bfdmodel::Library lib, std::vector<bfdmodel::KitFileEntry> files) {
    const juce::ScopedLock lock(modelLock_);
    const juce::String root(lib.root);
    auto libs = engine_.libraries();
    libs.erase(std::remove_if(libs.begin(), libs.end(), [&](const bfdmodel::Library& l) { return l.root == lib.root; }),
               libs.end());
    libs.push_back(std::move(lib));
    engine_.setLibraries(std::move(libs));
    libraryRoots_.addIfNotAlreadyThere(root);
    kitFiles_.erase(std::remove_if(kitFiles_.begin(), kitFiles_.end(),
                                   [&](const bfdmodel::KitFileEntry& e) {
                                       return juce::String(e.path).startsWith(root + "/");
                                   }),
                    kitFiles_.end());
    kitFiles_.insert(kitFiles_.end(), files.begin(), files.end());
    refreshMics(true);
    sendChangeMessage();
}

void OpenBfdProcessor::scanLibrariesAsync(juce::StringArray roots) {
    if (roots.isEmpty())
        for (const auto& r : bfdmodel::discoverLibraryRoots()) roots.add(juce::String(r));
    if (roots.isEmpty()) {
        setStatus("No libraries", true);
        sendChangeMessage();
        return;
    }
    if (scanThread_.joinable()) {
        quitScan_.store(true);
        scanThread_.join();
    }
    quitScan_.store(false);
    scanning_.store(true);
    setStatus("Scanning", false);
    sendChangeMessage();
    scanThread_ = std::thread([this, roots, alive = alive_] {
        for (const auto& root : roots) {
            if (quitScan_.load()) break;
            auto lib = std::make_shared<bfdmodel::Library>();
            std::string err;
            if (!bfdmodel::scanLibrary(root.toStdString(), lib.get(), &err)) continue;
            auto files = std::make_shared<std::vector<bfdmodel::KitFileEntry>>(bfdmodel::listKitFiles(root.toStdString()));
            juce::MessageManager::callAsync([this, alive, lib, files] {
                if (alive->load()) publishLibrary(std::move(*lib), std::move(*files));
            });
        }
        juce::MessageManager::callAsync([this, alive] {
            if (!alive->load()) return;
            scanning_.store(false);
            {
                const juce::ScopedLock lock(modelLock_);
                refreshMics(true); // every library is in: drop cables of mics that are gone
            }
            setStatus({}, false);
            sendChangeMessage();
        });
    });
}

void OpenBfdProcessor::refreshMics(bool patchNew) {
    const auto previous = mics_;
    mics_ = bfdengine::listMics(engine_.kit(), engine_.pieceLookup());
    std::set<std::string> fresh;
    for (const auto& m : mics_) {
        const bool known = std::any_of(previous.begin(), previous.end(), [&](const auto& p) { return p.key == m.key; });
        if (!known) fresh.insert(m.key);
    }
    auto mx = engine_.mixer();
    // While libraries are still arriving a missing mic may only be unscanned.
    bfdengine::autoPatch(mics_, mx, !scanning_.load(), patchNew ? &fresh : nullptr);
    if (patchNew || !scanning_.load()) engine_.setMixer(std::move(mx));
}

//==============================================================================
// kit

bool OpenBfdProcessor::loadKitFile(const juce::String& path) {
    const std::string p = path.toStdString();
    const juce::String ext = extensionOf(path);
    std::string err;
    bfdmodel::Preset pre;
    if (!bfdmodel::loadPreset(p, &pre, &err)) {
        setStatus(juce::String("Load failed: ") + err.c_str(), true);
        sendChangeMessage();
        return false;
    }
    const bool program = ext == ".bfd3" || ext == ".bfd2";
    {
        const juce::ScopedLock lock(modelLock_);
        recordUndo();
        lastEditDown_ = -1; // a load never merges with the next edit
        if (pre.kit.name.empty()) pre.kit.name = juce::File(path).getFileNameWithoutExtension().toStdString();
        engine_.setKeymap(program ? bfdmodel::parseMidiMap(pre.midiMapXml) : std::vector<bfdmodel::KeymapEntry>{});
        engine_.setKit(pre.kit);
        // A new kit gets fresh cables; a program also brings its mixer.
        bfdengine::MixerState mx;
        mx.masterGain = engine_.mixer().masterGain;
        mics_ = bfdengine::listMics(engine_.kit(), engine_.pieceLookup());
        bfdengine::autoPatch(mics_, mx);
        if (program) bfdengine::importBfdMixer(pre.mixerXml, mics_, mx);
        engine_.setMixer(std::move(mx));
        preset_ = program ? std::move(pre) : bfdmodel::Preset{};
        if (!program) preset_.kit = engine_.kit();
        loadedPath_ = path;
    }
    setStatus({}, false);
    sendChangeMessage();
    return true;
}

bool OpenBfdProcessor::saveKitFile(const juce::String& path) {
    const juce::ScopedLock lock(modelLock_);
    const std::string p = path.toStdString();
    std::string err;
    if (extensionOf(path) == ".bfd3") {
        // Round-trip the full preset document, with the live kit state.
        bfdmodel::Preset out = preset_;
        out.kit = engine_.kit();
        out.valid = true;
        if (!bfdmodel::savePreset(p, out, &err)) {
            setStatus(juce::String("Save failed: ") + err.c_str(), true);
            return false;
        }
        preset_ = out;
    } else if (!bfdmodel::saveKit(p, engine_.kit(), &err)) {
        setStatus(juce::String("Save failed: ") + err.c_str(), true);
        return false;
    }
    setStatus({}, false);
    return true;
}

juce::String OpenBfdProcessor::kitName() const {
    const auto& n = engine_.kit().name;
    return n.empty() ? juce::String("Untitled kit") : juce::String(n);
}

void OpenBfdProcessor::mutateKit(const std::function<void(bfdmodel::Kit&)>& fn) {
    const juce::ScopedLock lock(modelLock_);
    recordUndo();
    auto kit = engine_.kit();
    std::vector<std::string> before;
    for (const auto& p : kit.pieces) before.push_back(p.slotRef);
    fn(kit);
    std::vector<std::string> after;
    for (const auto& p : kit.pieces) after.push_back(p.slotRef);
    const bool structural = before != after;
    if (structural) engine_.setKeymap({}); // an edited kit plays on the GM auto map
    engine_.setKit(std::move(kit));
    if (structural) {
        refreshMics(true);
        sendChangeMessage();
    }
}

int OpenBfdProcessor::placePiece(int slot, const std::string& slotRef, float stageX, float stageY) {
    const bfdmodel::Piece* piece = nullptr;
    for (const auto& lib : engine_.libraries())
        for (const auto& p : lib.pieces)
            if (bfdmodel::pieceSlotKey(lib, *p) == slotRef) piece = p.get();
    if (!piece) return -1;
    int placed = slot;
    mutateKit([&](bfdmodel::Kit& kit) {
        auto occupied = [&](int s) {
            return s < int(kit.pieces.size()) && !kit.pieces[size_t(s)].slotRef.empty();
        };
        if (placed < 0) {
            for (int s : preferredSlots(bfdmodel::pieceKind(*piece)))
                if (!occupied(s)) {
                    placed = s;
                    break;
                }
            for (int s = 9; placed < 0 && s < bfdmodel::Kit::kMaxSlots; ++s)
                if (!occupied(s)) placed = s;
            for (int s = 0; placed < 0 && s < bfdmodel::Kit::kMaxSlots; ++s)
                if (!occupied(s)) placed = s;
        }
        if (placed < 0) return;
        if (kit.pieces.size() <= size_t(placed)) kit.pieces.resize(size_t(placed) + 1);
        if (kit.slots.size() < kit.pieces.size()) kit.slots.resize(kit.pieces.size());
        bfdmodel::KitPieceState st;
        st.slotRef = slotRef;
        st.stageX = stageX;
        st.stageY = stageY;
        kit.pieces[size_t(placed)] = st;
        kit.slots[size_t(placed)] = slotRef;
        int used = 0;
        for (const auto& p : kit.pieces) used += p.slotRef.empty() ? 0 : 1;
        kit.size = used;
    });
    return placed;
}

void OpenBfdProcessor::removePiece(int slot) {
    mutateKit([&](bfdmodel::Kit& kit) {
        if (slot < 0 || size_t(slot) >= kit.pieces.size()) return;
        kit.pieces[size_t(slot)] = bfdmodel::KitPieceState{};
        if (size_t(slot) < kit.slots.size()) kit.slots[size_t(slot)].clear();
    });
}

//==============================================================================
// mixer

void OpenBfdProcessor::mutateMixer(const std::function<void(bfdengine::MixerState&)>& fn) {
    const juce::ScopedLock lock(modelLock_);
    recordUndo();
    auto mx = engine_.mixer();
    fn(mx);
    engine_.setMixer(std::move(mx));
    sendChangeMessage();
}

void OpenBfdProcessor::autoPatchAll(bool fresh) {
    mutateMixer([&](bfdengine::MixerState& mx) {
        if (fresh) mx.patch.clear();
        bfdengine::autoPatch(mics_, mx);
    });
}

//==============================================================================
// undo

OpenBfdProcessor::Snapshot OpenBfdProcessor::snapshot() const {
    return {engine_.kit(), engine_.mixer(), engine_.explicitKeymap(), preset_, loadedPath_};
}

void OpenBfdProcessor::recordUndo() {
    // One step per gesture: edits made while the same mouse press is held, or
    // in quick succession without a new press (a wheel spin), merge.
    const auto& mouse = juce::Desktop::getInstance().getMainMouseSource();
    const juce::int64 down = mouse.getLastMouseDownTime().toMilliseconds();
    const bool held = juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown();
    const double now = juce::Time::getMillisecondCounterHiRes();
    const bool merge = !undo_.empty() && down == lastEditDown_ && (held || now - lastEditMs_ < 500.0);
    lastEditDown_ = down;
    lastEditMs_ = now;
    if (merge) return;
    undo_.push_back(snapshot());
    if (undo_.size() > kUndoDepth) undo_.erase(undo_.begin());
    redo_.clear();
}

void OpenBfdProcessor::restore(Snapshot s) {
    {
        const juce::ScopedLock lock(modelLock_);
        engine_.setKeymap(std::move(s.keymap));
        engine_.setKit(std::move(s.kit));
        mics_ = bfdengine::listMics(engine_.kit(), engine_.pieceLookup());
        engine_.setMixer(std::move(s.mixer));
        preset_ = std::move(s.preset);
        loadedPath_ = s.loadedPath;
        lastEditDown_ = -1; // the next edit starts a fresh step
    }
    sendChangeMessage();
}

void OpenBfdProcessor::undo() {
    if (undo_.empty()) return;
    auto s = std::move(undo_.back());
    undo_.pop_back();
    redo_.push_back(snapshot());
    restore(std::move(s));
}

void OpenBfdProcessor::redo() {
    if (redo_.empty()) return;
    auto s = std::move(redo_.back());
    redo_.pop_back();
    undo_.push_back(snapshot());
    restore(std::move(s));
}

//==============================================================================

int OpenBfdProcessor::noteForSlot(int slot) const {
    int best = -1;
    for (const auto& k : engine_.keymap())
        if (k.slot == slot) {
            // prefer the "main" hit: Hit / ClosedT / Bow / Edge over rims and bells
            const bool main = k.articulation == "Hit" || k.articulation == "ClosedT" || k.articulation == "Bow" ||
                              k.articulation == "Edge";
            if (best < 0 || main) best = k.note;
            if (main) break;
        }
    return best;
}

void OpenBfdProcessor::audition(int slot, float velocity) {
    const int note = noteForSlot(slot);
    if (note < 0) return;
    const uint32_t head = uiHead_.load(std::memory_order_relaxed);
    if (head - uiTail_.load(std::memory_order_acquire) >= uiEvents_.size()) return; // full
    uiEvents_[head % uiEvents_.size()] = {note, juce::jlimit(0.0f, 1.0f, velocity), true, 0};
    uiHead_.store(head + 1, std::memory_order_release);
}

void OpenBfdProcessor::setStreamingEnabled(bool on) { engine_.setStreamingEnabled(on); }
bool OpenBfdProcessor::streamingEnabled() const { return engine_.streamingEnabled(); }
void OpenBfdProcessor::setStreamLookaheadFrames(int frames) { engine_.setStreamLookaheadFrames(frames); }

//==============================================================================
// state

void OpenBfdProcessor::getStateInformation(juce::MemoryBlock& destData) {
    juce::XmlElement root("OpenBfdState");
    root.setAttribute("version", 2);
    root.setAttribute("dark", darkTheme);
    root.setAttribute("cables", showCables);
    root.setAttribute("micsLocked", micsLocked);
    auto* libs = root.createNewChildElement("Libraries");
    for (const auto& r : libraryRoots_) libs->createNewChildElement("Root")->setAttribute("path", r);
    const juce::ScopedLock lock(modelLock_);
    root.createNewChildElement("Kit")->addTextElement(juce::String(bfdmodel::kitToXml(engine_.kit(), true)));
    root.createNewChildElement("Mixer")->addTextElement(juce::String(bfdengine::mixerToXml(engine_.mixer())));
    if (!preset_.midiMapXml.empty() && !engine_.keymap().empty()) {
        // explicit map from a preset: keep the resolved entries
        auto* km = root.createNewChildElement("Keymap");
        for (const auto& k : engine_.keymap()) {
            auto* e = km->createNewChildElement("Key");
            e->setAttribute("note", k.note);
            e->setAttribute("slot", k.slot);
            e->setAttribute("artic", juce::String(k.articulation));
        }
    }
    copyXmlToBinary(root, destData);
}

void OpenBfdProcessor::setStateInformation(const void* data, int sizeInBytes) {
    auto root = getXmlFromBinary(data, sizeInBytes);
    if (!root || !root->hasTagName("OpenBfdState")) return;
    darkTheme = root->getBoolAttribute("dark", darkTheme);
    showCables = root->getBoolAttribute("cables", showCables);
    micsLocked = root->getBoolAttribute("micsLocked", micsLocked);
    juce::StringArray roots;
    if (auto* libs = root->getChildByName("Libraries")) {
        for (auto* r : libs->getChildWithTagNameIterator("Root")) roots.addIfNotAlreadyThere(r->getStringAttribute("path"));
        for (int i = 0; i < libs->getNumAttributes(); ++i) // version 1 kept roots as attributes
            if (libs->getAttributeValue(i).isNotEmpty()) roots.addIfNotAlreadyThere(libs->getAttributeValue(i));
    }
    {
        const juce::ScopedLock lock(modelLock_);
        std::vector<bfdmodel::KeymapEntry> keymap;
        if (auto* km = root->getChildByName("Keymap"))
            for (auto* e : km->getChildWithTagNameIterator("Key"))
                keymap.push_back({e->getIntAttribute("note", -1), e->getIntAttribute("slot", -1),
                                  e->getStringAttribute("artic").toStdString(), -1});
        engine_.setKeymap(std::move(keymap));
        bfdmodel::Kit kit;
        if (auto* k = root->getChildByName("Kit"))
            bfdmodel::kitFromXml(k->getAllSubText().toStdString(), &kit);
        engine_.setKit(std::move(kit));
        bfdengine::MixerState mx;
        if (auto* m = root->getChildByName("Mixer"))
            bfdengine::mixerFromXml(m->getAllSubText().toStdString(), &mx);
        engine_.setMixer(std::move(mx));
        mics_.clear();
        preset_ = bfdmodel::Preset{};
        preset_.kit = engine_.kit();
        undo_.clear();
        redo_.clear();
    }
    // libraries rescan in the background; the kit resolves as they arrive
    scanLibrariesAsync(roots);
}

// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new OpenBfdProcessor(); }
