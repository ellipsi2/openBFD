// Mixer model: patchable mic sources, 32 numbered channel strips, master.
// Pure C++20, no JUCE.
//
// Every mic in the kit is a source that can be patched into one input jack of
// a numbered strip. Each strip has an L and an R jack: only L (or only R)
// patched = mono strip, both = stereo strip (a Mid + Side pair decodes to
// L/R). The strip number is also its output: strip N feeds the plugin's
// "Ch N" output bus as well as the master mix.
//
// Sources
//   D:<slot>:<chan>  a kit piece's own direct mic ("Kick In", "Snare Top", "Direct")
//   A:<chan>         a kit-wide ambient mic ("OH Left", "Room Right", "Mono 1")
//   B:Other          "other" bleed (the kick / snare pieces' Direct channel)
// Kick and snare bleed are not sources of their own: per piece they go Off,
// into the piece's primary direct mic (Dir) or into the kick / snare mics (On),
// exactly like BFD3's kick_bl_dst / snre_bl_dst.
#pragma once

#include <array>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../Model/Model.h"

namespace bfdengine {

inline constexpr int kNumStrips = 32;

enum class MicKind { Direct, Ambient, Bleed };

struct MicSource {
    std::string key;      // see header comment
    MicKind kind = MicKind::Direct;
    int slot = -1;        // Direct: kit slot
    std::string chanName; // file channel name ("Kick In", "OH Left")
    std::string group;    // Ambient: "OH", "Room", ...; Direct: piece kind code
    std::string encoding; // mono | stereoLeft | stereoRight | stereoMid | stereoSide
    int micIndex = 0;     // BFD micIndex (matches BFD2MixerChan micID)
};

struct Jack {
    int strip = -1; // 0-based; the strip number shown is strip + 1
    int side = 0;   // 0 = L, 1 = R
    bool valid() const { return strip >= 0 && strip < kNumStrips && (side == 0 || side == 1); }
    bool operator==(const Jack& o) const { return strip == o.strip && side == o.side; }
};

struct StripState {
    float gain = 0.8f; // fader position, BFD law (0.8 = 0 dB)
    float pan = 0.5f;  // mono: position; stereo: balance
    bool mute = false, solo = false;
};

enum class StripMode { Empty, MonoL, MonoR, Stereo, MidSide };

using PieceLookup = std::vector<const bfdmodel::Piece*>; // index = kit slot, nullptr = empty

struct MixerState {
    std::map<std::string, Jack> patch; // mic key -> jack
    std::array<StripState, kNumStrips> strips{};
    float masterGain = 0.8f;
    float masterKickBleed = 0.8f, masterSnareBleed = 0.8f, masterOtherBleed = 0.8f;

    Jack jackOf(const std::string& key) const;
    const std::string* micAt(int strip, int side) const; // nullptr = free jack
    // Plug `key` into `jack`, replacing whatever was in that jack.
    void plug(const std::string& key, Jack jack);
    void unplug(const std::string& key);
    StripMode mode(int strip, const std::vector<MicSource>& mics) const;
};

// BFD fader law shared by strips, bleed masters and trims: 0.8 = 0 dB,
// 1.0 = +5.8 dB, 0.4 = -18 dB, 0 = silence.
float faderGain(float pos);
float faderDb(float pos); // -inf as -120

// Every patchable source for the kit, in a stable order: direct mics by kit
// order (kick, snare, hats, toms high -> low, cymbals, perc), ambient mics in
// file order, then other bleed.
std::vector<MicSource> listMics(const bfdmodel::Kit& kit, const PieceLookup& pieces);

// Forget patches of mics that no longer exist (`prune`) and give every
// unpatched mic a free strip: a direct mic gets its own mono strip, an
// ambient L/R (or M/S) pair shares one stereo strip. `only` limits the
// patching to those keys (mics the user unplugged on purpose stay loose).
void autoPatch(const std::vector<MicSource>& mics, MixerState& state, bool prune = true,
               const std::set<std::string>* only = nullptr);

// BFD2Mixer (inside .bfd3 presets): channel faders, pans, mutes, solos and the
// master bleed levels, applied to the strips the matching mics are patched to.
void importBfdMixer(const std::string& mixerXml, const std::vector<MicSource>& mics, MixerState& state);

// Mixer state <-> attribute-only XML (plugin state).
std::string mixerToXml(const MixerState& state);
bool mixerFromXml(const std::string& xml, MixerState* out);

} // namespace bfdengine
