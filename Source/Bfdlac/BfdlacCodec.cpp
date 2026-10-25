#include "BfdlacCodec.h"
#include "BfdlacTables.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace bfdlac {
namespace {

constexpr int kSubFrameSamples = 64;

uint32_t readBe32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// Residual fields are packed LSB-first within each byte: the field occupies
// bit positions [bitPos, bitPos+bits) of a little-endian 32-bit window at the
// current byte pointer, sign-extended from the top of the field.
class BitReader {
public:
    explicit BitReader(const uint8_t* data) : p_(data) {}

    int readSigned(int bits) {
        if (bits <= 0) return 0;
        uint32_t window;
        std::memcpy(&window, p_, 4); // little-endian host (all supported targets)
        const int bitPos = 8 - bitsLeft_;
        uint32_t v = window << (32 - bits - bitPos);
        v &= 0xFFFFFFFFu << (32 - bits);
        const int result = int32_t(v) >> (32 - bits); // arithmetic shift
        const int used = bitPos + bits;
        p_ += used >> 3;
        bitsLeft_ = (used & 7) ? 8 - (used & 7) : 8;
        return result;
    }

    void alignByte() {
        if (bitsLeft_ >= 1 && bitsLeft_ <= 7) (void) readSigned(bitsLeft_);
    }

    int32_t readUnsignedLE(int nbytes) {
        uint32_t v = 0;
        for (int i = 0; i < nbytes; ++i) v |= uint32_t(p_[i]) << (8 * i);
        p_ += nbytes;
        return int32_t(v);
    }

    int32_t readSignedLE(int nbytes) {
        const uint32_t v = uint32_t(readUnsignedLE(nbytes));
        const int bits = nbytes * 8;
        const uint32_t sign = 1u << (bits - 1);
        return int32_t((v ^ sign) - sign);
    }

    const uint8_t* position() const { return p_; }

private:
    const uint8_t* p_;
    int bitsLeft_ = 8;
};

int signExtendBits(uint32_t v, int bits) {
    const uint32_t sign = 1u << (bits - 1);
    return int32_t((v ^ sign) - sign);
}

} // namespace

int File::decodeBlock(const uint8_t* payload, uint8_t scheme, int bitDepth,
                      int version, int32_t* out) {
    const int alg = version >= 2 ? (scheme >> 4) & 0xF : (scheme >> 2) & 0xF;
    const int err = version >= 2 ? scheme & 0xF : scheme & 3;
    const int bits = tables::kBitDepthTable[err];
    const int bps = bitDepth >= 20 ? 3 : 2;
    const int n = kSubFrameSamples;

    BitReader r(payload);
    std::memset(out, 0, sizeof(int32_t) * n);

    switch (alg) {
        case 0: // raw PCM
            for (int i = 0; i < n; ++i) out[i] = r.readSignedLE(bps);
            break;
        case 1: { // constant
            const int32_t v = r.readSignedLE(bps);
            for (int i = 0; i < n; ++i) out[i] = v;
            break;
        }
        case 2: { // seed + per-sample residual
            const int32_t s = r.readSignedLE(bps);
            for (int i = 0; i < n; ++i) out[i] = s + r.readSigned(bits);
            break;
        }
        case 3: { // linear gradient: s0 slope, s1 intercept
            const int32_t s0 = r.readSignedLE(bps);
            const int32_t s1 = r.readSignedLE(bps);
            for (int i = 0; i < n; ++i) out[i] = s1 + s0 * i + r.readSigned(bits);
            break;
        }
        case 4: // zero fill
            break;
        case 5: { // second-order delta
            const int32_t a = r.readSignedLE(bps);
            const int32_t b = r.readSignedLE(bps);
            out[0] = a;
            out[1] = b;
            for (int i = 2; i < n; ++i)
                out[i] = 2 * out[i - 1] - out[i - 2] + r.readSigned(bits);
            break;
        }
        case 6: { // seed every 4 samples
            for (int g = 0; g < n / 4; ++g) {
                const int32_t s = r.readSignedLE(bps);
                out[g * 4] = s;
                for (int k = 1; k < 4; ++k) out[g * 4 + k] = s + r.readSigned(bits);
            }
            break;
        }
        case 7: { // first-order delta
            int32_t acc = r.readSignedLE(bps);
            out[0] = acc;
            for (int i = 1; i < n; ++i) out[i] = acc += r.readSigned(bits);
            break;
        }
        case 8: { // seed every 8 samples
            for (int g = 0; g < n / 8; ++g) {
                const int32_t s = r.readSignedLE(bps);
                out[g * 8] = s;
                for (int k = 1; k < 8; ++k) out[g * 8 + k] = s + r.readSigned(bits);
            }
            break;
        }
        case 9: { // half-slope extrapolation: out[i-1] + (out[i-1]-out[i-2])/2
            const int32_t a = r.readSignedLE(bps);
            const int32_t b = r.readSignedLE(bps);
            out[0] = a;
            out[1] = b;
            for (int i = 2; i < n; ++i)
                out[i] = out[i - 1] + ((out[i - 1] - out[i - 2]) >> 1) + r.readSigned(bits);
            break;
        }
        case 10: { // quadratic: s0*i^2 + s1*i + s2
            const int32_t s0 = r.readSignedLE(bps);
            const int32_t s1 = r.readSignedLE(bps);
            const int32_t s2 = r.readSignedLE(bps);
            for (int i = 0; i < n; ++i)
                out[i] = s2 + (s1 + s0 * i) * i + r.readSigned(bits);
            break;
        }
        case 11: { // third-order predictor (SHORTEN-style)
            const int32_t a = r.readSignedLE(bps);
            const int32_t b = r.readSignedLE(bps);
            const int32_t c = r.readSignedLE(bps);
            out[0] = a;
            out[1] = b;
            out[2] = c;
            for (int i = 3; i < n; ++i)
                out[i] = out[i - 3] + 3 * (out[i - 1] - out[i - 2]) + r.readSigned(bits);
            break;
        }
        case 12: { // delta, 16-bit seed
            int32_t acc = r.readSignedLE(2);
            out[0] = acc;
            for (int i = 1; i < n; ++i) out[i] = acc += r.readSigned(bits);
            break;
        }
        case 13: { // delta2, 16-bit seeds
            const int32_t a = r.readSignedLE(2);
            const int32_t b = r.readSignedLE(2);
            out[0] = a;
            out[1] = b;
            for (int i = 2; i < n; ++i)
                out[i] = 2 * out[i - 1] - out[i - 2] + r.readSigned(bits);
            break;
        }
        case 14: { // delta2, 8-bit seeds
            const int32_t a = r.readSignedLE(1);
            const int32_t b = r.readSignedLE(1);
            out[0] = a;
            out[1] = b;
            for (int i = 2; i < n; ++i)
                out[i] = 2 * out[i - 1] - out[i - 2] + r.readSigned(bits);
            break;
        }
        case 15: { // delta, 8-bit seed
            int32_t acc = r.readSignedLE(1);
            out[0] = acc;
            for (int i = 1; i < n; ++i) out[i] = acc += r.readSigned(bits);
            break;
        }
        default:
            break;
    }
    r.alignByte();
    (void) signExtendBits; // silence unused warning in some build configs
    return n;
}

bool File::open(const std::string& path, std::string* error) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        if (error) *error = "cannot open " + path;
        return false;
    }
    const auto size = size_t(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> bytes(size);
    if (size && !f.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size))) {
        if (error) *error = "cannot read " + path;
        return false;
    }
    return parse(std::move(bytes), error);
}

bool File::parse(std::vector<uint8_t> bytes, std::string* error) {
    auto fail = [&](const char* what) {
        if (error) *error = what;
        return false;
    };
    if (bytes.size() < 12) return fail("truncated file");
    if (std::memcmp(bytes.data(), "BFDC", 4) != 0) return fail("bad magic (expected BFDC)");

    struct Chunk {
        uint32_t offset, size;
    };
    Chunk fmt{}, bfdi{}, indx{}, data{};
    size_t pos = 8;
    bool haveFmt = false, haveIndx = false, haveData = false;
    while (pos + 8 <= bytes.size()) {
        const char* id = reinterpret_cast<const char*>(bytes.data() + pos);
        const uint32_t size = readBe32(bytes.data() + pos + 4);
        const uint32_t body = uint32_t(pos + 8);
        if (body + size > bytes.size()) return fail("chunk overruns file");
        if (!std::memcmp(id, "fmt ", 4)) {
            fmt = {body, size};
            haveFmt = true;
        } else if (!std::memcmp(id, "BFDi", 4)) {
            bfdi = {body, size};
        } else if (!std::memcmp(id, "Indx", 4)) {
            indx = {body, size};
            haveIndx = true;
        } else if (!std::memcmp(id, "data", 4)) {
            data = {body, size};
            haveData = true;
        }
        pos = body + size;
    }
    if (!haveFmt || !haveIndx || !haveData) return fail("missing fmt / Indx / data chunk");
    if (fmt.size < 20) return fail("fmt chunk too small");

    Header h;
    h.bitDepth = int(readBe32(bytes.data() + fmt.offset));
    h.channels = int(readBe32(bytes.data() + fmt.offset + 4));
    h.frames = int(readBe32(bytes.data() + fmt.offset + 8));
    h.sampleRate = int(readBe32(bytes.data() + fmt.offset + 12));
    h.flags = int(readBe32(bytes.data() + fmt.offset + 16));
    if (h.bitDepth < 8 || h.bitDepth > 32) return fail("bad bit depth");
    if (h.channels <= 0 || h.channels > 64) return fail("bad channel count");
    if (h.frames < 0) return fail("bad frame count");

    if (bfdi.size > 0) {
        const size_t n = std::min<size_t>(bfdi.size, 36);
        h.libraryId.assign(reinterpret_cast<const char*>(bytes.data() + bfdi.offset), n);
        const auto z = h.libraryId.find('\0');
        if (z != std::string::npos) h.libraryId.resize(z);
    }

    if (indx.size < 8) return fail("Indx chunk too small");
    h.blockSize = int(readBe32(bytes.data() + indx.offset));
    h.frameCount = int(readBe32(bytes.data() + indx.offset + 4));
    if (h.blockSize <= 0 || h.frameCount < 0) return fail("bad Indx header");
    if (indx.size < 8 + uint32_t(h.frameCount) * 4) return fail("Indx offsets truncated");
    h.frameOffsets.resize(size_t(h.frameCount));
    for (int i = 0; i < h.frameCount; ++i)
        h.frameOffsets[size_t(i)] = readBe32(bytes.data() + indx.offset + 8 + i * 4);

    h.dataStart = data.offset;
    h.dataSize = data.size;

    data_ = std::move(bytes);
    header_ = std::move(h);
    return true;
}

void File::decodeFrameInto(int frameIndex, int32_t* out, int outStride) const {
    const Header& h = header_;
    const int bps = h.bytesPerSample();
    const int version = 2; // shipped libraries are v2/v3 (fixed 64-sample blocks)
    const uint32_t start = h.dataStart + h.frameOffsets[size_t(frameIndex)];
    const uint32_t end = h.dataStart +
                         (frameIndex + 1 < h.frameCount ? h.frameOffsets[size_t(frameIndex) + 1]
                                                        : h.dataSize);
    if (start >= data_.size()) return;
    const uint8_t* p = data_.data() + start;
    const uint8_t* frameEnd = data_.data() + std::min<size_t>(end, data_.size());

    const int remaining = h.frames - frameIndex * h.blockSize;
    const int subFrames = std::min<int>(16, (remaining + kSubFrameSamples - 1) / kSubFrameSamples);
    int32_t block[kSubFrameSamples * 64];
    int sampleBase = 0;

    for (int s = 0; s < subFrames; ++s) {
        if (p >= frameEnd) break;
        const int n = *p++;
        if (n != h.channels) break; // channel-count mismatch stops the frame
        for (int c = 0; c < n; ++c) {
            if (p >= frameEnd) return;
            const uint8_t scheme = *p;
            const uint32_t size = tables::blockSize(scheme, bps, version);
            if (p + size > frameEnd + 8) return; // tolerate table slack near the end
            decodeBlock(p + 1, scheme, h.bitDepth, version, block + c * kSubFrameSamples);
            p += size;
        }
        const int count = std::min(kSubFrameSamples, h.frames - (frameIndex * h.blockSize + sampleBase));
        for (int k = 0; k < count; ++k)
            for (int c = 0; c < h.channels; ++c)
                out[(sampleBase + k) * outStride + c] = block[c * kSubFrameSamples + k];
        sampleBase += kSubFrameSamples;
    }
}

void File::decodeFrame(int frameIndex, int32_t* out) const {
    const Header& h = header_;
    std::fill(out, out + size_t(h.blockSize) * size_t(h.channels), 0);
    if (frameIndex < 0 || frameIndex >= h.frameCount) return;
    decodeFrameInto(frameIndex, out, h.channels);
}

void File::readInt32(int start, int count, int32_t* out) const {
    const Header& h = header_;
    std::fill(out, out + size_t(count) * h.channels, 0);
    if (start >= h.frames || count <= 0) return;
    const int firstFrame = start / h.blockSize;
    const int lastFrame = (start + count - 1) / h.blockSize;
    std::vector<int32_t> frameBuf(size_t(h.blockSize) * h.channels);
    for (int fi = firstFrame; fi <= lastFrame && fi < h.frameCount; ++fi) {
        std::fill(frameBuf.begin(), frameBuf.end(), 0);
        decodeFrameInto(fi, frameBuf.data(), h.channels);
        const int frameStart = fi * h.blockSize;
        const int from = std::max(start, frameStart);
        const int to = std::min(start + count, frameStart + h.blockSize);
        if (to <= from) continue;
        std::memcpy(out + size_t(from - start) * h.channels,
                    frameBuf.data() + size_t(from - frameStart) * h.channels,
                    size_t(to - from) * h.channels * sizeof(int32_t));
    }
}

void File::readFloat(int start, int count, float* out) const {
    std::vector<int32_t> tmp(size_t(count) * header_.channels);
    readInt32(start, count, tmp.data());
    const float scale = 1.0f / float(1 << (header_.bitDepth - 1));
    for (size_t i = 0; i < tmp.size(); ++i) out[i] = float(tmp[i]) * scale;
}

bool decodeWholeFile(const std::string& path, std::vector<float>* interleaved,
                     Header* header, std::string* error) {
    File f;
    if (!f.open(path, error)) return false;
    const Header& h = f.header();
    interleaved->assign(size_t(h.frames) * h.channels, 0.0f);
    f.readFloat(0, h.frames, interleaved->data());
    if (header) *header = h;
    return true;
}


// ---------------------------------------------------------------------------
// DecodeRing — SPSC ring of decoded frames.
// ---------------------------------------------------------------------------

void DecodeRing::allocate(int channels, int capacityFrames) {
    channels_ = std::max(1, channels);
    capacity_ = std::max(1, capacityFrames);
    buf_.assign(size_t(capacity_) * size_t(channels_), 0);
    write_.store(0, std::memory_order_relaxed);
    published_.store(0, std::memory_order_relaxed);
    read_.store(0, std::memory_order_relaxed);
}

void DecodeRing::reset() {
    write_.store(0, std::memory_order_relaxed);
    published_.store(0, std::memory_order_relaxed);
    read_.store(0, std::memory_order_relaxed);
}

int32_t* DecodeRing::reserve(int count) {
    if (count <= 0 || capacity_ <= 0) return nullptr;
    const uint64_t w = write_.load(std::memory_order_relaxed);
    const uint64_t r = read_.load(std::memory_order_acquire);
    if (uint64_t(count) > uint64_t(capacity_)) return nullptr;
    if (w - r + uint64_t(count) > uint64_t(capacity_)) return nullptr;
    const size_t off = size_t(w % uint64_t(capacity_)) * size_t(channels_);
    return buf_.data() + off;
}

void DecodeRing::commit(int count) {
    if (count <= 0) return;
    const uint64_t w = write_.load(std::memory_order_relaxed);
    write_.store(w + uint64_t(count), std::memory_order_release);
    published_.store(w + uint64_t(count), std::memory_order_release);
}

bool DecodeRing::peek(int start, int count, int32_t* out) const {
    if (count <= 0) return true;
    const int ch = channels_;
    if (ch <= 0 || capacity_ <= 0) {
        std::fill(out, out + size_t(count) * size_t(std::max(1, ch)), 0);
        return false;
    }
    const uint64_t r = read_.load(std::memory_order_acquire);
    const uint64_t pub = published_.load(std::memory_order_acquire);
    bool full = true;
    for (int i = 0; i < count; ++i) {
        const uint64_t idx = r + uint64_t(start) + uint64_t(i);
        if (idx >= pub) {
            std::fill(out + size_t(i) * size_t(ch), out + size_t(i + 1) * size_t(ch), 0);
            full = false;
            continue;
        }
        const size_t off = size_t(idx % uint64_t(capacity_)) * size_t(ch);
        std::copy(buf_.begin() + std::ptrdiff_t(off), buf_.begin() + std::ptrdiff_t(off + size_t(ch)),
                  out + size_t(i) * size_t(ch));
    }
    return full;
}

void DecodeRing::consume(int count) {
    if (count <= 0) return;
    const uint64_t r = read_.load(std::memory_order_relaxed);
    read_.store(r + uint64_t(count), std::memory_order_release);
}

int DecodeRing::available() const {
    const uint64_t pub = published_.load(std::memory_order_acquire);
    const uint64_t r = read_.load(std::memory_order_acquire);
    return int(pub - r);
}

int DecodeRing::freeSpace() const {
    const uint64_t w = write_.load(std::memory_order_acquire);
    const uint64_t r = read_.load(std::memory_order_acquire);
    return int(uint64_t(capacity_) - (w - r));
}

// ---------------------------------------------------------------------------
// StreamedFile — disk-backed decode ring with a compressed window cache.
// ---------------------------------------------------------------------------

namespace {

// Parse the BFDC container header from raw bytes (shared with File::parse).
bool parseHeaderFromBytes(const std::vector<uint8_t>& bytes, Header* h, std::string* error) {
    auto fail = [&](const char* what) {
        if (error) *error = what;
        return false;
    };
    if (bytes.size() < 12) return fail("truncated file");
    if (std::memcmp(bytes.data(), "BFDC", 4) != 0) return fail("bad magic (expected BFDC)");

    struct Chunk {
        uint32_t offset, size;
    };
    Chunk fmt{}, bfdi{}, indx{}, data{};
    size_t pos = 8;
    bool haveFmt = false, haveIndx = false, haveData = false;
    while (pos + 8 <= bytes.size()) {
        const char* id = reinterpret_cast<const char*>(bytes.data() + pos);
        const uint32_t size = readBe32(bytes.data() + pos + 4);
        const uint32_t body = uint32_t(pos + 8);
        if (body + size > bytes.size()) return fail("chunk overruns file");
        if (!std::memcmp(id, "fmt ", 4)) {
            fmt = {body, size};
            haveFmt = true;
        } else if (!std::memcmp(id, "BFDi", 4)) {
            bfdi = {body, size};
        } else if (!std::memcmp(id, "Indx", 4)) {
            indx = {body, size};
            haveIndx = true;
        } else if (!std::memcmp(id, "data", 4)) {
            data = {body, size};
            haveData = true;
        }
        pos = body + size;
    }
    if (!haveFmt || !haveIndx || !haveData) return fail("missing fmt / Indx / data chunk");
    if (fmt.size < 20) return fail("fmt chunk too small");

    Header hdr;
    hdr.bitDepth = int(readBe32(bytes.data() + fmt.offset));
    hdr.channels = int(readBe32(bytes.data() + fmt.offset + 4));
    hdr.frames = int(readBe32(bytes.data() + fmt.offset + 8));
    hdr.sampleRate = int(readBe32(bytes.data() + fmt.offset + 12));
    hdr.flags = int(readBe32(bytes.data() + fmt.offset + 16));
    if (hdr.bitDepth < 8 || hdr.bitDepth > 32) return fail("bad bit depth");
    if (hdr.channels <= 0 || hdr.channels > 64) return fail("bad channel count");
    if (hdr.frames < 0) return fail("bad frame count");

    if (bfdi.size > 0) {
        const size_t n = std::min<size_t>(bfdi.size, 36);
        hdr.libraryId.assign(reinterpret_cast<const char*>(bytes.data() + bfdi.offset), n);
        const auto z = hdr.libraryId.find('\0');
        if (z != std::string::npos) hdr.libraryId.resize(z);
    }

    if (indx.size < 8) return fail("Indx chunk too small");
    hdr.blockSize = int(readBe32(bytes.data() + indx.offset));
    hdr.frameCount = int(readBe32(bytes.data() + indx.offset + 4));
    if (hdr.blockSize <= 0 || hdr.frameCount < 0) return fail("bad Indx header");
    if (indx.size < 8 + uint32_t(hdr.frameCount) * 4) return fail("Indx offsets truncated");
    hdr.frameOffsets.resize(size_t(hdr.frameCount));
    for (int i = 0; i < hdr.frameCount; ++i)
        hdr.frameOffsets[size_t(i)] = readBe32(bytes.data() + indx.offset + 8 + i * 4);

    hdr.dataStart = data.offset;
    hdr.dataSize = data.size;
    *h = std::move(hdr);
    return true;
}

} // namespace

bool StreamedFile::open(const std::string& path, std::string* error) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        if (error) *error = "cannot open " + path;
        return false;
    }
    const auto fileSize = size_t(f.tellg());
    f.seekg(0);
    // Only the container header + Indx need to be in memory to start
    // streaming; the data chunk is pulled through the window cache.
    const size_t headBytes = std::min<size_t>(fileSize, size_t(1) << 22);
    std::vector<uint8_t> head(headBytes);
    if (headBytes && !f.read(reinterpret_cast<char*>(head.data()), std::streamsize(headBytes))) {
        if (error) *error = "cannot read " + path;
        return false;
    }
    Header hdr;
    if (!parseHeaderFromBytes(head, &hdr, error)) return false;

    path_ = path;
    header_ = std::move(hdr);
    windowDataStart_ = 0;
    windowDataLen_ = 0;
    windowOpen_ = true;
    window_.clear();
    frameScratch_.assign(size_t(header_.blockSize) * size_t(header_.channels), 0);
    // Ring holds several blocks of look-ahead per layer. Must be a multiple
    // of blockSize so a reserve(blockSize) write never wraps mid-block.
    const int blocks = std::max(4, std::min(64, header_.frameCount));
    const int ringFrames = blocks * std::max(1, header_.blockSize);
    ring_.allocate(header_.channels, ringFrames);
    return true;
}

bool StreamedFile::ensureWindow(uint32_t dataOffset, uint32_t need, std::string* error) {
    if (need == 0) return true;
    std::lock_guard<std::mutex> lock(windowMutex_);
    if (windowOpen_ && dataOffset >= windowDataStart_ && dataOffset + need <= windowDataStart_ + windowDataLen_)
        return true;
    std::ifstream f(path_, std::ios::binary);
    if (!f) {
        if (error) *error = "cannot reopen " + path_;
        return false;
    }
    // Read a full window's worth starting at the requested offset (aligned
    // down to a 64 KiB boundary to make sequential streaming cheap).
    const uint32_t align = 64u * 1024u;
    const uint32_t start = (dataOffset / align) * align;
    const uint32_t want = std::max(need + (dataOffset - start), kWindowBytes);
    f.seekg(std::streamoff(header_.dataStart + start));
    window_.assign(want, 0);
    f.read(reinterpret_cast<char*>(window_.data()), std::streamsize(want));
    windowDataStart_ = start;
    windowDataLen_ = uint32_t(f.gcount());
    windowOpen_ = true;
    return windowDataLen_ >= (dataOffset - start) + need;
}

int StreamedFile::decodeFrameInto(int frameIndex, int32_t* out, int outStride) const {
    const Header& h = header_;
    const int bps = h.bytesPerSample();
    const int version = 2; // shipped libraries are v2/v3 (fixed 64-sample blocks)
    if (frameIndex < 0 || frameIndex >= h.frameCount) {
        for (int i = 0; i < h.blockSize; ++i)
            for (int c = 0; c < outStride; ++c) out[size_t(i) * size_t(outStride) + size_t(c)] = 0;
        return 0;
    }
    const uint32_t start = h.frameOffsets[size_t(frameIndex)];
    const uint32_t end = frameIndex + 1 < h.frameCount ? h.frameOffsets[size_t(frameIndex) + 1] : h.dataSize;

    std::lock_guard<std::mutex> lock(windowMutex_);
    if (!windowOpen_ || start < windowDataStart_ || end > windowDataStart_ + windowDataLen_) {
        for (int i = 0; i < h.blockSize; ++i)
            for (int c = 0; c < outStride; ++c) out[size_t(i) * size_t(outStride) + size_t(c)] = 0;
        return 0;
    }
    const uint8_t* p = window_.data() + (start - windowDataStart_);
    const uint8_t* frameEnd = window_.data() + (end - windowDataStart_);

    constexpr int kSubFrameSamples = 64;
    const int remaining = h.frames - frameIndex * h.blockSize;
    const int subFrames = std::min<int>(16, (remaining + kSubFrameSamples - 1) / kSubFrameSamples);
    int32_t block[kSubFrameSamples * 64];
    int sampleBase = 0;

    for (int s = 0; s < subFrames; ++s) {
        if (p >= frameEnd) break;
        const int n = *p++;
        if (n != h.channels) break; // channel-count mismatch stops the frame
        for (int c = 0; c < n; ++c) {
            if (p >= frameEnd) return sampleBase;
            const uint8_t scheme = *p;
            const uint32_t size = tables::blockSize(scheme, bps, version);
            if (p + size > frameEnd + 8) return sampleBase; // tolerate table slack near the end
            File::decodeBlock(p + 1, scheme, h.bitDepth, version, block + c * kSubFrameSamples);
            p += size;
        }
        const int count = std::min(kSubFrameSamples, h.frames - (frameIndex * h.blockSize + sampleBase));
        for (int k = 0; k < count; ++k)
            for (int c = 0; c < h.channels; ++c)
                out[(sampleBase + k) * outStride + c] = block[c * kSubFrameSamples + k];
        sampleBase += kSubFrameSamples;
    }
    return sampleBase;
}

int StreamedFile::readFrame(int frameStart, int count) {
    if (count <= 0 || frameStart < 0 || frameStart >= header_.frameCount) return 0;
    const Header& h = header_;
    const int last = std::min(header_.frameCount, frameStart + count);
    int decoded = 0;
    for (int fi = frameStart; fi < last; ++fi) {
        if (ring_.freeSpace() < h.blockSize) break;
        // ensure the compressed bytes for this frame are in the window
        const uint32_t off = h.frameOffsets[size_t(fi)];
        const uint32_t end = fi + 1 < h.frameCount ? h.frameOffsets[size_t(fi) + 1] : h.dataSize;
        if (!ensureWindow(off, end - off, nullptr)) break;

        // The ring stores decoded sample frames; each indexed frame decodes
        // to exactly h.blockSize sample frames of interleaved PCM.
        int32_t* dst = ring_.reserve(h.blockSize);
        if (!dst) break;
        decodeFrameInto(fi, dst, h.channels);
        ring_.commit(h.blockSize);
        ++decoded;
    }
    return decoded;
}

void StreamedFile::decodeFrame(int frameIndex, int32_t* out) const {
    const Header& h = header_;
    const int ch = h.channels;
    std::fill(out, out + size_t(h.blockSize) * size_t(ch), 0);
    if (frameIndex < 0 || frameIndex >= h.frameCount) return;

    // Fast path: the ring holds whole decoded blocks in stream order. When
    // the requested block is already buffered, copy it straight out without
    // touching the compressed window. The ring is indexed in decoded sample
    // frames; each committed block pushed exactly h.blockSize frames, so
    // block N covers sample frames [N*blockSize, (N+1)*blockSize).
    if (ring_.channels() == ch) {
        const uint64_t r = ring_.readIndex();
        const uint64_t pub = ring_.writeIndex();
        const uint64_t want = uint64_t(frameIndex) * uint64_t(h.blockSize);
        if (want + uint64_t(h.blockSize) <= pub && want >= r) {
            if (ring_.peek(int(want - r), h.blockSize, out)) return;
        }
    }
    decodeFrameInto(frameIndex, out, ch);
}

} // namespace bfdlac
