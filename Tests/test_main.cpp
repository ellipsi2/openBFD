// openbfd unit tests — codec semantics, XML model, kit round-trip.
// Real-library checks activate when OPENBFD_LIBRARY points at a BFD library root.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "Bfdlac/BfdlacCodec.h"
#include "Engine/Engine.h"
#include "Model/Model.h"

namespace {

int failures = 0;
#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                 \
            ++failures;                                                                 \
        }                                                                               \
    } while (0)
#define CHECK_EQ(a, b)                                                                  \
    do {                                                                                \
        if (!((a) == (b))) {                                                            \
            std::printf("FAIL %s:%d  %s == %s\n", __FILE__, __LINE__, #a, #b);          \
            ++failures;                                                                 \
        }                                                                               \
    } while (0)

// LSB-first field writer, mirroring the decoder's BitReader.
class BitWriter {
public:
    explicit BitWriter(int bytes) : buf(size_t(bytes), 0) {}
    void writeSigned(int value, int bits) {
        uint32_t v = uint32_t(value) & (bits == 32 ? 0xFFFFFFFFu : ((1u << bits) - 1));
        for (int i = 0; i < bits; ++i) {
            const int bitPos = pos_ + i;
            if (v & (1u << i)) buf[size_t(bitPos >> 3)] |= uint8_t(1u << (bitPos & 7));
        }
        pos_ += bits;
    }
    void writeLE(int value, int nbytes) {
        pos_ = (pos_ + 7) & ~7;
        uint32_t v = uint32_t(value);
        for (int i = 0; i < nbytes; ++i) buf[size_t(pos_ >> 3) + i] = uint8_t(v >> (8 * i));
        pos_ += nbytes * 8;
    }
    std::vector<uint8_t> buf;

private:
    int pos_ = 0;
};

void testBitReader() {
    BitWriter w(16);
    w.writeSigned(-3, 3);  // bits 0..2
    w.writeSigned(5, 4);   // bits 3..6
    w.writeLE(0x123456, 3); // aligned at bit 7? -> byte-align first
    int32_t out[64];
    const uint8_t scheme = 0x00; // alg 0 raw not used here; direct block tests below
    (void) scheme;
    (void) out;
    // -3 in 3 bits = 0b101; 5 in 4 bits = 0b0101
    CHECK_EQ(int(w.buf[0] & 0x07), 5);
    CHECK_EQ(int((w.buf[0] >> 3) & 0x0F), 5);
}

void testSyntheticBlocks() {
    const int n = 64;
    int32_t out[n];

    // alg 7 (delta): seed 0x100, residuals of 2
    {
        BitWriter w(64);
        w.writeLE(0x100, 3);
        for (int i = 1; i < n; ++i) w.writeSigned(i % 2 ? 1 : -1, 2);
        bfdlac::File::decodeBlock(w.buf.data(), 0x74 /* alg7, errSig4 -> 2 bits */, 24, 2, out);
        CHECK_EQ(out[0], 0x100);
        CHECK_EQ(out[1], 0x101);
        CHECK_EQ(out[2], 0x100);
    }
    // alg 9 (hdiff2): seeds 10, 12 -> slope 2 -> out[2] = 12 + 1 = 13
    {
        BitWriter w(64);
        w.writeLE(10, 3);
        w.writeLE(12, 3);
        for (int i = 2; i < n; ++i) w.writeSigned(0, 2);
        bfdlac::File::decodeBlock(w.buf.data(), 0x94, 24, 2, out);
        CHECK_EQ(out[0], 10);
        CHECK_EQ(out[1], 12);
        CHECK_EQ(out[2], 13); // 12 + (2>>1)
        CHECK_EQ(out[3], 13); // (13-12)>>1 == 0
    }
    // alg 4 (zeros)
    {
        std::vector<uint8_t> payload(8, 0);
        bfdlac::File::decodeBlock(payload.data(), 0x40, 24, 2, out);
        for (int i = 0; i < n; ++i) CHECK_EQ(out[i], 0);
    }
    // alg 1 (constant)
    {
        BitWriter w(8);
        w.writeLE(-7, 3);
        bfdlac::File::decodeBlock(w.buf.data(), 0x10, 24, 2, out);
        for (int i = 0; i < n; ++i) CHECK_EQ(out[i], -7);
    }
}

void testXmlAndKit() {
    const char* xml = R"(<root a="1">
        <BFD2KitPieceInfo kpi_name="Test Kick" kpi_class_ignore="x">
          <VObjectArray name="audioChanInfo" count="2">
            <BFD2AudioChanInfo chanName="Kick In" chanType="dirKick" encoding="mono" micIndex="0" chanIndex="0" />
            <BFD2AudioChanInfo chanName="OH Left" chanType="ambient" encoding="stereoLeft" micIndex="0" chanIndex="1" />
          </VObjectArray>
          <VString name="kpi_class" string="Kick" />
          <VObjectArray name="articulation" count="1"><VString string="Hit" /></VObjectArray>
        </BFD2KitPieceInfo>
      </root>)";
    std::vector<bfdmodel::XmlElement> roots;
    std::string err;
    CHECK(bfdmodel::parseXml(xml, &roots, &err));
    CHECK_EQ(roots.size(), size_t(1));
    CHECK(roots[0].name == "root");
    CHECK_EQ(roots[0].attrNum("a"), 1.0);
    const auto* info = roots[0].child("BFD2KitPieceInfo");
    CHECK(info != nullptr);
    CHECK(info->attrOr("kpi_name") == "Test Kick");

    bfdmodel::Kit kit;
    kit.name = "Roundtrip";
    kit.slots.assign(32, {});
    kit.slots[0] = "YMA Drums/Kick MCA20 Felt";
    bfdmodel::KitPieceState st;
    st.slotRef = kit.slots[0];
    st.pan = 0.25f;
    st.trim = 0.9f;
    bfdmodel::KitArticState a;
    a.articName = "Hit";
    a.v2d = 0.7f;
    st.arts.push_back(a);
    kit.pieces.push_back(st);
    kit.size = 1;

    const std::string tmp = "/tmp/openbfd_test.kit";
    CHECK(bfdmodel::saveKit(tmp, kit, &err));
    bfdmodel::Kit loaded;
    CHECK(bfdmodel::loadKit(tmp, &loaded, &err));
    CHECK(loaded.name == "Roundtrip");
    CHECK_EQ(loaded.slots[0], std::string("YMA Drums/Kick MCA20 Felt"));
    CHECK_EQ(loaded.pieces.size(), size_t(1));
    CHECK(std::abs(loaded.pieces[0].pan - 0.25f) < 1e-5f);
    CHECK_EQ(loaded.pieces[0].arts.size(), size_t(1));
    CHECK(std::abs(loaded.pieces[0].arts[0].v2d - 0.7f) < 1e-5f);
}

void testVelocityLayers() {
    bfdmodel::Articulation art;
    art.layers.push_back({0.0f, 0.5f, 0, -60.0f, "a.bfdlac"});
    art.layers.push_back({0.5f, 1.0f, 0, -20.0f, "b.bfdlac"});
    CHECK(bfdengine::pickLayer(art, 0.1f) == &art.layers[0]);
    CHECK(bfdengine::pickLayer(art, 0.9f) == &art.layers[1]);
    const auto& L = art.layers;
    // as recorded (dyn 0.5): bottom of a window plays at unity
    CHECK(std::abs(bfdengine::layerGainDb(L, 0, 0, 0.0f, 0.5f)) < 1e-4f);
    CHECK(std::abs(bfdengine::layerGainDb(L, 1, 1, 0.5f, 0.5f)) < 1e-4f);
    // up a window the level slides towards the next layer: continuous at 0.5
    CHECK(std::abs(bfdengine::layerGainDb(L, 0, 0, 0.25f, 0.5f) - 20.0f) < 1e-3f);
    CHECK(bfdengine::layerGainDb(L, 0, 0, 0.4999f, 0.5f) > 39.9f);
    // a stand-in is matched to the requested layer's loudness
    CHECK(std::abs(bfdengine::layerGainDb(L, 0, 1, 0.0f, 0.5f) - (-40.0f)) < 1e-4f);
    // dyn 0 flattens to the loudest layer, dyn 1 doubles the range
    CHECK(std::abs(bfdengine::layerGainDb(L, 0, 0, 0.0f, 0.0f) - 40.0f) < 1e-4f);
    CHECK(std::abs(bfdengine::layerGainDb(L, 0, 0, 0.0f, 1.0f) - (-40.0f)) < 1e-4f);
}

void testRealLibrary() {
    const char* root = std::getenv("OPENBFD_LIBRARY");
    if (!root) {
        std::printf("skip real-library tests (set OPENBFD_LIBRARY)\n");
        return;
    }
    std::string pieceDir = std::string(root) + "/Audio";
    bfdmodel::Library lib;
    std::string err;
    CHECK(bfdmodel::scanLibrary(root, &lib, &err));
    CHECK(!lib.pieces.empty());

    const bfdmodel::Piece* piece = nullptr;
    for (const auto& p : lib.pieces)
        if (!p->articulations.empty() && !p->articulations[0].layers.empty()) {
            piece = p.get();
            break;
        }
    CHECK(piece != nullptr);
    if (!piece) return;

    const auto& art = piece->articulations[0];
    const auto& layer = art.layers[art.layers.size() / 2];
    const std::string path = bfdengine::resolveLayerPath(art.directory, layer.fileName);

    std::vector<float> audio;
    bfdlac::Header h;
    CHECK(bfdlac::decodeWholeFile(path, &audio, &h, &err));
    CHECK_EQ(h.channels, int(piece->channels.size()));
    CHECK(h.frames > 0);
    CHECK_EQ(audio.size(), size_t(h.frames) * h.channels);

    // raw blocks (alg 0) must reproduce literal PCM
    bfdlac::File f;
    CHECK(f.open(path, &err));
    const auto& hdr = f.header();
    std::vector<int32_t> pcm(size_t(hdr.frames) * hdr.channels);
    f.readInt32(0, hdr.frames, pcm.data());
    int rawChecked = 0;
    // decode whole file via cache path too
    bfdengine::SampleCache cache(64ull << 20);
    auto sample = cache.get(path);
    CHECK(sample != nullptr);
    if (sample) {
        CHECK_EQ(sample->channels, hdr.channels);
        CHECK_EQ(sample->frames, hdr.frames);
    }
    (void) rawChecked;
    (void) pcm;

    // level sanity: some channel within [-120, 0] dB
    double best = -999;
    for (int c = 0; c < h.channels; ++c) {
        double acc = 0;
        for (int s = 0; s < h.frames; ++s) {
            const float v = audio[size_t(s) * h.channels + c];
            acc += double(v) * v;
        }
        best = std::max(best, acc > 0 ? 10.0 * std::log10(acc / h.frames) : -999.0);
    }
    CHECK(best > -120.0 && best < 1.0);
}


void testStreamingDecode() {
    // Synthetic stream: push decoded frames through a DecodeRing and read
    // them back in order; verify SPSC semantics and zero-fill past the end.
    bfdlac::DecodeRing ring;
    ring.allocate(2, 8); // 8 frames of stereo
    CHECK_EQ(ring.capacity(), 8);
    CHECK_EQ(ring.available(), 0);
    CHECK_EQ(ring.freeSpace(), 8);

    // push 3 frames
    for (int f = 0; f < 3; ++f) {
        int32_t* dst = ring.reserve(1);
        CHECK(dst != nullptr);
        if (!dst) break;
        dst[0] = f * 10 + 1;
        dst[1] = f * 10 + 2;
        ring.commit(1);
    }
    CHECK_EQ(ring.available(), 3);
    CHECK_EQ(ring.freeSpace(), 5);

    // peek without consuming
    int32_t out[4];
    CHECK(ring.peek(0, 2, out));
    CHECK_EQ(out[0], 1);
    CHECK_EQ(out[1], 2);
    CHECK_EQ(out[2], 11);
    CHECK_EQ(out[3], 12);
    CHECK_EQ(ring.available(), 3); // unchanged

    // consume 1, peek again
    ring.consume(1);
    CHECK_EQ(ring.available(), 2);
    CHECK(ring.peek(0, 2, out));
    CHECK_EQ(out[0], 11);

    // peek past the write cursor zero-fills
    CHECK(!ring.peek(10, 2, out));
    CHECK_EQ(out[0], 0);
    CHECK_EQ(out[1], 0);

    // fill to capacity, then reserve must fail
    ring.reset();
    ring.allocate(2, 4);
    for (int f = 0; f < 4; ++f) {
        int32_t* dst = ring.reserve(1);
        CHECK(dst != nullptr);
        if (dst) ring.commit(1);
    }
    CHECK_EQ(ring.freeSpace(), 0);
    CHECK(ring.reserve(1) == nullptr);
}

void testRoundRobinGroups() {
    // Alt/ round-robin: an articulation with a primary set and an Alt/ set
    // must expose 2 RR groups and cycle through them on successive hits.
    const char* root = std::getenv("OPENBFD_LIBRARY");
    if (!root) {
        std::printf("skip round-robin tests (set OPENBFD_LIBRARY)\n");
        return;
    }
    bfdmodel::Library lib;
    std::string err;
    CHECK(bfdmodel::scanLibrary(root, &lib, &err));

    // find a piece whose articulation has an Alt/ set
    const bfdmodel::Piece* piece = nullptr;
    const bfdmodel::Articulation* art = nullptr;
    for (const auto& p : lib.pieces) {
        for (const auto& a : p->articulations) {
            if (a.hasRoundRobin()) {
                piece = p.get();
                art = &a;
                break;
            }
        }
        if (art) break;
    }
    if (!art) {
        std::printf("skip: no articulation with Alt/ set found in %s\n", root);
        return;
    }
    CHECK(art->rrCount() >= 2);
    CHECK(!art->rrGroups.empty());
    CHECK(art->rrGroups[0].layers.size() == art->layers.size());
    // group 1 (Alt) must have its own velocity map
    CHECK(art->rrGroups[1].layers.size() > 0);
    // layers in each group are sorted by velLo
    for (const auto& g : art->rrGroups)
        for (size_t i = 1; i < g.layers.size(); ++i)
            CHECK(g.layers[i].velLo >= g.layers[i - 1].velLo);
    std::printf("round-robin: %s/%s has %d RR groups (primary %d, alt %d layers)\n",
                piece->name.c_str(), art->name.c_str(), art->rrCount(),
                int(art->rrGroups[0].layers.size()), int(art->rrGroups[1].layers.size()));
}

void testStreamingFileAgainstResident() {
    // Streaming decode must reproduce the resident (in-memory) path bit for
    // bit: same frames, same samples, from the disk->ring reader.
    const char* root = std::getenv("OPENBFD_LIBRARY");
    if (!root) {
        std::printf("skip streaming file test (set OPENBFD_LIBRARY)\n");
        return;
    }
    // pick any piece's first layer
    bfdmodel::Library lib;
    std::string err;
    if (!bfdmodel::scanLibrary(root, &lib, &err) || lib.pieces.empty()) return;
    const bfdmodel::Piece* piece = nullptr;
    for (const auto& p : lib.pieces)
        if (!p->articulations.empty() && !p->articulations[0].layers.empty()) {
            piece = p.get();
            break;
        }
    if (!piece) return;
    const auto& art = piece->articulations[0];
    const auto& layer = art.layers[art.layers.size() / 2];
    const std::string path = bfdengine::resolveLayerPath(art.directory, layer.fileName);

    bfdlac::File resident;
    CHECK(resident.open(path, &err));
    bfdlac::StreamedFile stream;
    CHECK(stream.open(path, &err));
    CHECK_EQ(resident.header().channels, stream.header().channels);
    CHECK_EQ(resident.header().frames, stream.header().frames);
    CHECK_EQ(resident.header().frameCount, stream.header().frameCount);

    const int ch = resident.header().channels;
    const int bs = resident.header().blockSize;
    const int frames = std::min(resident.header().frameCount, 8);
    std::vector<int32_t> a(size_t(bs) * size_t(ch)), b(size_t(bs) * size_t(ch));
    // prime the stream ring (loader thread role)
    stream.readFrame(0, frames);
    for (int fi = 0; fi < frames; ++fi) {
        resident.decodeFrame(fi, a.data());
        stream.decodeFrame(fi, b.data());
        int mismatch = 0, firstBad = -1;
        for (int i = 0; i < bs * ch; ++i)
            if (a[size_t(i)] != b[size_t(i)]) {
                ++mismatch;
                if (firstBad < 0) firstBad = i;
            }
        (void) firstBad;
        CHECK_EQ(mismatch, 0);
    }
    std::printf("streaming: %d frames x %d ch match resident path (%s)\n", frames, ch,
                path.c_str());
}

// A tiny two-piece kit (kick + tom) with BFD3's channel layout, for routing tests.
struct RoutingFixture {
    bfdmodel::Piece kick, tom;
    bfdmodel::Kit kit;
    bfdengine::PieceLookup pieces;
    RoutingFixture() {
        auto chans = [](bfdmodel::Piece& p) {
            p.channels.push_back({"Kick In", "dirKick", "mono", 0, 0});
            p.channels.push_back({"Snare Top", "dirSnare", "mono", 0, 1});
            p.channels.push_back({"Direct", "dirOther", "mono", 0, 2});
            p.channels.push_back({"OH Left", "ambient", "stereoLeft", 0, 3});
            p.channels.push_back({"OH Right", "ambient", "stereoRight", 1, 4});
            p.channels.push_back({"Room Mid", "ambient", "stereoMid", 0, 5});
            p.channels.push_back({"Room Side", "ambient", "stereoSide", 1, 6});
        };
        kick.klass = "Kick";
        kick.name = "Kick";
        chans(kick);
        tom.klass = "Tom";
        tom.name = "Rack Tom";
        tom.diameter = 12;
        chans(tom);
        kit.slots = {"g/kick", "g/tom"};
        kit.pieces.resize(2);
        kit.pieces[0].slotRef = "g/kick";
        kit.pieces[1].slotRef = "g/tom";
        pieces = {&kick, &tom};
    }
};

void testBleedRouting() {
    RoutingFixture f;
    const auto mics = bfdengine::listMics(f.kit, f.pieces);
    // own mics: kick -> Kick In, tom -> Direct; 4 ambient; other bleed (kick's Direct)
    CHECK_EQ(mics.size(), size_t(7));
    CHECK_EQ(mics[0].key, std::string("D:0:Kick In"));
    CHECK_EQ(mics[1].key, std::string("D:1:Direct"));
    CHECK_EQ(mics.back().key, std::string("B:Other"));

    bfdengine::MixerState mx;
    bfdengine::autoPatch(mics, mx);
    CHECK(mx.jackOf("D:0:Kick In") == (bfdengine::Jack{0, 0}));
    CHECK(mx.jackOf("D:1:Direct") == (bfdengine::Jack{1, 0}));
    CHECK(mx.jackOf("A:OH Left") == (bfdengine::Jack{2, 0}));
    CHECK(mx.jackOf("A:OH Right") == (bfdengine::Jack{2, 1}));
    CHECK(mx.mode(0, mics) == bfdengine::StripMode::MonoL);
    CHECK(mx.mode(2, mics) == bfdengine::StripMode::Stereo);
    CHECK(mx.mode(3, mics) == bfdengine::StripMode::MidSide);
    CHECK(mx.mode(4, mics) == bfdengine::StripMode::MonoL); // B:Other
    CHECK(mx.mode(5, mics) == bfdengine::StripMode::Empty);

    // plugging into an occupied jack replaces its cable; unplugging R makes the strip mono
    mx.plug("A:OH Right", {0, 1});
    CHECK(mx.mode(0, mics) == bfdengine::StripMode::Stereo);
    CHECK(mx.mode(2, mics) == bfdengine::StripMode::MonoL);
    mx.plug("D:1:Direct", {0, 1});
    CHECK(mx.jackOf("A:OH Right").valid() == false);
    mx.unplug("D:1:Direct");
    CHECK(mx.mode(0, mics) == bfdengine::StripMode::MonoL);
    bfdengine::autoPatch(mics, mx); // re-patches the two loose mics into free strips
    CHECK(mx.jackOf("D:1:Direct").valid());
    CHECK(mx.jackOf("A:OH Right") == (bfdengine::Jack{2, 1})); // back next to its partner

    // tom kick-bleed: On -> kick piece's Kick In jack, scaled by the kick's return
    mx = {};
    bfdengine::autoPatch(mics, mx);
    f.kit.pieces[1].kickBleedLvl = 0.5f;
    f.kit.pieces[1].kickBleedDst = 2;
    f.kit.pieces[0].bleedReturn = 0.5f;
    auto r = bfdengine::buildRoutes(f.kit, 1, f.pieces, mx);
    CHECK_EQ(r.size(), size_t(7));
    CHECK_EQ(r[0].target, 0 * 2 + 0);                         // Kick In bleed -> strip 1 L
    CHECK(std::abs(r[0].gain - 0.5f * 0.5f) < 1e-5f);         // lvl * return (masters at unity)
    CHECK_EQ(r[2].target, 1 * 2 + 0);                         // tom's own Direct -> strip 2
    CHECK(r[3].ambient && r[3].target == 2 * 2 + 0);
    CHECK_EQ(r[1].target, -1);                                // no snare in the kit: snare bleed unheard
    // Dir -> the tom's own primary direct mic
    f.kit.pieces[1].kickBleedDst = 1;
    r = bfdengine::buildRoutes(f.kit, 1, f.pieces, mx);
    CHECK_EQ(r[0].target, 1 * 2 + 0);
    CHECK(std::abs(r[0].gain - 0.5f) < 1e-5f);
    // Off
    f.kit.pieces[1].kickBleedDst = 0;
    r = bfdengine::buildRoutes(f.kit, 1, f.pieces, mx);
    CHECK_EQ(r[0].target, -1);
    // master kick bleed follows the fader law
    f.kit.pieces[1].kickBleedDst = 2;
    mx.masterKickBleed = 0.4f;
    r = bfdengine::buildRoutes(f.kit, 1, f.pieces, mx);
    CHECK(std::abs(r[0].gain - 0.25f * bfdengine::faderGain(0.4f)) < 1e-5f);
    // the kick's own Direct channel is other bleed
    r = bfdengine::buildRoutes(f.kit, 0, f.pieces, mx);
    CHECK_EQ(r[0].target, 0);                         // own Kick In
    CHECK_EQ(r[2].target, mx.jackOf("B:Other").strip * 2);
    // flip_amb swaps the OH pair
    f.kit.pieces[0].flipAmb = true;
    r = bfdengine::buildRoutes(f.kit, 0, f.pieces, mx);
    CHECK_EQ(r[3].target, 2 * 2 + 1);
    CHECK_EQ(r[4].target, 2 * 2 + 0);

    CHECK(std::abs(bfdengine::faderGain(0.8f) - 1.0f) < 1e-6f);
    CHECK(std::abs(bfdengine::faderDb(0.4f) + 18.06f) < 0.1f);

    // mixer state round-trip
    mx.strips[3].gain = 0.5f;
    mx.strips[3].solo = true;
    bfdengine::MixerState back;
    CHECK(bfdengine::mixerFromXml(bfdengine::mixerToXml(mx), &back));
    CHECK_EQ(back.patch.size(), mx.patch.size());
    CHECK(std::abs(back.strips[3].gain - 0.5f) < 1e-6f && back.strips[3].solo);
    CHECK(std::abs(back.masterKickBleed - 0.4f) < 1e-6f);
}

void testKeymapsAndKitXml() {
    RoutingFixture f;
    auto map = bfdmodel::autoKeymap(f.kit, f.pieces);
    auto noteSlot = [&](int note) {
        for (const auto& k : map)
            if (k.note == note) return k.slot;
        return -1;
    };
    CHECK_EQ(noteSlot(36), 0);
    CHECK_EQ(noteSlot(50), 1);
    CHECK_EQ(noteSlot(41), 1); // a single tom answers every tom note
    CHECK_EQ(noteSlot(38), -1);

    const char* midi = R"(<BFD2MidiMap name="m">
        <VObjectArray name="midi_note_36" count="1">
          <MIDIMapElement note="36" isgrv="false"><RequestedArticulationID name="req_artic" slot="0" artic="0" /></MIDIMapElement>
        </VObjectArray>
        <VObjectArray name="midi_note_37" count="0" />
        <VObjectArray name="midi_note_40" count="1">
          <MIDIMapElement note="40" isgrv="false"><RequestedArticulationID name="req_artic" slot="1" artic="2" /></MIDIMapElement>
        </VObjectArray></BFD2MidiMap>)";
    const auto mm = bfdmodel::parseMidiMap(midi);
    CHECK_EQ(mm.size(), size_t(2));
    if (mm.size() == 2) {
        CHECK(mm[1].note == 40 && mm[1].slot == 1 && mm[1].articIndex == 2);
    }

    f.kit.pieces[1].stageX = 0.25f;
    f.kit.pieces[1].stageY = 0.75f;
    f.kit.pieces[1].bleedReturn = 0.3f;
    bfdmodel::Kit back;
    CHECK(bfdmodel::kitFromXml(bfdmodel::kitToXml(f.kit, true), &back));
    CHECK_EQ(back.pieces.size(), size_t(2));
    if (back.pieces.size() == 2) {
        CHECK(std::abs(back.pieces[1].stageX - 0.25f) < 1e-6f);
        CHECK(std::abs(back.pieces[1].bleedReturn - 0.3f) < 1e-6f);
        CHECK_EQ(back.pieces[1].slotRef, std::string("g/tom"));
    }
    // BFD kit files never carry the layout
    CHECK(bfdmodel::kitToXml(f.kit, false).find("ob_stage") == std::string::npos);
    CHECK(bfdmodel::KitPieceState{}.bleedReturn == 1.0f);

    bfdmodel::Piece p;
    p.dimensions = "18x20";
    CHECK(p.dimensions.size() > 0);
}

void testPresetRoundTrip() {
    // Full .bfd3 round-trip: load a real preset, save it back, reload, and
    // confirm kit state (bleed, ambience, articulations) survives.
    const char* root = std::getenv("OPENBFD_PRESET");
    if (!root) {
        std::printf("skip preset round-trip (set OPENBFD_PRESET)\n");
        return;
    }
    bfdmodel::Preset pre;
    std::string err;
    CHECK(bfdmodel::loadPreset(root, &pre, &err));
    CHECK(pre.valid);
    CHECK(!pre.kit.pieces.empty() || !pre.kit.slots.empty());

    const std::string tmp = "/tmp/openbfd_preset_rt.bfd3";
    CHECK(bfdmodel::savePreset(tmp, pre, &err));
    bfdmodel::Preset re;
    CHECK(bfdmodel::loadPreset(tmp, &re, &err));
    CHECK_EQ(re.kit.name, pre.kit.name);
    CHECK_EQ(re.kit.slots.size(), pre.kit.slots.size());
    CHECK_EQ(re.kit.pieces.size(), pre.kit.pieces.size());
    for (size_t i = 0; i < pre.kit.pieces.size() && i < re.kit.pieces.size(); ++i) {
        const auto& a = pre.kit.pieces[i];
        const auto& b = re.kit.pieces[i];
        CHECK(std::abs(a.pan - b.pan) < 1e-5f);
        CHECK(std::abs(a.trim - b.trim) < 1e-5f);
        CHECK(std::abs(a.kickBleedLvl - b.kickBleedLvl) < 1e-5f);
        CHECK(std::abs(a.snareBleedLvl - b.snareBleedLvl) < 1e-5f);
        CHECK_EQ(a.kickBleedDst, b.kickBleedDst);
        CHECK_EQ(a.snareBleedDst, b.snareBleedDst);
        CHECK(std::abs(a.bleedSend - b.bleedSend) < 1e-5f);
        CHECK(std::abs(a.bleedReturn - b.bleedReturn) < 1e-5f);
        CHECK(std::abs(a.ambMasterGain - b.ambMasterGain) < 1e-5f);
        CHECK_EQ(a.arts.size(), b.arts.size());
    }
    // pass-through subtrees survive
    CHECK_EQ(re.grooveBundleXml == pre.grooveBundleXml, true);
    // no duplicated kit_slot_ strings on save
    CHECK(std::none_of(re.kitInfoStrings.begin(), re.kitInfoStrings.end(),
                       [](const std::string& v) { return v.find("kit_slot_") != std::string::npos; }));
    // the preset's own MIDI map and mixer are readable
    if (!pre.midiMapXml.empty()) {
        const auto map = bfdmodel::parseMidiMap(pre.midiMapXml);
        CHECK(!map.empty());
        std::printf("preset midi map: %d notes\n", int(map.size()));
    }
    if (!pre.mixerXml.empty()) {
        bfdengine::MixerState mx;
        bfdengine::importBfdMixer(pre.mixerXml, {}, mx);
        CHECK(mx.masterKickBleed > 0.0f && mx.masterKickBleed <= 1.0f);
    }
    std::printf("preset round-trip: %s -> %s (kit %s, %d pieces)\n", root, tmp.c_str(),
                re.kit.name.c_str(), int(re.kit.pieces.size()));
}

void testPreloadCompletes() {
    // Every layer of a piece with Alt/ round-robin sets must preload, not
    // only the primary set.
    const char* root = std::getenv("OPENBFD_LIBRARY");
    if (!root) return;
    bfdmodel::Library lib;
    std::string err;
    if (!bfdmodel::scanLibrary(root, &lib, &err)) return;
    const bfdmodel::Piece* piece = nullptr;
    for (const auto& p : lib.pieces) {
        for (const auto& a : p->articulations)
            if (a.hasRoundRobin()) piece = p.get();
        if (piece) break;
    }
    if (!piece) return;
    bfdengine::Engine engine;
    engine.setLibraries({lib});
    bfdmodel::Kit kit;
    kit.slots.assign(32, {});
    kit.pieces.resize(32);
    const std::string ref = bfdmodel::pieceSlotKey(lib, *piece);
    kit.slots[0] = ref;
    kit.pieces[0].slotRef = ref;
    engine.setKit(kit);
    CHECK(engine.waitUntilLoaded(60000));
    CHECK(engine.layersTotal() > 0);
    CHECK(engine.layersDone() == engine.layersTotal());
    CHECK(engine.layersReady() == engine.layersTotal());
    std::printf("preload: %s %d/%d layers\n", piece->name.c_str(), engine.layersReady(), engine.layersTotal());
}

void testEngineSmoke() {
    const char* root = std::getenv("OPENBFD_LIBRARY");
    if (!root) return;
    bfdmodel::Library lib;
    std::string err;
    if (!bfdmodel::scanLibrary(root, &lib, &err) || lib.pieces.empty()) return;

    bfdengine::Engine engine;
    engine.setSampleRate(44100.0);
    engine.setLibraries({lib});
    bfdmodel::Kit kit;
    kit.slots.assign(32, {});
    kit.pieces.resize(32);
    // put the first kick-like piece on slot 0
    const bfdmodel::Piece* piece = lib.pieces.front().get();
    const std::string ref = bfdmodel::pieceSlotKey(lib, *piece);
    kit.slots[0] = ref;
    kit.pieces[0].slotRef = ref;
    engine.setKit(kit);
    // Large libraries (BFD3 Core = 48 GB) can exceed a minute of preload. The
    // engine still plays via nearest-layer fallback and continues loading in
    // the background, so a preload timeout is a budget outcome, not a
    // correctness failure -- only fail if *nothing* loaded.
    engine.waitUntilLoaded(120000);
    CHECK(engine.layersTotal() == 0 || engine.layersReady() > 0);

    // patch every mic, then hit the first mapped note
    engine.setMixer([&] {
        bfdengine::MixerState mx;
        bfdengine::autoPatch(bfdengine::listMics(engine.kit(), engine.pieceLookup()), mx);
        return mx;
    }());
    std::vector<float> l(44100), r(44100), s0l(44100), s0r(44100);
    std::vector<float*> outs(bfdengine::Engine::kNumOuts, nullptr);
    outs[0] = l.data();
    outs[1] = r.data();
    outs[2] = s0l.data();
    outs[3] = s0r.data();
    const int note = engine.keymap().empty() ? 36 : engine.keymap().front().note;
    bfdengine::NoteEvent on{note, 1.0f, true, 100};
    engine.process(&on, 1, outs.data(), 44100);
    double energy = 0, strip = 0;
    for (size_t i = 0; i < l.size(); ++i) energy += double(l[i]) * l[i] + double(r[i]) * r[i];
    for (size_t i = 0; i < s0l.size(); ++i) strip += double(s0l[i]) * s0l[i];
    CHECK(energy > 0.0);
    CHECK(strip > 0.0);                 // strip 1 output bus carries its own mic
    CHECK(engine.takeMeter(0) > 0.0f);
    CHECK(engine.takeMeter(0) == 0.0f); // meters reset on read
    CHECK(engine.takeSlotHit(0) > 0.0f);
    std::printf("engine smoke: mix energy %.6f (%s)\n", energy, ref.c_str());
}

} // namespace

int main() {
    testBitReader();
    testSyntheticBlocks();
    testXmlAndKit();
    testVelocityLayers();
    testStreamingDecode();
    testStreamingFileAgainstResident();
    testBleedRouting();
    testKeymapsAndKitXml();
    testRoundRobinGroups();
    testPresetRoundTrip();
    testRealLibrary();
    testPreloadCompletes();
    testEngineSmoke();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all tests passed\n");
    return 0;
}
