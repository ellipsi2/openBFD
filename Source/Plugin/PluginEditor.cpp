#include "PluginEditor.h"

#include "UI/Motion.h"
#include "UI/Widgets.h"

using namespace openbfd::ui;

namespace {
constexpr int kHeaderH = 46, kFooterH = 30, kBrowserW = 280, kMixerW = 620;
} // namespace

OpenBfdEditor::OpenBfdEditor(OpenBfdProcessor& p) : juce::AudioProcessorEditor(p), proc_(p) {
    Theme::get().setMode(proc_.darkTheme ? ThemeMode::dark : ThemeMode::light);
    lnf_.refreshColours();
    setLookAndFeel(&lnf_);

    addAndMakeVisible(browser_);
    addAndMakeVisible(stage_);
    addAndMakeVisible(mixer_);
    addAndMakeVisible(cables_);
    for (auto* b : {&open_, &save_, &theme_}) addAndMakeVisible(*b);
    open_.setTooltip("Open a kit or preset");
    save_.setTooltip("Save the kit (.bfd3kit) or preset (.bfd3)");
    theme_.setTooltip("Light / dark");
    theme_.setButtonText(proc_.darkTheme ? "LIGHT" : "DARK");
    open_.onClick = [this] { chooseKitFile(false); };
    save_.onClick = [this] { chooseKitFile(true); };
    theme_.onClick = [this] { applyTheme(!proc_.darkTheme); };

    proc_.addChangeListener(this);
    setResizable(true, true);
    setResizeLimits(1180, 720, 2600, 1600);
    setSize(1500, 880);
    syncNow();
    if (proc_.libraries().empty() && !proc_.scanning()) proc_.scanLibrariesAsync();
    startTimerHz(30);
}

OpenBfdEditor::~OpenBfdEditor() {
    proc_.removeChangeListener(this);
    setLookAndFeel(nullptr);
}

void OpenBfdEditor::syncNow() {
    browser_.refresh();
    stage_.refresh();
    mixer_.sync();
    updateCables();
    repaint();
}

void OpenBfdEditor::changeListenerCallback(juce::ChangeBroadcaster*) { syncNow(); }

void OpenBfdEditor::applyTheme(bool dark) {
    proc_.darkTheme = dark;
    Theme::get().setMode(dark ? ThemeMode::dark : ThemeMode::light);
    lnf_.refreshColours();
    theme_.setButtonText(dark ? "LIGHT" : "DARK");
    sendLookAndFeelChange();
    syncNow();
}

void OpenBfdEditor::chooseKitFile(bool save) {
    const auto start = proc_.loadedPath().isNotEmpty() ? juce::File(proc_.loadedPath()) : juce::File();
    chooser_ = std::make_unique<juce::FileChooser>(save ? "Save kit" : "Open kit or preset", start,
                                                   "*.bfd3kit;*.bfd2kit;*.bfd3;*.bfd2");
    const int flags = save ? juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting
                           : juce::FileBrowserComponent::openMode;
    chooser_->launchAsync(flags | juce::FileBrowserComponent::canSelectFiles, [this, save](const juce::FileChooser& fc) {
        const auto f = fc.getResult();
        if (f == juce::File()) return;
        if (save) proc_.saveKitFile(f.getFullPathName());
        else proc_.loadKitFile(f.getFullPathName());
        syncNow();
    });
}

//==============================================================================
// layout + chrome

void OpenBfdEditor::resized() {
    auto r = getLocalBounds();
    auto header = r.removeFromTop(kHeaderH);
    r.removeFromBottom(kFooterH);
    browser_.setBounds(r.removeFromLeft(kBrowserW));
    r.removeFromLeft(1);
    const int mixerW = std::min(kMixerW, std::max(420, r.getWidth() / 2));
    mixer_.setBounds(r.removeFromRight(mixerW));
    r.removeFromRight(1);
    stage_.setBounds(r);
    cables_.setBounds(getLocalBounds());

    auto h = header.reduced(14, 12);
    theme_.setBounds(h.removeFromRight(64));
    h.removeFromRight(6);
    save_.setBounds(h.removeFromRight(56));
    h.removeFromRight(6);
    open_.setBounds(h.removeFromRight(56));
    updateCables();
}

void OpenBfdEditor::paint(juce::Graphics& g) {
    const auto& p = pal();
    g.fillAll(p.canvas);
    // header: sheet with a hairline under it
    auto header = getLocalBounds().removeFromTop(kHeaderH);
    g.setColour(p.panel);
    g.fillRect(header);
    g.setColour(p.ruleStrong);
    g.fillRect(header.removeFromBottom(1));
    auto h = header.reduced(14, 0);
    // breadcrumb: openbfd / <kit>
    g.setFont(Theme::get().mono(12.0f));
    g.setColour(p.textDim);
    const juce::String root = "openbfd  /  ";
    const int rw = juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), root);
    g.drawText(root, h.removeFromLeft(rw), juce::Justification::centredLeft, false);
    g.setColour(p.text);
    g.setFont(Theme::get().mono(12.0f, true));
    g.drawText(proc_.kitName(), h.removeFromLeft(320), juce::Justification::centredLeft, true);
    // readouts, right-aligned before the buttons
    auto readouts = header.withTrimmedRight(14 + 64 + 6 + 56 + 6 + 56 + 24);
    g.setFont(Theme::get().mono(10.5f));
    auto readout = [&](const juce::String& label, const juce::String& value) {
        const int vw = juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), value);
        g.setColour(p.text);
        g.drawText(value, readouts.removeFromRight(vw), juce::Justification::centredRight, false);
        readouts.removeFromRight(6);
        const int lw = juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), label);
        g.setColour(p.textDim);
        g.drawText(label, readouts.removeFromRight(lw), juce::Justification::centredRight, false);
        readouts.removeFromRight(22);
    };
    readout("Voices:", juce::String(voices_).paddedLeft('0', 2));
    readout("Layers:", juce::String(layersReady_) + "/" + juce::String(layersTotal_));
    readout("SR:", juce::String(proc_.getSampleRate() > 0 ? proc_.getSampleRate() / 1000.0 : 48.0, 1) + "k");

    // panel separators
    g.setColour(p.ruleStrong);
    g.fillRect(browser_.getRight(), browser_.getY(), 1, browser_.getHeight());
    g.fillRect(mixer_.getX() - 1, mixer_.getY(), 1, mixer_.getHeight());

    // footer: hint on the left, status only when there is something to say
    auto foot = getLocalBounds().removeFromBottom(kFooterH);
    g.setColour(p.panel);
    g.fillRect(foot);
    g.setColour(p.ruleStrong);
    g.fillRect(foot.removeFromTop(1));
    auto f = foot.reduced(14, 0).withTrimmedRight(14); // clear of the resize corner
    juce::String status = proc_.statusText();
    bool problem = proc_.statusIsProblem(), working = false;
    if (status.isEmpty() && layersTotal_ > 0 && layersReady_ < layersTotal_) {
        status = "Loading " + juce::String(layersReady_ * 100 / std::max(1, layersTotal_)) + "%";
        working = true;
    } else if (status.isNotEmpty() && !problem) {
        working = true;
    }
    if (status.isNotEmpty()) {
        g.setFont(Theme::get().mono(10.5f));
        const int w = juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), status.toUpperCase()) + 26;
        draw::statusChip(g, f.removeFromRight(std::min(w, 360)).withSizeKeepingCentre(std::min(w, 360), 20), status,
                         problem, working);
        f.removeFromRight(14);
    }
    g.setColour(p.textDim);
    g.setFont(Theme::get().mono(10.5f));
    g.drawText(hint_, f, juce::Justification::centredLeft, true);
}

void OpenBfdEditor::setHint(const juce::String& text) {
    if (text == hint_) return;
    hint_ = text;
    repaint(getLocalBounds().removeFromBottom(kFooterH));
}

//==============================================================================
// cables

bfdengine::Jack OpenBfdEditor::jackUnder(juce::Point<float> editorPos) const {
    if (!mixer_.showingBack()) return {};
    return mixer_.jackAt(mixer_.getLocalPoint(this, editorPos));
}

void OpenBfdEditor::updateCables() {
    std::vector<CableLayer::Ends> ends;
    for (const auto& [key, jack] : proc_.mixer().patch) {
        if (!jack.valid()) continue;
        juce::Point<float> anchor;
        if (!stage_.micPoint(key, anchor)) continue;
        ends.push_back({key, getLocalPoint(&stage_, anchor), getLocalPoint(&mixer_, mixer_.socketCentre(jack)),
                        mixer_.jackInView(jack)});
    }
    cables_.setPlugged(ends);
    cables_.setReveal(proc_.showCables ? mixer_.backAmount() : 0.0f);
}

void OpenBfdEditor::cableBegin(const std::string& micKey, juce::Component& source, juce::Point<float> plugAt) {
    ++flipBackToken_; // a pending turn back to the front is off
    cableKey_ = micKey;
    hotJack_ = {};
    mixer_.flipTo(true);
    juce::Point<float> anchor;
    if (!stage_.micPoint(micKey, anchor)) anchor = getLocalPoint(&source, plugAt);
    else anchor = getLocalPoint(&stage_, anchor);
    cables_.beginDrag(micKey, anchor, getLocalPoint(&source, plugAt));
    const auto* mic = findMic(proc_.mics(), micKey);
    setHint((mic ? micCaption(*mic, nullptr) : juce::String(micKey)) + arrowText() + "drop on a jack");
    stage_.repaint();
}

void OpenBfdEditor::cableMove(juce::Component& source, juce::Point<float> pos) {
    if (cableKey_.empty()) return;
    const auto at = getLocalPoint(&source, pos);
    hotJack_ = jackUnder(at);
    mixer_.setHotJack(hotJack_);
    juce::Point<float> anchor;
    if (stage_.micPoint(cableKey_, anchor)) anchor = getLocalPoint(&stage_, anchor);
    else anchor = at;
    auto plug = at;
    if (hotJack_.valid()) plug = getLocalPoint(&mixer_, mixer_.socketCentre(hotJack_));
    cables_.dragTo(anchor, plug, hotJack_.valid());
    const auto* mic = findMic(proc_.mics(), cableKey_);
    const auto name = mic ? micCaption(*mic, nullptr) : juce::String(cableKey_);
    if (hotJack_.valid()) {
        const auto* occupant = proc_.mixer().micAt(hotJack_.strip, hotJack_.side);
        const auto* other = occupant && *occupant != cableKey_ ? findMic(proc_.mics(), *occupant) : nullptr;
        setHint(name + arrowText() + "CH " + jackCode(hotJack_) + (other ? "  replaces " + micCaption(*other, nullptr) : juce::String()));
    } else {
        setHint(name + arrowText() + "drop on a jack");
    }
}

void OpenBfdEditor::cableEnd(juce::Component& source, juce::Point<float> pos) {
    if (cableKey_.empty()) return;
    const auto key = std::exchange(cableKey_, {});
    const auto jack = jackUnder(getLocalPoint(&source, pos));
    mixer_.setHotJack({});
    if (jack.valid()) {
        cables_.endDrag(true);
        proc_.mutateMixer([&](bfdengine::MixerState& mx) { mx.plug(key, jack); });
    } else {
        cables_.endDrag(false);
        if (proc_.mixer().jackOf(key).valid())
            proc_.mutateMixer([&](bfdengine::MixerState& mx) { mx.unplug(key); });
    }
    setHint({});
    syncNow();
    // turn the mixer back around shortly after, unless the rear is pinned
    const int token = ++flipBackToken_;
    juce::Component::SafePointer<OpenBfdEditor> safe(this);
    juce::Timer::callAfterDelay(650, [safe, token] {
        if (safe && safe->flipBackToken_ == token && safe->cableKey_.empty()) safe->mixer_.flipTo(false);
    });
}

//==============================================================================

void OpenBfdEditor::timerCallback() {
    auto& engine = proc_.engine();
    mixer_.updateMeters(engine);
    for (int s = 0; s < bfdmodel::Kit::kMaxSlots; ++s)
        if (const float v = engine.takeSlotHit(s); v > 0.0f) stage_.hit(s, v);
    const int voices = engine.activeVoices(), ready = engine.layersReady(), total = engine.layersTotal();
    if (voices != voices_ || ready != layersReady_ || total != layersTotal_) {
        voices_ = voices;
        layersReady_ = ready;
        layersTotal_ = total;
        repaint(getLocalBounds().removeFromTop(kHeaderH));
        repaint(getLocalBounds().removeFromBottom(kFooterH));
    }
    // cables follow the flip and any piece being dragged
    updateCables();
}
