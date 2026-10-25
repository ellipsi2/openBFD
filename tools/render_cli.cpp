// openbfd-render: drive OpenBfdProcessor like a host and write the Mix bus.
// Usage: openbfd-render <libraryRoot> <kitFile> <out.wav> [sampleRate] [blockSize] [seconds]
// Plays a fixed groove (kick/snare/hihat/crash), reports per-block cost
// against the real-time budget and block-boundary continuity of the output.
#include <JuceHeader.h>

#include <chrono>
#include <cmath>
#include <cstdio>

#include "Plugin/PluginProcessor.h"

int main(int argc, char** argv) {
    if (argc < 4) {
        std::printf("usage: openbfd-render <libraryRoot> <kit> <out.wav> [sr] [block] [sec]\n");
        return 2;
    }
    juce::ScopedJuceInitialiser_GUI juceInit;
    const double sr = argc > 4 ? std::atof(argv[4]) : 48000.0;
    const int bs = argc > 5 ? std::atoi(argv[5]) : 256;
    const double seconds = argc > 6 ? std::atof(argv[6]) : 8.0;

    OpenBfdProcessor proc;
    proc.addLibraryRoot(argv[1]);
    if (!proc.loadKitFile(argv[2])) {
        std::printf("%s\n", proc.statusText().toRawUTF8());
        return 1;
    }
    proc.engine().waitUntilLoaded(120000);
    auto& engine = proc.engine();
    std::printf("layers %d/%d (tried %d)%s\n", engine.layersReady(), engine.layersTotal(), engine.layersDone(),
                engine.cacheFull() ? ", cache full" : "");
    proc.setRateAndBufferSizeDetails(sr, bs);
    proc.prepareToPlay(sr, bs);

    const int outCh = proc.getTotalNumOutputChannels();
    juce::AudioBuffer<float> buffer(outCh, bs);
    const int total = int(seconds * sr);
    const int step = int(sr / 8); // 8th notes at 120 bpm... ish
    juce::AudioBuffer<float> mix(2, total);
    double worst = 0.0;
    int over = 0;
    const double budgetMs = 1000.0 * bs / sr;
    juce::Random rng(1);

    for (int pos = 0; pos + bs <= total; pos += bs) {
        juce::MidiBuffer midi;
        for (int s = pos; s < pos + bs; ++s) {
            if (s % step) continue;
            const int k = s / step, o = s - pos;
            auto on = [&](int note) {
                midi.addEvent(juce::MidiMessage::noteOn(10, note, juce::uint8(40 + rng.nextInt(87))), o);
            };
            on(42);
            if (k % 4 == 0) on(36);
            if (k % 4 == 2) on(38);
            if (k % 16 == 0) on(49);
        }
        buffer.clear();
        const auto t = std::chrono::steady_clock::now();
        proc.processBlock(buffer, midi);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
        worst = std::max(worst, ms);
        if (ms > budgetMs) ++over;
        auto main = proc.getBusBuffer(buffer, false, 0);
        mix.copyFrom(0, pos, main, 0, 0, bs);
        mix.copyFrom(1, pos, main, 1, 0, bs);
    }

    // continuity: step size at block boundaries vs everywhere else
    double edge = 0, inner = 0;
    long ne = 0, ni = 0;
    float peak = 0;
    long hot = 0; // samples above 0 dBFS (would clip at a fixed-point output)
    double power = 0;
    for (int c = 0; c < 2; ++c) {
        const float* x = mix.getReadPointer(c);
        for (int i = 1; i < total; ++i) {
            const double d = std::abs(x[i] - x[i - 1]);
            peak = std::max(peak, std::abs(x[i]));
            if (std::abs(x[i]) > 1.0f) ++hot;
            power += double(x[i]) * x[i];
            if (i % bs == 0) { edge += d; ++ne; } else { inner += d; ++ni; }
        }
    }
    std::printf("blocks over budget (%.2f ms): %d, worst %.2f ms\n", budgetMs, over, worst);
    std::printf("mix peak %.1f dBFS, boundary/inner step ratio %.2f (1.0 = seamless)\n",
                20 * std::log10(std::max(peak, 1e-9f)), (edge / std::max(1L, ne)) / (inner / std::max(1L, ni)));
    std::printf("mix rms %.1f dBFS, samples over 0 dBFS %ld (%.3f%%)\n",
                10 * std::log10(std::max(power / std::max(1, 2 * total), 1e-18)), hot,
                100.0 * double(hot) / std::max(1, 2 * total));

    juce::WavAudioFormat wav;
    juce::File outFile(argv[3]);
    outFile.deleteFile();
    std::unique_ptr<juce::AudioFormatWriter> w(
        wav.createWriterFor(new juce::FileOutputStream(outFile), sr, 2, 24, {}, 0));
    if (w) w->writeFromAudioSampleBuffer(mix, 0, total);
    return 0;
}
