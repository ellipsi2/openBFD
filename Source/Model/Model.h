// Content-library model: BFD piece / articulation / kit metadata.
// All metadata in BFD3 libraries is plain XML; only audio is compressed.
// Formats documented in analysis/docs/01-02. Pure C++20, no JUCE.
#pragma once

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace bfdmodel {

// --- tiny XML DOM (attribute-centric, enough for BFD metadata) -------------

struct XmlElement {
    std::string name;
    std::map<std::string, std::string> attrs;
    std::vector<XmlElement> children;
    std::string text; // character data (VString bodies)

    const std::string* attr(const std::string& key) const {
        auto it = attrs.find(key);
        return it == attrs.end() ? nullptr : &it->second;
    }
    std::string attrOr(const std::string& key, const std::string& fallback = {}) const {
        auto* v = attr(key);
        return v ? *v : fallback;
    }
    double attrNum(const std::string& key, double fallback = 0.0) const;
    const XmlElement* child(const std::string& childName) const;
    std::vector<const XmlElement*> childrenNamed(const std::string& childName) const;
};

// Parse one or more top-level elements; returns false on malformed input.
bool parseXml(const std::string& xml, std::vector<XmlElement>* roots, std::string* error = nullptr);

// --- piece / articulation ------------------------------------------------

struct ChanInfo {
    std::string name;      // "OH Left", "Kick In", ...
    std::string type;      // ambient | dirKick | dirSnare | dirOther
    std::string encoding;  // mono | stereoLeft | stereoRight | stereoMid | stereoSide
    int micIndex = 0;
    int chanIndex = 0;     // interleave index inside the file

    bool isAmbient() const { return type == "ambient"; }
    // Ambience group derived from the channel name ("OH", "Room", "Amb3", ...).
    std::string group() const;
};

struct VelLayer {
    float velLo = 0, velHi = 0; // normalised 0..1, contiguous windows
    float dbTrim = 0;
    float loudness = -100; // measured dB of the layer
    std::string fileName;  // relative to the articulation directory
};

// One round-robin group of velocity layers (BFD2ArticRRGroup). The primary
// layer list is group 0; "Alt/" carries group 1 with its own velocity map.
struct RrGroup {
    std::string name;
    std::string directory;         // folder holding this group's BFDArticTweaks.xml
    std::vector<VelLayer> layers;  // sorted by velLo
};

struct Articulation {
    std::string name;              // "Hit", "Rim Click", ...
    std::string directory;         // absolute path of the articulation folder
    float trimDb = 0;
    std::vector<VelLayer> layers;  // sorted by velLo (round-robin group 0)
    std::vector<RrGroup> rrGroups; // group 0 = primary, then "Alt", ...
    std::vector<VelLayer> altLayers; // optional alternate set (Alt/) -- kept for compat

    // True when this articulation carries more than one round-robin group.
    bool hasRoundRobin() const { return rrGroups.size() > 1; }
    // Round-robin group count (>=1).
    int rrCount() const { return int(std::max<size_t>(1, rrGroups.size())); }
};

struct Piece {
    std::string directory;
    std::vector<std::string> articOrder; // Info.xml articulation order (MIDI map indices)
    float diameter = 0;        // inches, from kpi_dimensions (0 = unknown)
    std::string name;          // kpi_name
    std::string family;        // kpi_family
    std::string manufacturer;
    std::string model;
    std::string dimensions;
    std::string library;       // info_library SKU
    std::string libraryLong;
    std::string klass;         // kpi_class: "Kick", "Snare", ...
    std::vector<ChanInfo> channels; // sorted by chanIndex
    std::vector<Articulation> articulations;

    const Articulation* findArticulation(const std::string& n) const;
};

// Coarse kit-piece kind, from kpi_class refined by name and size. Drives the
// stage drawing, default placement, the GM auto-keymap and which file
// channels are the piece's own direct mics (the rest are bleed).
enum class PieceKind { Kick, Snare, Hihat, Tom, FloorTom, Crash, Ride, Splash, China, Perc };
PieceKind pieceKind(const Piece& piece);
const char* pieceKindCode(PieceKind kind); // "KICK", "SNR", ...
float pieceDiameter(const Piece& piece);   // inches, with per-kind fallbacks

// True when `ch` is one of the piece's own direct mics. Every BFD3 file
// carries Kick In/Out + Snare Top/Bottom + Direct: for a kick the kick mics
// are its own, for a snare the snare mics, for everything else "Direct".
bool isOwnChannel(PieceKind kind, const ChanInfo& ch);

// Load Info.xml / BFDInfo.xml + every articulation's BFDArticTweaks.xml.
bool loadPiece(const std::string& pieceDir, Piece* out, std::string* error = nullptr);

// --- library scanning ----------------------------------------------------

struct Library {
    std::string root;
    std::string name;
    std::vector<std::shared_ptr<Piece>> pieces;
};

// Scan `<root>/Audio/**` for piece directories (contain Info.xml/BFDInfo.xml).
// `groupPieceKey` yields the BFD slot reference ("YMA Drums/Kick MCA20 Felt").
bool scanLibrary(const std::string& root, Library* out, std::string* error = nullptr);
std::string pieceSlotKey(const Library& lib, const Piece& piece);

// Library roots BFD3 itself knows about (~/Library/Application Support/BFD
// Drums/BFD3/DataPaths.xml, enabled entries that still exist on disk).
std::vector<std::string> discoverLibraryRoots();

// Kit and preset documents shipped with a library (<root>/Kits, <root>/Presets).
struct KitFileEntry {
    std::string path;
    std::string name;    // file stem
    std::string library; // library folder name
    bool preset = false; // .bfd3 / .bfd2 program vs .bfd2kit / .bfd3kit
};
std::vector<KitFileEntry> listKitFiles(const std::string& libraryRoot);

// --- kits (.bfd2kit) -----------------------------------------------------

struct KitArticState {
    std::string articName;
    float v2d = 0.5f, v2p = 0.5f, v2a = 0.75f, aTrim = 0.5f, velVar = 1.0f;
    bool loadState = false;
    float amgVarDepth = 1.0f; // AMGVarDepth (BFD3)
};

// Per-piece ambience send (BFD2KitPieceAmbSend): sourceMic -> destChan.
struct AmbSend {
    std::string sourceMic; // "OH", "Room", "Amb3", "Mono 1", ...
    std::string destChan;  // destination channel name
    float gain = 0.8f;
    bool active = true;
};

// Per-piece physical model (BFD2KitPieceModel): tom resonance / spill sim.
struct PieceModel {
    bool chokeFadeActive = false;
    float chokeFadeBase = 1.0f, chokeFadeRange = 1.0f;
    float hihatTightenDamp = 0.0f, hihatTightenPitch = 0.0f;
    bool tomResActive = false;
    float resTrim = 0.5f, spillTrim = 0.5f;
};

struct KitPieceState {
    std::string slotRef; // "YMA Drums/Kick MCA20 Felt"
    float pan = 0.5f, trim = 0.8f, dampAmt = 0.0f, dampFreq = 500.0f;
    float dampRatio = 1.0f;
    float gainOH = 0.8f, gainRM = 0.8f, gainPZM = 0.8f;
    float tune = 0.5f, dyn = 0.5f;
    float loudnessCurve = 0.0f, loudnessDynRange = 0.5f;
    bool loudnessNorm = false;
    bool flipAmb = false, mute = false, solo = false;
    // bleed: the piece's sound as picked up by the kick / snare mics.
    // kick_bl_dst / snre_bl_dst follow BFD3's inspector radio group:
    // 0 = Off, 1 = Dir (into this piece's primary direct mic), 2 = On (into
    // the kit's kick / snare mic channels). bleed_send scales what this piece
    // sends; bleed_return scales what a kick/snare piece's mics take back in;
    // inputBleedTrim is the channel's bleed input trim (fader law, 0.8 = 0 dB).
    float kickBleedLvl = 1.0f, snareBleedLvl = 1.0f;
    int kickBleedDst = 2, snareBleedDst = 2;
    float bleedSend = 1.0f, bleedReturn = 1.0f, bleedInputTrim = 0.8f;
    std::string amb1Dest = "OH", amb2Dest = "Room", amb3Dest = "Amb3";
    float ambMasterGain = 0.8f;
    bool ambMasterGainLink = false;
    float micBlend = 0.5f;
    int chokeGroup = 0;
    std::vector<AmbSend> ambSends;
    PieceModel model;
    std::vector<KitArticState> arts;
    // openbfd only: stage position, normalised 0..1 (-1 = automatic).
    // Kept in plugin state, never written to BFD kit files.
    float stageX = -1.0f, stageY = -1.0f;
};

struct Kit {
    static constexpr int kMaxSlots = 64;
    std::string name;
    std::string library, libraryLong;
    std::string sourceEngine = "BFD3";
    std::string sourceEngineVersion = "3.5";
    float masterDynamics = 0.5f;
    int size = 0;
    std::vector<std::string> slots;      // up to 64 slot references ("" = empty)
    std::vector<KitPieceState> pieces;   // per populated slot
};

// Load a kit file. Handles both *.bfd2kit / *.bfd3kit (bare BFD2KitInfo +
// BFD2Kit) and *.bfd3 program presets (root wrapper with BFD2ProgramInfo,
// BFD2KitInfo, BFD2Kit, plus groove/song state we preserve verbatim).
bool loadKit(const std::string& path, Kit* out, std::string* error = nullptr);
// Save kit state. `path` extension selects the writer:
//   .bfd3      -> full program preset round-trip
//   otherwise  -> bare BFD2KitInfo + BFD2Kit (BFD2/BFD3 kit)
bool saveKit(const std::string& path, const Kit& kit, std::string* error = nullptr);

// Kit <-> XML text (plugin state). `withLayout` adds openbfd's stage
// positions as extra attributes, which kitFromXml reads back.
std::string kitToXml(const Kit& kit, bool withLayout);
bool kitFromXml(const std::string& xml, Kit* out, std::string* error = nullptr);

// Preset document: everything loadKit/saveKit round-trips for .bfd3 files.
// Kit state lives in `kit`; `passthroughXml` preserves groove/song/mixer
// subtrees that openbfd does not model but must not lose on a save.
struct Preset {
    Kit kit;
    std::string rootAttrs;            // attributes of <root> verbatim
    std::string programInfoXml;       // <BFD2ProgramInfo .../>
    std::string kitInfoAttrs;         // attributes of <BFD2KitInfo ...>
    std::vector<std::string> kitInfoStrings; // <VString .../> children of kit_info
    std::string grooveBundleXml;      // <BFD2GrooveBundle> ... </BFD2GrooveBundle>
    std::string patternManagerXml;    // <BFD2PatternManagerSettings> ...
    std::string midiMapXml;           // <BFD2MidiMap> ...
    std::string synthXml;             // <BFD2Synth> ...
    std::string mixerXml;             // <BFD2Mixer> ...
    std::string songXml;              // <BFD2Song> ...
    std::string automationXml;        // <BFD2AutomationMap> ...
    bool valid = false;
};

bool loadPreset(const std::string& path, Preset* out, std::string* error = nullptr);
bool savePreset(const std::string& path, const Preset& preset, std::string* error = nullptr);

// --- MIDI keymap ---------------------------------------------------------

// GM percussion defaults: note -> (slot index, articulation name).
struct KeymapEntry {
    int note = -1;
    int slot = -1;
    std::string articulation;
    int articIndex = -1; // BFD2MidiMap: index into Piece::articOrder (wins when >= 0)
};
std::vector<KeymapEntry> defaultGmKeymap();

// BFD2MidiMap (inside .bfd3 presets): midi_note_N -> RequestedArticulationID
// {slot, artic}. Returns an empty map when the document carries none.
std::vector<KeymapEntry> parseMidiMap(const std::string& midiMapXml);

// GM keymap built from what is actually in the kit: the first kick / snare /
// hihat get the GM kick / snare / hat notes, toms are ordered high -> low by
// size onto 50 48 47 45 43 41, crashes 49 57 55 52, rides 51/53/59, the rest
// fill the free percussion notes. `pieceAt(slot)` returns nullptr for empty slots.
std::vector<KeymapEntry> autoKeymap(const Kit& kit, const std::vector<const Piece*>& pieceAt);

} // namespace bfdmodel
