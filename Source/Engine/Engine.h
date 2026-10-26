// Drum engine: layer cache, streaming voice playback, mic-channel routing.
// Pure C++20, no JUCE. The plugin layer feeds it MIDI and drains its buses.
//
// Real-time model
// ---------------
// * Layers stay *compressed* in RAM (`SampleData` owns the .bfdlac bytes,
//   ~1/4 of the decoded float size). Voices decode 1024-frame chunks on the
//   fly; decoding runs >1000x realtime per voice and allocates nothing.
// * All disk I/O happens on the engine's loader thread. When a kit is set,
//   every layer reachable from the keymap is resolved on the message thread
//   and preloaded in the background. The audio thread never opens files,
//   never takes a blocking lock and never allocates.
// * A hit whose layer is not loaded yet plays the nearest loaded layer of the
//   same articulation (level-matched) and bumps the missing layer to the
//   front of the loader queue.
#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../Bfdlac/BfdlacCodec.h"
#include "../Model/Model.h"
#include "Mixer.h"

namespace bfdengine {

inline constexpr int kMaxChannels = 32;   // shipped files use 7..16 mics
inline constexpr int kMaxBlockSize = 1024; // BFDLAC frame size (Indx block)

// One velocity layer: compressed file bytes + level statistics + decode ring.
// `file` is the in-memory compressed representation used by fast paths;
// `stream` is the disk->ring streaming reader used by voices
// when the layer is too large to keep fully resident.
struct SampleData {
    int channels = 0;
    int frames = 0;
    int sampleRate = 44100;
    int blockSize = kMaxBlockSize;
    size_t bytes = 0;     // resident (compressed) size
    bfdlac::File file;    // decoded on demand by voices (resident path)
    std::shared_ptr<bfdlac::StreamedFile> stream; // streaming path (disk -> ring)
    bool streaming = false; // true when `stream` is the playback source
};

// Budgeted LRU cache of loaded layers. `get` performs disk I/O and must not
// be called from the audio thread. Entries still referenced elsewhere (a kit
// or a sounding voice) are never evicted.
class SampleCache {
public:
    explicit SampleCache(size_t budgetBytes = 2048ull << 20) : budget_(budgetBytes) {}

    std::shared_ptr<const SampleData> get(const std::string& path);  // loads if needed
    // Streaming variant: opens the layer as a disk->ring StreamedFile instead
    // of holding the whole compressed file resident.
    std::shared_ptr<const SampleData> get(const std::string& path, bool streaming);
    std::shared_ptr<const SampleData> find(const std::string& path) const; // no I/O
    void clear();
    size_t bytesUsed() const;
    size_t budget() const;
    void setBudget(size_t bytes);

private:
    void evictLocked();
    struct Entry {
        std::shared_ptr<const SampleData> data;
        size_t bytes = 0;
        std::list<std::string>::iterator lru;
    };
    mutable std::mutex mutex_;
    size_t budget_;
    size_t used_ = 0;
    std::map<std::string, Entry> map_;
    mutable std::list<std::string> lru_;
};

// Per file channel: which strip input jack it lands in and how loud. Built
// on the message thread from the kit state and the mixer patch.
struct ChannelRoute {
    int target = -1;      // strip * 2 + side (0 = L, 1 = R); -1 = not heard
    float gain = 0.0f;
    bool ambient = false; // scaled by the hit's velocity->ambience amount
};

// Routes for every channel of one kit slot's piece.
std::vector<ChannelRoute> buildRoutes(const bfdmodel::Kit& kit, int slot, const PieceLookup& pieces,
                                      const MixerState& mixer);

// Trigger-time piece parameters + channel routes for one slot.
struct SlotRouting {
    const bfdmodel::Piece* piece = nullptr;
    std::array<ChannelRoute, kMaxChannels> routes{};
    int routeCount = 0;
    float trim = 0.8f, tune = 0.5f, dyn = 0.5f, dampAmt = 0.0f, dampFreq = 500.0f;
    bool mute = false;
};

// Everything the audio thread needs besides samples: swapped as one object
// whenever the kit parameters or the mixer change (no layer reloading).
struct Routing {
    std::vector<SlotRouting> slots;
    std::array<StripMode, kNumStrips> modes{};
    std::array<StripState, kNumStrips> strips{};
    bool anySolo = false;
    float masterGain = 0.8f;
};

struct VoiceParams {
    std::shared_ptr<const SampleData> sample;
    const bfdmodel::Piece* piece = nullptr; // live routes apply only while the slot still holds it
    std::array<ChannelRoute, kMaxChannels> routes{}; // frozen at trigger (slot changed underneath)
    int routeCount = 0;
    int slot = -1;         // kit slot, for per-piece polyphony
    int rrGroup = 0;       // round-robin group index used for this hit
    float rate = 1.0f;     // playback rate (tuning and sample-rate conversion)
    float gain = 1.0f;     // master gain of the voice
    float ambScale = 1.0f; // velocity -> ambience share
    float lpCoeff = 0.0f;  // one-pole lowpass coefficient (damping)
    float dampMix = 0.0f;  // 0 = dry, 1 = fully lowpassed
};

class Voice {
public:
    void allocate(); // message thread: reserve the decode buffers
    void start(const VoiceParams& params, uint64_t age);
    void stop(float releaseSec, double sampleRate); // fade out, then free
    bool active() const { return active_; }
    bool releasing() const { return releasing_; }
    int slot() const { return params_.slot; }
    int rrGroup() const { return params_.rrGroup; }
    uint64_t age() const { return age_; }

    // Render frames [from, from+n) of the current chunk into the strip input
    // jacks (`jacks[strip * 2 + side]`). `live` is the slot's current routing.
    // `scratch` holds kMaxBlockSize*kMaxChannels ints (shared, audio thread).
    void render(float* const* jacks, int from, int n, int32_t* scratch, const SlotRouting* live);

private:
    const float* frameData(int frameIndex, int32_t* scratch); // decoded chunk

    VoiceParams params_;
    double pos_ = 0.0;
    bool active_ = false;
    bool releasing_ = false;
    float env_ = 1.0f, releaseStep_ = 0.0f;
    uint64_t age_ = 0;
    std::array<float, kMaxChannels> lpState_{};
    std::vector<float> chunk_[2]; // two decoded frames (k and k+1) for interpolation
    int chunkIndex_[2] = {-1, -1};
};

struct NoteEvent {
    int note = 60;
    float velocity = 1.0f; // 0..1
    bool isOn = true;
    int offset = 0;        // sample offset inside the processed block
};

// Kit resolved for playback (built on the message thread, read by the audio
// thread, filled by the loader thread). Internal; exposed for diagnostics.
struct ResolvedKit;

class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Library / kit wiring (message thread). Each call re-resolves the kit and
    // schedules background loading; it never blocks the audio thread.
    void setLibraries(std::vector<bfdmodel::Library> libs);
    void setKit(bfdmodel::Kit kit);
    const bfdmodel::Kit& kit() const { return kit_; }
    const std::vector<bfdmodel::Library>& libraries() const { return libs_; }
    const bfdmodel::Piece* findPiece(const std::string& slotRef) const;
    // Explicit keymap (e.g. a preset's BFD2MidiMap); an empty map selects the
    // automatic GM map built from the kit's piece kinds.
    void setKeymap(std::vector<bfdmodel::KeymapEntry> map);
    const std::vector<bfdmodel::KeymapEntry>& keymap() const { return activeKeymap_; }
    const std::vector<bfdmodel::KeymapEntry>& explicitKeymap() const { return keymap_; }
    // Kit slot -> piece (nullptr = empty / not in any scanned library).
    PieceLookup pieceLookup() const;

    // Mixer (message thread): patch, strips, master, bleed masters.
    void setMixer(MixerState mixer);
    const MixerState& mixer() const { return mixer_; }

    // Readouts for the UI (any thread). Peaks since the previous call:
    // index 0/1 = master L/R, 2 + strip * 2 + side = strips.
    static constexpr int kNumMeters = 2 + kNumStrips * 2;
    float takeMeter(int index) { return meters_[size_t(index)].exchange(0.0f, std::memory_order_relaxed); }
    // Strongest hit velocity on `slot` since the previous call (0 = none).
    float takeSlotHit(int slot) {
        return slot >= 0 && slot < bfdmodel::Kit::kMaxSlots ? slotHits_[size_t(slot)].exchange(0.0f, std::memory_order_relaxed) : 0.0f;
    }
    int activeVoices() const { return activeVoices_.load(std::memory_order_relaxed); }

    // Loader progress (any thread).
    int layersReady() const { return layersReady_.load(std::memory_order_relaxed); }
    int layersTotal() const { return layersTotal_.load(std::memory_order_relaxed); }
    // Layers the loader has tried (ready + failed). Below the total the
    // loader is still working, unless the cache is full: then the rest loads
    // on demand.
    int layersDone() const { return layersDone_.load(std::memory_order_relaxed); }
    bool cacheFull() const { return cache_.bytesUsed() >= cache_.budget(); }
    // Block until the current kit is fully loaded (tools/tests; not audio thread).
    bool waitUntilLoaded(int timeoutMs) const;

    // Realtime. `events` must be sorted by offset. `outs` holds planar output
    // channels: [0] master L, [1] master R, [2 + 2s] / [3 + 2s] strip s L / R.
    // Master pointers must be valid; strip pointers may be null (bus off).
    static constexpr int kNumOuts = 2 + kNumStrips * 2;
    void process(const NoteEvent* events, int numEvents, float* const* outs, int n);
    void setSampleRate(double sr) { sampleRate_.store(sr); }
    SampleCache& cache() { return cache_; }

    // Streaming: when enabled, layers are opened as StreamedFile (disk ->
    // decode ring) and the loader fills the ring ahead of playback instead of
    // holding whole compressed files in RAM. Disabled by default (resident
    // path is faster for small layers); enable for very large layers.
    void setStreamingEnabled(bool on) { streamingEnabled_.store(on); }
    bool streamingEnabled() const { return streamingEnabled_.load(); }
    // Streaming look-ahead, in whole BFDLAC frames (1024 samples each).
    void setStreamLookaheadFrames(int frames) { streamLookahead_.store(std::max(1, frames)); }
    int streamLookaheadFrames() const { return streamLookahead_.load(); }

    static constexpr int kChunk = 256;    // internal render chunk (frames)
    static constexpr int kMaxVoices = 96;
    static constexpr int kMaxVoicesPerSlot = 8; // older repeats of a piece fade out

private:
    void rebuild();
    void updateRouting();
    void trigger(ResolvedKit& kit, const Routing& routing, const NoteEvent& ev);
    void mixStrips(const Routing& routing, float* const* outs, int offset, int n);
    void loaderMain();

    SampleCache cache_;
    std::vector<bfdmodel::Library> libs_;
    bfdmodel::Kit kit_;
    std::vector<bfdmodel::KeymapEntry> keymap_;       // explicit map (empty = automatic)
    std::vector<bfdmodel::KeymapEntry> activeKeymap_; // what rebuild() resolved
    MixerState mixer_;
    std::map<std::string, std::string> pathMemo_; // message thread: layer path resolution

    // kit hand-off: message thread -> audio thread
    std::mutex swapMutex_;
    std::shared_ptr<ResolvedKit> pendingKit_; // guarded by swapMutex_
    std::shared_ptr<ResolvedKit> retiredKit_; // guarded by swapMutex_ (freed off the audio thread)
    std::shared_ptr<ResolvedKit> activeKit_;  // audio thread only
    std::shared_ptr<Routing> pendingRouting_; // guarded by swapMutex_
    std::shared_ptr<Routing> retiredRouting_; // guarded by swapMutex_
    std::shared_ptr<Routing> activeRouting_;  // audio thread only

    // loader thread
    mutable std::mutex loaderMutex_;
    mutable std::condition_variable loaderCv_;
    std::shared_ptr<ResolvedKit> loaderKit_; // guarded by loaderMutex_
    std::atomic<bool> loaderWake_{false};
    std::atomic<bool> quit_{false};
    std::atomic<int> layersReady_{0}, layersTotal_{0}, layersDone_{0};
    std::thread loader_;

    // audio thread
    std::array<Voice, kMaxVoices> voices_;
    std::vector<int32_t> scratch_;
    std::vector<float> jackStorage_;                 // kNumStrips * 2 * kChunk
    std::array<float*, kNumStrips * 2> jacks_{};
    std::array<float, kNumStrips * 2> stripGain_{}; // smoothed per-side strip gains
    float masterGainNow_ = -1.0f;
    std::array<std::atomic<float>, kNumMeters> meters_{};
    std::array<std::atomic<float>, bfdmodel::Kit::kMaxSlots> slotHits_{};
    std::atomic<int> activeVoices_{0};
    uint64_t voiceAge_ = 0;
    uint32_t rr_ = 0;
    std::atomic<double> sampleRate_{44100.0};
    std::atomic<bool> streamingEnabled_{false};
    std::atomic<int> streamLookahead_{32}; // BFDLAC frames of look-ahead
};

// Resolve a velocity-layer file, tolerating stale extensions in the metadata
// (BFDArticTweaks.xml often still says "masterNN.wav" for ".bfdlac" files).
std::string resolveLayerPath(const std::string& dir, const std::string& fileName);

// Velocity-layer selection helpers (shared with tests).
const bfdmodel::VelLayer* pickLayer(const bfdmodel::Articulation& art, float velocity01);
// Gain (dB) for a hit at `velocity01` that asks for layer `want` and plays
// layer `idx` (a stand-in while `want` loads). Layers play as recorded; in a
// velocity window the level slides towards the next layer's loudness, a
// stand-in is matched to the requested layer's loudness, and `dyn` scales the
// distance to the loudest layer (0.5 = as recorded, 0 = flat, 1 = doubled).
float layerGainDb(const std::vector<bfdmodel::VelLayer>& layers, int want, int idx, float velocity01, float dyn);

} // namespace bfdengine
