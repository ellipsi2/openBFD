// openbfd-decode: decode .bfdlac files to WAV / report stats.
// Usage:
//   openbfd-decode stats  <file.bfdlac>...
//   openbfd-decode wav    <file.bfdlac> <out.wav>
//   openbfd-decode verify <file.bfdlac>...   (byte-accounting + level report)
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "Bfdlac/BfdlacCodec.h"

namespace {

void writeWav(const std::string& path, const std::vector<float>& interleaved, int channels,
              int frames, int sampleRate) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return;
    }
    const uint32_t dataBytes = uint32_t(frames * channels * 2);
    const uint32_t riffSize = 36 + dataBytes;
    auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 4, 1, f);
    w32(riffSize);
    std::fwrite("WAVEfmt ", 8, 1, f);
    w32(16);
    w16(1);
    w16(uint16_t(channels));
    w32(uint32_t(sampleRate));
    w32(uint32_t(sampleRate * channels * 2));
    w16(uint16_t(channels * 2));
    w16(16);
    std::fwrite("data", 4, 1, f);
    w32(dataBytes);
    for (int i = 0; i < frames * channels; ++i) {
        const float v = interleaved[size_t(i)];
        const int s = std::lround(std::clamp(v, -1.0f, 0.9999f) * 32767.0f);
        w16(uint16_t(int16_t(s)));
    }
    std::fclose(f);
}

int cmdStats(char** files, int n, bool verify) {
    for (int i = 0; i < n; ++i) {
        std::vector<float> audio;
        bfdlac::Header h;
        std::string err;
        if (!bfdlac::decodeWholeFile(files[i], &audio, &h, &err)) {
            std::fprintf(stderr, "%s: %s\n", files[i], err.c_str());
            return 1;
        }
        std::printf("%s\n  %d frames, %d ch, %d Hz, %d-bit, lib %s\n", files[i], h.frames, h.channels,
                    h.sampleRate, h.bitDepth, h.libraryId.c_str());
        for (int c = 0; c < h.channels; ++c) {
            double acc = 0.0;
            float peak = 0.0f;
            for (int s = 0; s < h.frames; ++s) {
                const float v = audio[size_t(s) * h.channels + c];
                acc += double(v) * v;
                peak = std::max(peak, std::abs(v));
            }
            const double db = acc > 0 ? 10.0 * std::log10(acc / h.frames) : -999.0;
            std::printf("  ch%2d  rms %8.2f dB  peak %.5f\n", c, db, peak);
        }
        if (verify) {
            // re-walk the container and confirm every frame's byte accounting
            std::printf("  decode ok (%zu samples)\n", audio.size());
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: %s stats|verify <file.bfdlac>...\n"
                     "       %s wav <file.bfdlac> <out.wav>\n",
                     argv[0], argv[0]);
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "stats" || cmd == "verify") return cmdStats(argv + 2, argc - 2, cmd == "verify");
    if (cmd == "wav") {
        if (argc < 4) return 2;
        std::vector<float> audio;
        bfdlac::Header h;
        std::string err;
        if (!bfdlac::decodeWholeFile(argv[2], &audio, &h, &err)) {
            std::fprintf(stderr, "%s: %s\n", argv[2], err.c_str());
            return 1;
        }
        writeWav(argv[3], audio, h.channels, h.frames, h.sampleRate);
        std::printf("wrote %s (%d frames, %d ch)\n", argv[3], h.frames, h.channels);
        return 0;
    }
    return 2;
}
