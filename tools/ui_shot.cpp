// ui_shot: render the openbfd editor to a PNG without a DAW.
// Usage: openbfd-ui-shot <out.png> [light|dark] [kit-or-preset] [front|rear|cable|bare] [WxH]
//   Libraries come from BFD3's DataPaths.xml. "rear" turns the mixer around,
//   "cable" photographs a cable in the hand on its way to a jack, "bare" the
//   rear with the plugged cables hidden. OPENBFD_SHOT_WAIT=<seconds> waits for
//   the kit to finish loading.
#include <JuceHeader.h>

#include "Plugin/PluginEditor.h"
#include "Plugin/PluginProcessor.h"

namespace {
void pump(int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }
} // namespace

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI juceInit;
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <out.png> [light|dark] [kit] [front|rear|cable|bare] [WxH]\n", argv[0]);
        return 2;
    }
    const bool dark = argc > 2 && juce::String(argv[2]) == "dark";
    const juce::String kit = argc > 3 ? juce::String::fromUTF8(argv[3]) : juce::String();
    const juce::String view = argc > 4 ? juce::String(argv[4]) : juce::String("front");
    const juce::String size = argc > 5 ? juce::String(argv[5]) : juce::String("1500x880");

    OpenBfdProcessor proc;
    proc.darkTheme = dark;
    proc.showCables = view != "bare";
    for (const auto& root : bfdmodel::discoverLibraryRoots()) proc.addLibraryRoot(juce::String(root));
    if (kit.isNotEmpty() && kit != "-" && !proc.loadKitFile(kit)) std::fprintf(stderr, "kit failed: %s\n", argv[3]);

    std::unique_ptr<juce::AudioProcessorEditor> base(proc.createEditor());
    auto* ed = dynamic_cast<OpenBfdEditor*>(base.get());
    ed->setSize(size.upToFirstOccurrenceOf("x", false, false).getIntValue(),
                size.fromFirstOccurrenceOf("x", false, false).getIntValue());
    ed->syncNow();
    pump(200);
    // OPENBFD_SHOT_WAIT=<seconds>: wait for the preload to finish first
    const int waitMs = juce::SystemStats::getEnvironmentVariable("OPENBFD_SHOT_WAIT", "0").getIntValue() * 1000;
    auto& engine = proc.engine();
    for (int waited = 0; waited < waitMs && engine.layersDone() < engine.layersTotal() && !engine.cacheFull(); waited += 250) pump(250);

    if (view == "rear" || view == "bare") {
        ed->mixerPanel().flipTo(true);
        pump(2500); // let the cables settle
    } else if (view == "cable") {
        // pull the first ambient mic's cable towards a free jack on the rear
        std::string key;
        for (const auto& m : proc.mics())
            if (m.kind == bfdengine::MicKind::Ambient) {
                key = m.key;
                break;
            }
        if (key.empty() && !proc.mics().empty()) key = proc.mics().front().key;
        auto& stage = ed->stage();
        juce::Point<float> from;
        if (!key.empty() && stage.micPoint(key, from)) {
            ed->cableBegin(key, stage, from);
            pump(500);
            auto& mixer = ed->mixerPanel();
            const auto target = stage.getLocalPoint(&mixer, mixer.socketCentre({4, 1}));
            for (int i = 1; i <= 40; ++i) {
                const float t = float(i) / 40.0f;
                const auto at = from + (target - from) * t + juce::Point<float>(0.0f, std::sin(t * 3.14159f) * 120.0f);
                ed->cableMove(stage, at);
                pump(16);
            }
            pump(250);
        }
    }

    auto img = ed->createComponentSnapshot(ed->getLocalBounds(), true, 2.0f);
    juce::File out{juce::String::fromUTF8(argv[1])};
    out.deleteFile();
    juce::FileOutputStream stream{out};
    juce::PNGImageFormat png;
    if (!stream.openedOk() || !png.writeImageToStream(img, stream)) {
        std::fprintf(stderr, "cannot write %s\n", argv[1]);
        return 1;
    }
    std::printf("wrote %s (%dx%d)\n", argv[1], img.getWidth(), img.getHeight());
    std::printf("layers %d/%d (tried %d), cache %zu/%zu MB\n", engine.layersReady(), engine.layersTotal(), engine.layersDone(),
                engine.cache().bytesUsed() >> 20, engine.cache().budget() >> 20);
    return 0;
}
