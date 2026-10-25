// BFDLAC codec — BFDC container reader and block decoder.
// Pure C++20, no JUCE. Semantics derived from analysis/ (BFD3 3.5.1 arm64).
//
// Container: "BFDC" + u32be totalSize + chunks: "fmt " "BFDi" "Indx" "data".
// Frames: 1024 sample frames each, 16 sub-frames of 64 samples; every
// sub-frame is u8 channelCount then per channel { u8 scheme, payload }.
// Payload size comes from the recovered size table (analysis/bfdlac_tables.json).
// Residual fields are two's complement, consumed LSB-first inside each byte,
// starting byte-aligned at the payload; seeds are little-endian signed.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace bfdlac {

struct Header {
    int bitDepth = 0;      // 24 observed across all shipped libraries
    int channels = 0;      // 7..16, fixed per file
    int frames = 0;        // sample frames
    int sampleRate = 0;    // 44100 observed
    int flags = 0;
    std::string libraryId; // "BFYMA-..." from the BFDi chunk
    int blockSize = 1024;  // samples per indexed frame
    int frameCount = 0;
    std::vector<uint32_t> frameOffsets; // byte offsets into the data chunk
    uint32_t dataStart = 0;
    uint32_t dataSize = 0;

    int bytesPerSample() const { return bitDepth >= 20 ? 3 : 2; }
};

// Decoded samples keep the source bit-depth scale (24-bit values in int32).
// readFloat() normalises to [-1, 1) by 2^(bitDepth-1).
class File {
public:
    // Load and parse a .bfdlac file. Returns false and fills `error` on failure.
    bool open(const std::string& path, std::string* error = nullptr);
    bool parse(std::vector<uint8_t> bytes, std::string* error = nullptr);

    const Header& header() const { return header_; }

    // Decode `count` sample frames starting at `start` into interleaved output.
    void readInt32(int start, int count, int32_t* out) const;
    void readFloat(int start, int count, float* out) const;

    // Decode one indexed frame (header().blockSize sample frames) into
    // interleaved `out` (blockSize * channels values). Allocation-free and
    // const, so it is safe to call from the audio thread for streaming
    // playback. Samples past the end of the file are zero.
    void decodeFrame(int frameIndex, int32_t* out) const;

    // Decode one 64-sample sub-frame block; used by tests and tools.
    // Returns the number of samples written (always 64).
    static int decodeBlock(const uint8_t* payload, uint8_t scheme, int bitDepth,
                           int version, int32_t* out64);

private:
    void decodeFrameInto(int frameIndex, int32_t* out, int outStride) const;

    std::vector<uint8_t> data_;
    Header header_;
};

// ---------------------------------------------------------------------------
// Streaming decode path: disk -> ring buffer.
//
// BFD3 streams compressed frames through VSoundDiskStreamCache /
// VSoundIOBFDLAC::ReadDataBlock into a TRingBuffer of IOBuffers, then decodes
// into the mixer. openbfd mirrors that: a DecodeRing of *decoded* PCM frames
// (single producer = loader thread, single consumer = audio thread), fed by
// StreamedFile::readFrame on the loader thread and drained by decodeFrame on
// the audio thread. The audio thread never touches the filesystem and never
// allocates.
// ---------------------------------------------------------------------------

// SPSC ring of decoded sample frames (interleaved int32).
class DecodeRing {
public:
    DecodeRing() = default;
    DecodeRing(const DecodeRing&) = delete;
    DecodeRing& operator=(const DecodeRing&) = delete;

    // message/loader thread: size the ring (allocates once).
    void allocate(int channels, int capacityFrames);
    void reset(); // quiescent producer+consumer (loader restart)

    // producer (loader thread)
    // Reserve `count` contiguous frames; nullptr when the ring is too full.
    int32_t* reserve(int count);
    void commit(int count); // publish `count` frames previously reserved

    // consumer (audio thread)
    // Copy `count` frames starting `start` frames after the read cursor
    // without consuming. Frames past the write cursor are zero-filled.
    bool peek(int start, int count, int32_t* out) const;
    void consume(int count);

    int available() const;  // frames readable right now
    int freeSpace() const;  // frames writable right now
    int capacity() const { return capacity_; }
    int channels() const { return channels_; }
    uint64_t writeIndex() const { return write_.load(std::memory_order_acquire); }
    uint64_t readIndex() const { return read_.load(std::memory_order_acquire); }

private:
    int channels_ = 0;
    int capacity_ = 0;
    std::vector<int32_t> buf_; // capacity_ * channels_
    std::atomic<uint64_t> write_{0}; // total frames ever reserved
    std::atomic<uint64_t> published_{0}; // total frames visible to the consumer
    std::atomic<uint64_t> read_{0};  // total frames ever consumed
};

// One streamed layer: header + bounded compressed window + decode ring.
// readFrame() is loader-thread (does disk I/O on a window miss);
// decodeFrame() is audio-thread safe and allocation-free after open().
class StreamedFile {
public:
    bool open(const std::string& path, std::string* error = nullptr);
    const Header& header() const { return header_; }
    const std::string& path() const { return path_; }

    // Loader thread: decode `count` indexed frames starting at `start` into
    // the ring, reading compressed bytes from disk only when the window
    // cache misses. Returns the number of frames actually decoded.
    int readFrame(int frameStart, int count);

    // Audio thread: one indexed frame of interleaved int32 (blockSize *
    // channels). Uses the ring when the frame is buffered, otherwise falls
    // back to decoding directly from the compressed window (which may have
    // to consult the loader-filled cache; if that misses, output is zero).
    void decodeFrame(int frameIndex, int32_t* out) const;

    // Audio thread: frames of decoded look-ahead currently in the ring.
    int bufferedFrames() const { return ring_.available(); }
    DecodeRing& ring() { return ring_; }

private:
    bool ensureWindow(uint32_t dataOffset, uint32_t need, std::string* error);
    int decodeFrameInto(int frameIndex, int32_t* out, int outStride) const;

    std::string path_;
    Header header_;

    // Compressed window cache: a bounded slice of the data chunk kept
    // resident so repeated decodeFrame calls never re-read the disk.
    static constexpr uint32_t kWindowBytes = 1u << 22; // 4 MiB of compressed data
    mutable std::mutex windowMutex_;
    std::vector<uint8_t> window_;
    uint32_t windowDataStart_ = 0; // data-chunk offset of window_[0]
    uint32_t windowDataLen_ = 0;
    bool windowOpen_ = false;

    DecodeRing ring_;
    std::vector<int32_t> frameScratch_; // one frame, loader thread only
};

// Convenience: decode an entire file to interleaved float.
bool decodeWholeFile(const std::string& path, std::vector<float>* interleaved,
                     Header* header, std::string* error = nullptr);

} // namespace bfdlac
