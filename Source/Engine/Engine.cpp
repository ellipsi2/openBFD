#include "Engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <numeric>

namespace bfdengine {
using bfdmodel::Articulation;
using bfdmodel::ChanInfo;
using bfdmodel::KitPieceState;
using bfdmodel::Piece;
using bfdmodel::VelLayer;

// --- resolved kit (internal) ----------------------------------------------

struct LayerSlot {
    std::string path;
    std::shared_ptr<const SampleData> sample; // written by the loader before `ready`
    std::atomic<bool> ready{false};
    std::atomic<bool> wanted{false};          // audio thread: "load me next"
    bool failed = false;                      // loader thread only
};

struct ResolvedArtic {
    const Articulation* art = nullptr;
    int first = 0, count = 0; // kit->layers[first .. first+count), parallel to art->layers
    float trimDb = 0.0f;      // articulation trim + kit aTrim
    float v2p = 0.0f;         // bipolar velocity->pitch amount, -1..1
    float v2a = 0.0f;         // velocity->ambience amount, 0..1
    // round-robin: rrGroups.size() >= 1 (group 0 = primary). Each group has
    // its own layer list and its own slice of kit->layers.
    struct RrSpan {
        const bfdmodel::RrGroup* group = nullptr;
        int first = 0, count = 0;
    };
    std::vector<RrSpan> rrGroups;
    int rrCount() const { return int(std::max<size_t>(1, rrGroups.size())); }
};

struct ResolvedSlot {
    std::shared_ptr<const Piece> piece;
    KitPieceState state;
};

struct NoteTarget {
    int slot = -1;
    int artic = -1;
};

struct ResolvedKit {
    std::vector<ResolvedSlot> slots;
    std::vector<ResolvedArtic> artics;
    std::unique_ptr<LayerSlot[]> layers;
    int layerCount = 0;
    std::array<NoteTarget, 128> notes{};
    std::vector<int> loadOrder; // background preload order
    size_t cursor = 0;          // loader thread only
    std::atomic<int> ready{0}, done{0};
    // Round-robin cursor per articulation (audio thread only). Each trigger
    // advances it, so successive hits cycle through rrGroups[0..n).
    std::vector<uint32_t> rrCursor;
    // Streaming: LayerSlot gains a streamed variant when streaming is on.
};

// --- SampleCache ---------------------------------------------------------

namespace {

std::shared_ptr<SampleData> loadLayer(const std::string& path, bool streaming = false) {
    auto data = std::make_shared<SampleData>();
    std::string err;
    if (streaming) {
        data->stream = std::make_shared<bfdlac::StreamedFile>();
        if (!data->stream->open(path, &err)) return nullptr;
        const bfdlac::Header& h = data->stream->header();
        if (h.channels <= 0 || h.channels > kMaxChannels) return nullptr;
        if (h.blockSize <= 0 || h.blockSize > kMaxBlockSize) return nullptr;
        data->channels = h.channels;
        data->frames = h.frames;
        data->sampleRate = h.sampleRate > 0 ? h.sampleRate : 44100;
        data->blockSize = h.blockSize;
        data->streaming = true;
        // Level statistics: one pass over the decoded ring after a full read.
        // For streaming we defer this to a cheap partial estimate so loading
        // stays bounded; a full pass happens if the caller uses the resident
        // path later.
        data->rms = 1.0f;
        data->peak = 1.0f;
        data->sumPeak = 1.0f;
        data->bytes = size_t(h.dataSize) / 4 + sizeof(SampleData); // ring budget estimate
        return data;
    }
    if (!data->file.open(path, &err)) return nullptr;
    const bfdlac::Header& h = data->file.header();
    // Voices keep two decoded frames per channel in fixed buffers.
    if (h.channels <= 0 || h.channels > kMaxChannels) return nullptr;
    if (h.blockSize <= 0 || h.blockSize > kMaxBlockSize) return nullptr;
    data->channels = h.channels;
    data->frames = h.frames;
    data->sampleRate = h.sampleRate > 0 ? h.sampleRate : 44100;
    data->blockSize = h.blockSize;

    // One full decode for the level statistics used by gain staging.
    const int ch = h.channels;
    const float scale = 1.0f / float(1 << (h.bitDepth - 1));
    std::vector<int32_t> frame(size_t(h.blockSize) * size_t(ch));
    double acc = 0.0;
    float peak = 0.0f, sumPeak = 0.0f;
    for (int fi = 0; fi < h.frameCount; ++fi) {
        data->file.decodeFrame(fi, frame.data());
        const int count = std::min(h.blockSize, h.frames - fi * h.blockSize);
        for (int s = 0; s < count; ++s) {
            float sum = 0.0f;
            for (int c = 0; c < ch; ++c) {
                const float v = float(frame[size_t(s) * size_t(ch) + size_t(c)]) * scale;
                acc += double(v) * v;
                peak = std::max(peak, std::abs(v));
                sum += v;
            }
            sumPeak = std::max(sumPeak, std::abs(sum));
        }
    }
    const double values = std::max(1.0, double(h.frames) * ch);
    data->rms = float(std::sqrt(acc / values));
    data->peak = std::max(peak, 1.0e-9f);
    data->sumPeak = std::max(sumPeak, 1.0e-9f);
    data->bytes = size_t(h.dataSize) + sizeof(SampleData);
    return data;
}

} // namespace

std::shared_ptr<const SampleData> SampleCache::get(const std::string& path) {
    return get(path, false);
}

std::shared_ptr<const SampleData> SampleCache::get(const std::string& path, bool streaming) {
    if (auto hit = find(path)) return hit;
    std::shared_ptr<const SampleData> data = loadLayer(path, streaming); // I/O outside the lock
    if (!data) return nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(path);
    if (it != map_.end()) return it->second.data; // raced with another loader
    lru_.push_front(path);
    map_[path] = {data, data->bytes, lru_.begin()};
    used_ += data->bytes;
    evictLocked();
    return data;
}

std::shared_ptr<const SampleData> SampleCache::find(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(path);
    if (it == map_.end()) return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second.lru);
    return it->second.data;
}

void SampleCache::evictLocked() {
    // Only entries nobody else references (no kit, no voice) can go; their
    // memory is released here, on a non-audio thread.
    auto it = lru_.end();
    while (used_ > budget_ && it != lru_.begin()) {
        --it;
        auto mit = map_.find(*it);
        if (mit == map_.end() || mit->second.data.use_count() > 1) continue;
        used_ -= mit->second.bytes;
        map_.erase(mit);
        it = lru_.erase(it);
    }
}

void SampleCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    map_.clear();
    lru_.clear();
    used_ = 0;
}

size_t SampleCache::bytesUsed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return used_;
}

size_t SampleCache::budget() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return budget_;
}

void SampleCache::setBudget(size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    budget_ = bytes;
    evictLocked();
}

// --- routing -------------------------------------------------------------

namespace {

// Ambience send gain for one ambient channel of a piece. BFD3 kits carry one
// BFD2KitPieceAmbSend per ambient mic group; BFD2-era kits only have
// gainOH / gainRM / gainPZM.
float ambientGain(const KitPieceState& st, const ChanInfo& ch) {
    const std::string group = ch.group();
    float g = 1.0f;
    if (!st.ambSends.empty()) {
        for (const auto& send : st.ambSends) {
            ChanInfo src;
            src.name = send.sourceMic;
            if (src.group() == group || send.destChan == group || send.sourceMic == ch.name) {
                g = send.active ? faderGain(send.gain) : 0.0f;
                break;
            }
        }
    } else if (group == "OH" || group == "Overhead") {
        g = faderGain(st.gainOH);
    } else if (group == "Amb3" || group == "PZM" || group == "Amb" || group == "Ambient") {
        g = faderGain(st.gainPZM);
    } else {
        g = faderGain(st.gainRM);
    }
    return g * faderGain(st.ambMasterGain);
}

int jackTarget(const MixerState& mixer, const std::string& key) {
    const Jack j = mixer.jackOf(key);
    return j.valid() ? j.strip * 2 + j.side : -1;
}

std::string directKey(int slot, const std::string& chan) { return "D:" + std::to_string(slot) + ":" + chan; }

// The kick (or snare) mic of piece `owner` that matches bleed channel `ch`:
// same name, else same mic index, else its first own mic.
const ChanInfo* matchingOwnMic(const Piece& owner, const ChanInfo& ch) {
    const bfdmodel::PieceKind kind = bfdmodel::pieceKind(owner);
    const ChanInfo* first = nullptr;
    const ChanInfo* byIndex = nullptr;
    for (const auto& o : owner.channels) {
        if (!bfdmodel::isOwnChannel(kind, o)) continue;
        if (o.name == ch.name) return &o;
        if (!first) first = &o;
        if (!byIndex && o.micIndex == ch.micIndex) byIndex = &o;
    }
    return byIndex ? byIndex : first;
}

int firstSlotOfKind(const PieceLookup& pieces, bfdmodel::PieceKind kind) {
    for (size_t s = 0; s < pieces.size(); ++s)
        if (pieces[s] && bfdmodel::pieceKind(*pieces[s]) == kind) return int(s);
    return -1;
}

} // namespace

std::vector<ChannelRoute> buildRoutes(const bfdmodel::Kit& kit, int slot, const PieceLookup& pieces,
                                      const MixerState& mixer) {
    std::vector<ChannelRoute> routes;
    if (slot < 0 || size_t(slot) >= pieces.size() || !pieces[size_t(slot)] || size_t(slot) >= kit.pieces.size())
        return routes;
    const Piece& piece = *pieces[size_t(slot)];
    const KitPieceState& st = kit.pieces[size_t(slot)];
    const bfdmodel::PieceKind kind = bfdmodel::pieceKind(piece);
    routes.resize(piece.channels.size());

    std::string primary; // first own mic: the "Dir" bleed destination
    for (const auto& ch : piece.channels)
        if (bfdmodel::isOwnChannel(kind, ch)) {
            primary = directKey(slot, ch.name);
            break;
        }

    // Bleed into the kick / snare mics (kick_bl_dst / snre_bl_dst):
    // 0 Off, 1 Dir (this piece's primary direct mic), 2 On (the kit's kick /
    // snare piece mics, scaled by that piece's bleed return and input trim).
    auto bleedRoute = [&](const ChanInfo& ch, int dst, float level, float master,
                          bfdmodel::PieceKind ownerKind) {
        ChannelRoute r;
        const float base = level * st.bleedSend * faderGain(master);
        if (dst == 1) {
            if (!primary.empty()) {
                r.target = jackTarget(mixer, primary);
                r.gain = base * faderGain(st.bleedInputTrim);
            }
        } else if (dst == 2) {
            const int owner = firstSlotOfKind(pieces, ownerKind);
            if (owner >= 0 && owner != slot) {
                if (const ChanInfo* mic = matchingOwnMic(*pieces[size_t(owner)], ch)) {
                    const KitPieceState& os = kit.pieces.size() > size_t(owner) ? kit.pieces[size_t(owner)] : st;
                    r.target = jackTarget(mixer, directKey(owner, mic->name));
                    r.gain = base * os.bleedReturn * faderGain(os.bleedInputTrim);
                }
            }
        }
        return r;
    };

    for (size_t i = 0; i < piece.channels.size(); ++i) {
        const ChanInfo& ch = piece.channels[i];
        ChannelRoute r;
        if (bfdmodel::isOwnChannel(kind, ch)) {
            r.target = jackTarget(mixer, directKey(slot, ch.name));
            r.gain = 1.0f;
        } else if (ch.isAmbient()) {
            std::string name = ch.name;
            if (st.flipAmb && (ch.encoding == "stereoLeft" || ch.encoding == "stereoRight")) {
                const char* other = ch.encoding == "stereoLeft" ? "stereoRight" : "stereoLeft";
                for (const auto& o : piece.channels)
                    if (o.isAmbient() && o.group() == ch.group() && o.encoding == other) name = o.name;
            }
            r.target = jackTarget(mixer, "A:" + name);
            r.gain = ambientGain(st, ch);
            r.ambient = true;
        } else if (ch.type == "dirKick") {
            r = bleedRoute(ch, st.kickBleedDst, st.kickBleedLvl, mixer.masterKickBleed, bfdmodel::PieceKind::Kick);
        } else if (ch.type == "dirSnare") {
            r = bleedRoute(ch, st.snareBleedDst, st.snareBleedLvl, mixer.masterSnareBleed, bfdmodel::PieceKind::Snare);
        } else {
            // the kick / snare pieces' Direct channel: other bleed
            r.target = jackTarget(mixer, "B:Other");
            r.gain = st.bleedSend * faderGain(mixer.masterOtherBleed);
        }
        if (r.target < 0) r.gain = 0.0f;
        routes[i] = r;
    }
    return routes;
}

// --- velocity layers -----------------------------------------------------

namespace {
int pickLayerIndex(const Articulation& art, float velocity01) {
    if (art.layers.empty()) return -1;
    const float v = std::clamp(velocity01, 0.0f, 1.0f);
    for (size_t i = 0; i < art.layers.size(); ++i)
        if (v >= art.layers[i].velLo && v < art.layers[i].velHi) return int(i);
    return int(art.layers.size()) - 1;
}
} // namespace

const VelLayer* pickLayer(const Articulation& art, float velocity01) {
    const int i = pickLayerIndex(art, velocity01);
    return i < 0 ? nullptr : &art.layers[size_t(i)];
}

float layerGainDb(float velocity01, float layerLoudness, float dyn, float meanLoudness) {
    const float v = std::clamp(velocity01, 0.0f, 1.0f);
    const float d = std::clamp(dyn, 0.0f, 1.0f);
    // dyn=1: velocity maps to a 60 dB playback range (v=1 -> -6 dB).
    // dyn=0: every layer plays at the articulation mean loudness.
    const float targetDb = -6.0f + d * (-60.0f * (1.0f - v)) + (1.0f - d) * (meanLoudness + 6.0f);
    return targetDb - layerLoudness;
}

// --- Voice ---------------------------------------------------------------

void Voice::allocate() {
    for (auto& c : chunk_) c.assign(size_t(kMaxBlockSize) * kMaxChannels, 0.0f);
}

void Voice::start(const VoiceParams& params, uint64_t age) {
    params_ = params;
    pos_ = 0.0;
    active_ = params.sample && params.sample->frames > 1 && !chunk_[0].empty();
    releasing_ = false;
    env_ = 1.0f;
    releaseStep_ = 0.0f;
    age_ = age;
    chunkIndex_[0] = chunkIndex_[1] = -1;
    lpState_.fill(0.0f);
}

void Voice::stop(float releaseSec, double sampleRate) {
    if (!active_ || releasing_) return;
    releasing_ = true;
    releaseStep_ = 1.0f / std::max(1.0f, releaseSec * float(sampleRate));
}

const float* Voice::frameData(int frameIndex, int32_t* scratch) {
    const int slot = frameIndex & 1;
    float* dst = chunk_[slot].data();
    if (chunkIndex_[slot] != frameIndex) {
        const SampleData& s = *params_.sample;
        if (s.streaming && s.stream) {
            // Streaming path: decode from the disk->ring reader (allocation
            // free; falls back to zeros when the ring is short).
            s.stream->decodeFrame(frameIndex, scratch);
            const float scale = 1.0f / float(1 << (s.stream->header().bitDepth - 1));
            const int count = s.blockSize * s.channels;
            for (int i = 0; i < count; ++i) dst[i] = float(scratch[i]) * scale;
        } else {
            s.file.decodeFrame(frameIndex, scratch);
            const float scale = 1.0f / float(1 << (s.file.header().bitDepth - 1));
            const int count = s.blockSize * s.channels;
            for (int i = 0; i < count; ++i) dst[i] = float(scratch[i]) * scale;
        }
        chunkIndex_[slot] = frameIndex;
    }
    return dst;
}

void Voice::render(float* const* jacks, int from, int n, int32_t* scratch, const SlotRouting* live) {
    if (!active_ || !params_.sample) return;
    const SampleData& s = *params_.sample;
    const int ch = s.channels;
    const int frames = s.frames;
    const int bs = s.blockSize;
    const double rate = std::max(0.01f, params_.rate);
    // Follow live patch / bleed / ambience edits while the slot still holds
    // the piece this voice was started with; otherwise keep the frozen routes.
    const bool useLive = live && live->piece == params_.piece;
    const ChannelRoute* routes = useLive ? live->routes.data() : params_.routes.data();
    const int routeCount = std::min(useLive ? live->routeCount : params_.routeCount, ch);
    const float lpA = params_.lpCoeff;
    const float wet = params_.dampMix;
    const float amb = params_.ambScale;

    for (int i = from; i < from + n; ++i) {
        if (pos_ >= double(frames - 1)) {
            active_ = false;
            break;
        }
        const int i0 = int(pos_);
        const float frac = float(pos_ - i0);
        const int k = i0 / bs;
        const int off = i0 - k * bs;
        const float* a = frameData(k, scratch) + size_t(off) * ch;
        const float* b = off + 1 < bs ? a + ch : frameData(k + 1, scratch); // other buffer slot
        const float g = params_.gain * env_;
        for (int c = 0; c < routeCount; ++c) {
            const ChannelRoute& r = routes[c];
            if (r.target < 0) continue;
            float v = a[c] + (b[c] - a[c]) * frac;
            if (wet > 0.0f) {
                float& st = lpState_[size_t(c)];
                st += lpA * (v - st);
                v += wet * (st - v);
            }
            jacks[r.target][i] += v * g * r.gain * (r.ambient ? amb : 1.0f);
        }
        pos_ += rate;
        if (releasing_) {
            env_ -= releaseStep_;
            if (env_ <= 0.0f) {
                env_ = 0.0f;
                active_ = false;
                break;
            }
        }
    }
}

// --- Engine --------------------------------------------------------------

Engine::Engine() {
    scratch_.assign(size_t(kMaxBlockSize) * kMaxChannels, 0);
    jackStorage_.assign(size_t(kNumStrips) * 2 * kChunk, 0.0f);
    for (size_t j = 0; j < jacks_.size(); ++j) jacks_[j] = jackStorage_.data() + j * kChunk;
    for (auto& v : voices_) v.allocate();
    loader_ = std::thread([this] { loaderMain(); });
}

Engine::~Engine() {
    quit_.store(true);
    loaderCv_.notify_all();
    if (loader_.joinable()) loader_.join();
}

void Engine::setLibraries(std::vector<bfdmodel::Library> libs) {
    libs_ = std::move(libs);
    rebuild();
}

namespace {
// Same pieces in the same slots with the same articulation settings: only
// routing-level parameters changed, so no layer re-resolution is needed.
bool sameStructure(const bfdmodel::Kit& a, const bfdmodel::Kit& b) {
    if (a.pieces.size() != b.pieces.size()) return false;
    for (size_t i = 0; i < a.pieces.size(); ++i) {
        const auto& pa = a.pieces[i];
        const auto& pb = b.pieces[i];
        if (pa.slotRef != pb.slotRef || pa.arts.size() != pb.arts.size()) return false;
        for (size_t j = 0; j < pa.arts.size(); ++j) {
            const auto& x = pa.arts[j];
            const auto& y = pb.arts[j];
            if (x.articName != y.articName || x.v2p != y.v2p || x.v2a != y.v2a || x.aTrim != y.aTrim ||
                x.v2d != y.v2d)
                return false;
        }
    }
    return true;
}
} // namespace

void Engine::setKit(bfdmodel::Kit kit) {
    const bool structural = !sameStructure(kit_, kit);
    kit_ = std::move(kit);
    if (structural) rebuild();
    else updateRouting();
}

void Engine::setKeymap(std::vector<bfdmodel::KeymapEntry> map) {
    keymap_ = std::move(map);
    rebuild();
}

void Engine::setMixer(MixerState mixer) {
    mixer_ = std::move(mixer);
    updateRouting();
}

PieceLookup Engine::pieceLookup() const {
    std::map<std::string, const Piece*> byRef;
    for (const auto& lib : libs_)
        for (const auto& p : lib.pieces) byRef.emplace(bfdmodel::pieceSlotKey(lib, *p), p.get());
    PieceLookup out(kit_.pieces.size(), nullptr);
    for (size_t i = 0; i < kit_.pieces.size(); ++i) {
        std::string ref = kit_.pieces[i].slotRef;
        for (auto& c : ref)
            if (c == '\\') c = '/';
        auto it = byRef.find(ref);
        if (it != byRef.end()) out[i] = it->second;
    }
    return out;
}

void Engine::updateRouting() {
    auto r = std::make_shared<Routing>();
    const PieceLookup pieces = pieceLookup();
    r->slots.resize(kit_.pieces.size());
    for (size_t i = 0; i < kit_.pieces.size(); ++i) {
        SlotRouting& sr = r->slots[i];
        const KitPieceState& st = kit_.pieces[i];
        sr.piece = pieces[i];
        sr.trim = st.trim;
        sr.tune = st.tune;
        sr.dyn = st.dyn;
        sr.dampAmt = st.dampAmt;
        sr.dampFreq = st.dampFreq;
        sr.mute = st.mute;
        const auto routes = buildRoutes(kit_, int(i), pieces, mixer_);
        sr.routeCount = int(std::min<size_t>(routes.size(), kMaxChannels));
        std::copy_n(routes.begin(), sr.routeCount, sr.routes.begin());
    }
    const auto mics = listMics(kit_, pieces);
    for (int s = 0; s < kNumStrips; ++s) {
        r->modes[size_t(s)] = mixer_.mode(s, mics);
        r->strips[size_t(s)] = mixer_.strips[size_t(s)];
        r->anySolo = r->anySolo || (mixer_.strips[size_t(s)].solo && r->modes[size_t(s)] != StripMode::Empty);
    }
    r->masterGain = mixer_.masterGain;

    std::shared_ptr<Routing> garbage, oldPending;
    std::lock_guard<std::mutex> lock(swapMutex_);
    garbage = std::move(retiredRouting_);
    oldPending = std::move(pendingRouting_);
    pendingRouting_ = std::move(r);
}

const Piece* Engine::findPiece(const std::string& slotRef) const {
    for (const auto& lib : libs_)
        for (const auto& p : lib.pieces)
            if (bfdmodel::pieceSlotKey(lib, *p) == slotRef) return p.get();
    return nullptr;
}

// BFDArticTweaks.xml often keeps stale ".wav" names while the files on disk
// are ".bfdlac" (documented in analysis/docs/01). Resolve the real file.
std::string resolveLayerPath(const std::string& dir, const std::string& fileName) {
    namespace fs = std::filesystem;
    const std::string direct = dir + "/" + fileName;
    std::error_code ec;
    if (fs::exists(direct, ec)) return direct;
    const auto dot = fileName.find_last_of('.');
    if (dot == std::string::npos) return direct;
    for (const char* ext : {".bfdlac", ".BFDLAC", ".wav", ".WAV"}) {
        const std::string alt = dir + "/" + fileName.substr(0, dot) + ext;
        if (fs::exists(alt, ec)) return alt;
    }
    return direct;
}

namespace {
// Kit slot references use "group\piece" (BFD3) or "group/piece" (BFD2).
std::string normalizeRef(std::string ref) {
    for (auto& ch : ref)
        if (ch == '\\') ch = '/';
    return ref;
}

const Articulation* resolveArticulation(const Piece& piece, const std::string& wanted) {
    if (const Articulation* a = piece.findArticulation(wanted)) return a;
    // Different piece classes use different articulation vocabularies
    // (crash/ride: Bow/Edge/Bell, hats: ClosedT/OpenT/Pedal, ...).
    static const char* kFallbacks[] = {"Hit",  "Bow",      "Edge",   "ClosedT", "ClosedS",
                                       "Tip",  "Stick",    "Pedal",  "Bell",    "Center"};
    for (const char* name : kFallbacks)
        if (const Articulation* a = piece.findArticulation(name)) return a;
    return piece.articulations.empty() ? nullptr : &piece.articulations.front();
}

// Preload order inside one articulation: loudest, softest, then bisection,
// so the nearest-loaded fallback is never far from the requested layer.
std::vector<int> spreadOrder(int n) {
    std::vector<int> out;
    if (n <= 0) return out;
    std::vector<char> seen(size_t(n), 0);
    auto add = [&](int i) {
        if (i >= 0 && i < n && !seen[size_t(i)]) {
            seen[size_t(i)] = 1;
            out.push_back(i);
        }
    };
    add(n - 1);
    add(0);
    for (int step = n; step > 1; step = (step + 1) / 2)
        for (int i = step / 2; i < n; i += std::max(1, step)) add(i);
    for (int i = 0; i < n; ++i) add(i);
    return out;
}
} // namespace

void Engine::rebuild() {
    auto rk = std::make_shared<ResolvedKit>();

    std::map<std::string, std::shared_ptr<const Piece>> pieces;
    for (const auto& lib : libs_)
        for (const auto& p : lib.pieces) pieces.emplace(bfdmodel::pieceSlotKey(lib, *p), p);

    rk->slots.resize(kit_.pieces.size());
    for (size_t i = 0; i < kit_.pieces.size(); ++i) {
        ResolvedSlot& rs = rk->slots[i];
        rs.state = kit_.pieces[i];
        if (rs.state.slotRef.empty()) continue;
        auto it = pieces.find(normalizeRef(rs.state.slotRef));
        if (it == pieces.end()) continue;
        rs.piece = it->second;
    }

    // Keymap: the explicit one (preset MIDI map), else GM from piece kinds.
    {
        PieceLookup lookup(rk->slots.size(), nullptr);
        for (size_t i = 0; i < rk->slots.size(); ++i) lookup[i] = rk->slots[i].piece.get();
        activeKeymap_ = keymap_.empty() ? bfdmodel::autoKeymap(kit_, lookup) : keymap_;
        for (auto& k : activeKeymap_) {
            if (k.articIndex < 0 || k.slot < 0 || size_t(k.slot) >= lookup.size() || !lookup[size_t(k.slot)])
                continue;
            const auto& order = lookup[size_t(k.slot)]->articOrder;
            if (size_t(k.articIndex) < order.size()) k.articulation = order[size_t(k.articIndex)];
        }
    }

    // Only articulations reachable from the keymap are resolved and loaded.
    std::map<std::pair<int, const Articulation*>, int> articIndex;
    for (const auto& k : activeKeymap_) {
        if (k.note < 0 || k.note > 127 || k.slot < 0 || size_t(k.slot) >= rk->slots.size()) continue;
        const ResolvedSlot& rs = rk->slots[size_t(k.slot)];
        if (!rs.piece) continue;
        const Articulation* art = resolveArticulation(*rs.piece, k.articulation);
        if (!art || art->layers.empty()) continue;
        auto [it, inserted] = articIndex.emplace(std::make_pair(k.slot, art), int(rk->artics.size()));
        if (inserted) {
            ResolvedArtic ra;
            ra.art = art;
            ra.count = int(art->layers.size());
            ra.trimDb = art->trimDb;
            for (const auto& a : rs.state.arts) {
                if (a.articName != art->name) continue;
                ra.trimDb += (a.aTrim - 0.5f) * 24.0f;
                ra.v2p = std::clamp((a.v2p - 0.5f) * 2.0f, -1.0f, 1.0f); // 0.5 = off
                ra.v2a = std::clamp(a.v2a, 0.0f, 1.0f);
            }
            // Resolve round-robin groups: group 0 is always the primary layer
            // list; further groups are the Alt/ (or named rrGroups) sets.
            if (art->rrGroups.empty()) {
                ResolvedArtic::RrSpan sp;
                sp.group = nullptr; // primary
                sp.first = 0;
                sp.count = ra.count;
                ra.rrGroups.push_back(sp);
            } else {
                for (const auto& g : art->rrGroups) {
                    ResolvedArtic::RrSpan sp;
                    sp.group = &g;
                    sp.first = 0; // filled below once layer offsets are known
                    sp.count = int(g.layers.size());
                    ra.rrGroups.push_back(sp);
                }
            }
            rk->artics.push_back(ra);
        }
        rk->notes[size_t(k.note)] = {k.slot, it->second};
    }

    // Allocate one contiguous layer span per round-robin group so that
    // group g of articulation a occupies layers[ra.rrGroups[g].first .. +count).
    int total = 0;
    for (auto& ra : rk->artics) {
        ra.first = total;
        int spanTotal = 0;
        for (auto& sp : ra.rrGroups) {
            sp.first = total + spanTotal;
            spanTotal += sp.count;
        }
        // fall back to the primary layer list when no groups were resolved
        if (ra.rrGroups.empty()) {
            ResolvedArtic::RrSpan sp;
            sp.first = total;
            sp.count = ra.count;
            ra.rrGroups.push_back(sp);
            spanTotal = ra.count;
        }
        ra.count = spanTotal;
        total += spanTotal;
    }
    rk->layerCount = total;
    rk->layers = std::make_unique<LayerSlot[]>(size_t(std::max(1, total)));
    rk->rrCursor.assign(rk->artics.size(), 0);
    int readyNow = 0;
    for (const auto& ra : rk->artics) {
        for (const auto& sp : ra.rrGroups) {
            const std::vector<VelLayer>* layers = nullptr;
            if (sp.group) layers = &sp.group->layers;
            else layers = &ra.art->layers;
            for (int i = 0; i < sp.count && i < int(layers->size()); ++i) {
                const VelLayer& l = (*layers)[size_t(i)];
                const std::string dir = sp.group ? sp.group->directory : ra.art->directory;
                const std::string key = dir + '\n' + l.fileName;
                auto memo = pathMemo_.find(key);
                if (memo == pathMemo_.end())
                    memo = pathMemo_.emplace(key, resolveLayerPath(dir, l.fileName)).first;
                LayerSlot& slot = rk->layers[size_t(sp.first + i)];
                slot.path = memo->second;
                if (auto s = cache_.find(slot.path)) {
                    slot.sample = std::move(s);
                    slot.ready.store(true, std::memory_order_release);
                    ++readyNow;
                }
            }
        }
    }
    rk->ready.store(readyNow);
    rk->done.store(readyNow);

    // Round-robin across articulations so every mapped piece becomes playable early.
    // Within one articulation, interleave the RR groups so the first hit of
    // every group is available before deeper layers of any single group.
    std::vector<std::vector<int>> orders;
    for (const auto& ra : rk->artics) {
        std::vector<int> order;
        std::vector<char> seen(size_t(std::max(0, ra.count)), 0);
        auto add = [&](int i) {
            if (i >= 0 && i < ra.count && !seen[size_t(i)]) {
                seen[size_t(i)] = 1;
                order.push_back(i);
            }
        };
        // per-group spread order, interleaved across groups
        std::vector<std::vector<int>> perGroup;
        for (const auto& sp : ra.rrGroups) perGroup.push_back(spreadOrder(sp.count));
        for (size_t round = 0;; ++round) {
            bool any = false;
            for (size_t g = 0; g < perGroup.size(); ++g) {
                if (round >= perGroup[g].size()) continue;
                any = true;
                add(ra.rrGroups[g].first + perGroup[g][round] - ra.rrGroups[g].first);
            }
            if (!any) break;
        }
        orders.push_back(std::move(order));
    }
    for (size_t round = 0;; ++round) {
        bool any = false;
        for (size_t a = 0; a < orders.size(); ++a) {
            if (round >= orders[a].size()) continue;
            any = true;
            rk->loadOrder.push_back(rk->artics[a].first + orders[a][round]);
        }
        if (!any) break;
    }

    layersTotal_.store(total);
    layersReady_.store(readyNow);

    std::shared_ptr<ResolvedKit> garbage, oldPending;
    {
        std::lock_guard<std::mutex> lock(swapMutex_);
        garbage = std::move(retiredKit_);
        oldPending = std::move(pendingKit_);
        pendingKit_ = rk;
    }
    {
        std::lock_guard<std::mutex> lock(loaderMutex_);
        loaderKit_ = rk;
    }
    loaderCv_.notify_all();
    updateRouting();
    // `garbage` / `oldPending` are released here, on the message thread.
}

bool Engine::waitUntilLoaded(int timeoutMs) const {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(loaderMutex_);
            if (!loaderKit_ || loaderKit_->done.load() >= loaderKit_->layerCount) return true;
        }
        // Preload pauses at the cache budget; the rest loads on demand.
        if (cache_.bytesUsed() >= cache_.budget()) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void Engine::loaderMain() {
    while (!quit_.load()) {
        std::shared_ptr<ResolvedKit> k;
        {
            std::lock_guard<std::mutex> lock(loaderMutex_);
            k = loaderKit_;
        }
        loaderWake_.store(false);
        int pick = -1;
        if (k) {
            // 1) layers the audio thread asked for
            for (int i = 0; i < k->layerCount && pick < 0; ++i) {
                LayerSlot& s = k->layers[size_t(i)];
                if (s.wanted.load(std::memory_order_relaxed) && !s.failed &&
                    !s.ready.load(std::memory_order_acquire))
                    pick = i;
            }
            // 2) background preload, within the cache budget
            if (pick < 0 && cache_.bytesUsed() < cache_.budget()) {
                while (k->cursor < k->loadOrder.size()) {
                    const int i = k->loadOrder[k->cursor];
                    LayerSlot& s = k->layers[size_t(i)];
                    if (!s.failed && !s.ready.load(std::memory_order_acquire)) {
                        pick = i;
                        break;
                    }
                    ++k->cursor;
                }
            }
        }
        if (pick < 0) {
            std::unique_lock<std::mutex> lock(loaderMutex_);
            loaderCv_.wait_for(lock, std::chrono::milliseconds(2), [&] {
                return quit_.load() || loaderWake_.load() || loaderKit_ != k;
            });
            continue;
        }
        LayerSlot& s = k->layers[size_t(pick)];
        const bool wantStream = streamingEnabled_.load(std::memory_order_relaxed);
        if (auto data = cache_.get(s.path, wantStream)) {
            s.sample = std::move(data);
            s.ready.store(true, std::memory_order_release);
            k->ready.fetch_add(1);
            // Streaming: prime the decode ring with look-ahead frames so the
            // audio thread never waits on the disk.
            if (s.sample->streaming && s.sample->stream) {
                const int look = streamLookahead_.load(std::memory_order_relaxed);
                s.sample->stream->readFrame(0, look);
            }
        } else {
            s.failed = true;
        }
        k->done.fetch_add(1);
        // Streaming top-up: any ready streamed layer whose ring is short gets
        // refilled here (the audio thread only flags `wanted`).
        if (wantStream) {
            for (int i = 0; i < k->layerCount; ++i) {
                LayerSlot& ls = k->layers[size_t(i)];
                if (!ls.ready.load(std::memory_order_acquire) || !ls.sample) continue;
                if (!ls.sample->streaming || !ls.sample->stream) continue;
                if (!ls.wanted.load(std::memory_order_relaxed)) continue;
                const int look = streamLookahead_.load(std::memory_order_relaxed);
                if (ls.sample->stream->bufferedFrames() >= look) {
                    ls.wanted.store(false, std::memory_order_relaxed);
                    continue;
                }
                // decode the next batch of frames into the ring
                const bfdlac::Header& h = ls.sample->stream->header();
                const int cur = int(ls.sample->stream->ring().readIndex() / uint64_t(std::max(1, h.blockSize)));
                ls.sample->stream->readFrame(cur, look);
                if (ls.sample->stream->bufferedFrames() >= look)
                    ls.wanted.store(false, std::memory_order_relaxed);
            }
        }
        std::lock_guard<std::mutex> lock(loaderMutex_);
        if (loaderKit_ == k) layersReady_.store(k->ready.load());
    }
}

void Engine::trigger(ResolvedKit& kit, const Routing& routing, const NoteEvent& ev) {
    if (ev.note < 0 || ev.note > 127) return;
    const NoteTarget t = kit.notes[size_t(ev.note)];
    if (t.slot < 0 || t.artic < 0) return;
    const ResolvedSlot& rs = kit.slots[size_t(t.slot)];
    if (!rs.piece) return;
    const SlotRouting* sr = size_t(t.slot) < routing.slots.size() ? &routing.slots[size_t(t.slot)] : nullptr;
    if (!sr || sr->mute || sr->piece != rs.piece.get()) return;
    const ResolvedArtic& ra = kit.artics[size_t(t.artic)];
    const float velocity = std::clamp(ev.velocity, 0.0f, 1.0f);

    // Round-robin: cycle through the articulation's RR groups on every hit.
    // Group 0 is the primary set; further groups are the Alt/ sets. The
    // cursor advances per articulation so successive hits rotate.
    const int nGroups = ra.rrCount();
    uint32_t& cursor = kit.rrCursor[size_t(t.artic)];
    const int rrGroup = nGroups <= 1 ? 0 : int(cursor % uint32_t(nGroups));
    cursor += 1;
    static const ResolvedArtic::RrSpan kEmptySpan{};
    const ResolvedArtic::RrSpan& span =
        ra.rrGroups.empty() ? kEmptySpan : ra.rrGroups[size_t(rrGroup)];

    // Pick the velocity layer inside the chosen RR group.
    const std::vector<VelLayer>* layers =
        span.group ? &span.group->layers : &ra.art->layers;
    int want = -1;
    if (!layers->empty()) {
        for (size_t i = 0; i < layers->size(); ++i)
            if (velocity >= (*layers)[i].velLo && velocity < (*layers)[i].velHi) {
                want = int(i);
                break;
            }
        if (want < 0) want = int(layers->size()) - 1;
    }
    if (want < 0) return;

    // Never touch the disk here: use the requested layer if resident, else
    // the nearest resident one *within the same RR group*, and ask the loader
    // for the missing layer.
    int idx = -1;
    const int groupFirst = span.first;
    const int groupCount = span.count;
    LayerSlot& wanted = kit.layers[size_t(groupFirst + want)];
    if (wanted.ready.load(std::memory_order_acquire)) {
        idx = want;
    } else {
        if (!wanted.wanted.exchange(true, std::memory_order_relaxed)) loaderWake_.store(true);
        for (int d = 1; d < groupCount && idx < 0; ++d) {
            if (want + d < groupCount && kit.layers[size_t(groupFirst + want + d)].ready.load(std::memory_order_acquire))
                idx = want + d;
            else if (want - d >= 0 && kit.layers[size_t(groupFirst + want - d)].ready.load(std::memory_order_acquire))
                idx = want - d;
        }
    }
    if (idx < 0) return;
    const LayerSlot& chosen = kit.layers[size_t(groupFirst + idx)];
    const VelLayer& layer = (*layers)[size_t(idx)];
    const auto& sample = chosen.sample;
    // Streaming: request more look-ahead on the loader thread. The loader
    // thread refills the ring as it drains; this flag is a cheap hint.
    if (sample && sample->streaming && sample->stream) {
        const int look = streamLookahead_.load(std::memory_order_relaxed);
        if (sample->stream->bufferedFrames() < look) {
            if (!wanted.wanted.exchange(true, std::memory_order_relaxed)) loaderWake_.store(true);
        }
    }
    const double sr44 = sampleRate_.load();

    VoiceParams vp;
    vp.sample = sample;
    vp.piece = sr->piece;
    vp.slot = t.slot;
    vp.rrGroup = rrGroup;
    vp.routes = sr->routes;
    vp.routeCount = sr->routeCount;

    // Level staging: place the layer's coherent channel-sum peak on the
    // velocity target curve (the mics see the same hit in phase, so the sum -
    // not any single channel - sets the output peak). A full-velocity hit
    // lands at kHeadroomDb so a whole kit stacks without clipping the Mix bus.
    const float dyn = std::clamp(sr->dyn, 0.0f, 1.0f);
    const float targetDb = kHeadroomDb - (1.0f - velocity) * 30.0f * dyn;
    const float peakDb = 20.0f * std::log10(std::max(sample->sumPeak, 1.0e-9f));
    float gainDb = targetDb - peakDb;
    gainDb += layer.dbTrim + ra.trimDb;
    gainDb += (sr->trim - 0.8f) * 12.0f; // kit trim knob (0.8 = unity)
    vp.gain = std::pow(10.0f, gainDb / 20.0f);

    // tune knob (0.5 = centre, +-1 semitone) and bipolar velocity->pitch
    const float semis = (sr->tune - 0.5f) * 2.0f + (velocity - 0.5f) * ra.v2p * 2.0f;
    vp.rate = float(std::pow(2.0, semis / 12.0) * double(sample->sampleRate) / std::max(1.0, sr44));

    if (sr->dampAmt > 0.0f) {
        const float f = std::clamp(sr->dampFreq, 20.0f, 18000.0f);
        vp.lpCoeff = std::clamp(1.0f - std::exp(-2.0f * float(M_PI) * f / float(sr44)), 0.0f, 1.0f);
        vp.dampMix = std::clamp(sr->dampAmt, 0.0f, 1.0f);
    }

    // v2a: velocity scales the ambience share
    vp.ambScale = 1.0f - ra.v2a * (1.0f - velocity) * 0.7f;

    // Per-piece polyphony: fast repeats fade the oldest ring-out instead of
    // piling up (and instead of hard-stealing unrelated voices later).
    int same = 0;
    Voice* oldestSame = nullptr;
    for (auto& v : voices_) {
        if (!v.active() || v.releasing() || v.slot() != t.slot) continue;
        ++same;
        if (!oldestSame || v.age() < oldestSame->age()) oldestSame = &v;
    }
    if (same >= kMaxVoicesPerSlot && oldestSame) oldestSame->stop(0.03f, sr44);

    Voice* target = nullptr;
    for (auto& v : voices_)
        if (!v.active()) {
            target = &v;
            break;
        }
    if (!target) // reuse the oldest fading voice, then the oldest voice overall
        for (auto& v : voices_)
            if (v.releasing() && (!target || v.age() < target->age())) target = &v;
    if (!target)
        for (auto& v : voices_)
            if (!target || v.age() < target->age()) target = &v;
    target->start(vp, ++voiceAge_);
    ++rr_;
    if (t.slot < bfdmodel::Kit::kMaxSlots) {
        auto& hit = slotHits_[size_t(t.slot)];
        if (velocity > hit.load(std::memory_order_relaxed)) hit.store(std::max(velocity, 0.05f), std::memory_order_relaxed);
    }
}

namespace {
inline void notePeak(std::atomic<float>& meter, float peak) {
    if (peak > meter.load(std::memory_order_relaxed)) meter.store(peak, std::memory_order_relaxed);
}
} // namespace

void Engine::mixStrips(const Routing& routing, float* const* outs, int offset, int n) {
    float* masterL = outs[0] + offset;
    float* masterR = outs[1] + offset;
    const float invN = 1.0f / float(std::max(1, n));
    for (int s = 0; s < kNumStrips; ++s) {
        const StripMode mode = routing.modes[size_t(s)];
        const StripState& st = routing.strips[size_t(s)];
        const bool audible = mode != StripMode::Empty && !st.mute && (!routing.anySolo || st.solo);
        float tl = 0.0f, tr = 0.0f;
        if (audible) {
            const float g = faderGain(st.gain);
            const float pan = std::clamp(st.pan, 0.0f, 1.0f);
            if (mode == StripMode::MonoL || mode == StripMode::MonoR) {
                // constant power, -3 dB at centre
                tl = g * std::cos(pan * float(M_PI_2));
                tr = g * std::sin(pan * float(M_PI_2));
            } else {
                tl = g * std::min(1.0f, 2.0f * (1.0f - pan)); // balance
                tr = g * std::min(1.0f, 2.0f * pan);
            }
        }
        float& gl = stripGain_[size_t(s) * 2];
        float& gr = stripGain_[size_t(s) * 2 + 1];
        float* outL = outs[2 + s * 2] ? outs[2 + s * 2] + offset : nullptr;
        float* outR = outs[3 + s * 2] ? outs[3 + s * 2] + offset : nullptr;
        if (mode == StripMode::Empty || (tl == 0.0f && tr == 0.0f && gl == 0.0f && gr == 0.0f)) {
            gl = tl;
            gr = tr;
            if (outL) std::memset(outL, 0, sizeof(float) * size_t(n));
            if (outR) std::memset(outR, 0, sizeof(float) * size_t(n));
            continue;
        }
        const float dl = (tl - gl) * invN, dr = (tr - gr) * invN;
        const float* inL = jacks_[size_t(s) * 2];
        const float* inR = jacks_[size_t(s) * 2 + 1];
        float peakL = 0.0f, peakR = 0.0f;
        for (int i = 0; i < n; ++i) {
            gl += dl;
            gr += dr;
            float a, b;
            switch (mode) {
                case StripMode::MonoL: a = b = inL[i]; break;
                case StripMode::MonoR: a = b = inR[i]; break;
                case StripMode::MidSide: a = inL[i] + inR[i]; b = inL[i] - inR[i]; break;
                default: a = inL[i]; b = inR[i]; break;
            }
            const float l = a * gl, r = b * gr;
            if (outL) outL[i] = l;
            if (outR) outR[i] = r;
            masterL[i] += l;
            masterR[i] += r;
            peakL = std::max(peakL, std::abs(l));
            peakR = std::max(peakR, std::abs(r));
        }
        gl = tl;
        gr = tr;
        notePeak(meters_[size_t(2 + s * 2)], peakL);
        notePeak(meters_[size_t(3 + s * 2)], peakR);
    }

    const float target = faderGain(routing.masterGain);
    if (masterGainNow_ < 0.0f) masterGainNow_ = target;
    const float dm = (target - masterGainNow_) * invN;
    float peakL = 0.0f, peakR = 0.0f;
    for (int i = 0; i < n; ++i) {
        masterGainNow_ += dm;
        masterL[i] = softClip(masterL[i] * masterGainNow_);
        masterR[i] = softClip(masterR[i] * masterGainNow_);
        peakL = std::max(peakL, std::abs(masterL[i]));
        peakR = std::max(peakR, std::abs(masterR[i]));
    }
    masterGainNow_ = target;
    notePeak(meters_[0], peakL);
    notePeak(meters_[1], peakR);
}

void Engine::process(const NoteEvent* events, int numEvents, float* const* outs, int n) {
    std::memset(outs[0], 0, sizeof(float) * size_t(n));
    std::memset(outs[1], 0, sizeof(float) * size_t(n));

    // Pick up a newly resolved kit / routing without ever blocking; the
    // previous objects are parked for the message thread to free.
    if (swapMutex_.try_lock()) {
        if (pendingKit_ && !retiredKit_) {
            retiredKit_ = std::move(activeKit_);
            activeKit_ = std::move(pendingKit_);
        }
        if (pendingRouting_ && !retiredRouting_) {
            retiredRouting_ = std::move(activeRouting_);
            activeRouting_ = std::move(pendingRouting_);
        }
        swapMutex_.unlock();
    }
    ResolvedKit* kit = activeKit_.get();
    const Routing* routing = activeRouting_.get();
    if (!routing) {
        for (int s = 0; s < kNumStrips; ++s)
            for (int c = 0; c < 2; ++c)
                if (outs[2 + s * 2 + c]) std::memset(outs[2 + s * 2 + c], 0, sizeof(float) * size_t(n));
        return;
    }

    int e = 0;
    for (int done = 0; done < n; done += kChunk) {
        const int len = std::min(kChunk, n - done);
        std::memset(jackStorage_.data(), 0, sizeof(float) * jackStorage_.size());

        auto renderVoices = [&](int from, int to) {
            if (to <= from) return;
            for (auto& v : voices_) {
                if (!v.active()) continue;
                const int slot = v.slot();
                const SlotRouting* live =
                    slot >= 0 && size_t(slot) < routing->slots.size() ? &routing->slots[size_t(slot)] : nullptr;
                v.render(jacks_.data(), from, to - from, scratch_.data(), live);
            }
        };

        // Sample-accurate triggering: render up to each event, then start it.
        int cursor = 0;
        for (; e < numEvents && events[e].offset < done + len; ++e) {
            const NoteEvent& ev = events[e];
            if (!ev.isOn) continue; // one-shot drums: note-off never cuts a hit
            const int at = std::clamp(ev.offset - done, cursor, len);
            renderVoices(cursor, at);
            cursor = at;
            if (kit) trigger(*kit, *routing, ev);
        }
        renderVoices(cursor, len);
        mixStrips(*routing, outs, done, len);
    }
    int active = 0;
    for (const auto& v : voices_) active += v.active() ? 1 : 0;
    activeVoices_.store(active, std::memory_order_relaxed);
}

} // namespace bfdengine
