#include "Model.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace bfdmodel {
namespace fs = std::filesystem;

// --- XML -----------------------------------------------------------------

namespace {

std::string decodeEntities(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '&') {
            const auto semi = s.find(';', i);
            if (semi != std::string::npos && semi - i < 10) {
                const std::string ent = s.substr(i + 1, semi - i - 1);
                if (ent == "amp") out += '&';
                else if (ent == "lt") out += '<';
                else if (ent == "gt") out += '>';
                else if (ent == "quot") out += '"';
                else if (ent == "apos") out += '\'';
                else out += '&' + ent + ';';
                i = semi + 1;
                continue;
            }
        }
        out += s[i++];
    }
    return out;
}

struct Parser {
    const std::string& s;
    size_t pos = 0;

    void skipWs() {
        while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
    }
    bool starts(const char* p) const { return s.compare(pos, std::strlen(p), p) == 0; }
    void skipMisc() {
        for (;;) {
            skipWs();
            if (starts("<!--")) {
                const auto end = s.find("-->", pos);
                pos = end == std::string::npos ? s.size() : end + 3;
            } else if (starts("<?")) {
                const auto end = s.find("?>", pos);
                pos = end == std::string::npos ? s.size() : end + 2;
            } else if (starts("<!")) {
                const auto end = s.find('>', pos);
                pos = end == std::string::npos ? s.size() : end + 1;
            } else {
                break;
            }
        }
    }

    bool parseName(std::string* name) {
        skipWs();
        const size_t start = pos;
        while (pos < s.size() && (std::isalnum(static_cast<unsigned char>(s[pos])) ||
                                  s[pos] == '_' || s[pos] == '-' || s[pos] == ':' || s[pos] == '.'))
            ++pos;
        if (pos == start) return false;
        *name = s.substr(start, pos - start);
        return true;
    }

    bool parseElement(XmlElement* out) {
        skipMisc();
        if (pos >= s.size() || s[pos] != '<' || starts("</")) return false;
        ++pos;
        if (!parseName(&out->name)) return false;
        for (;;) {
            skipWs();
            if (pos >= s.size()) return false;
            if (s[pos] == '/') {
                ++pos;
                if (pos < s.size() && s[pos] == '>') { ++pos; return true; }
                return false;
            }
            if (s[pos] == '>') {
                ++pos;
                break;
            }
            std::string key;
            if (!parseName(&key)) return false;
            skipWs();
            std::string value;
            if (pos < s.size() && s[pos] == '=') {
                ++pos;
                skipWs();
                if (pos < s.size() && (s[pos] == '"' || s[pos] == '\'')) {
                    const char q = s[pos++];
                    const auto end = s.find(q, pos);
                    if (end == std::string::npos) return false;
                    value = decodeEntities(s.substr(pos, end - pos));
                    pos = end + 1;
                } else {
                    return false;
                }
            }
            out->attrs[key] = value;
        }
        // children + character data until </name>
        std::string text;
        for (;;) {
            skipMisc();
            if (pos >= s.size()) return false;
            if (starts("</")) {
                pos += 2;
                std::string closing;
                if (!parseName(&closing)) return false;
                skipWs();
                if (pos >= s.size() || s[pos] != '>') return false;
                ++pos;
                out->text = decodeEntities(text);
                return true;
            }
            if (s[pos] == '<') {
                if (starts("<!--") || starts("<!")) {
                    const auto end = s.find('>', pos);
                    pos = end == std::string::npos ? s.size() : end + 1;
                    continue;
                }
                XmlElement child;
                if (!parseElement(&child)) return false;
                out->children.push_back(std::move(child));
            } else {
                const auto next = s.find('<', pos);
                text += s.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
                pos = next == std::string::npos ? s.size() : next;
            }
        }
    }
};

bool readFile(const std::string& path, std::string* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    *out = ss.str();
    return true;
}

} // namespace

double XmlElement::attrNum(const std::string& key, double fallback) const {
    auto* v = attr(key);
    if (!v) return fallback;
    try {
        return std::stod(*v);
    } catch (...) {
        return fallback;
    }
}

const XmlElement* XmlElement::child(const std::string& childName) const {
    for (const auto& c : children)
        if (c.name == childName) return &c;
    return nullptr;
}

std::vector<const XmlElement*> XmlElement::childrenNamed(const std::string& childName) const {
    std::vector<const XmlElement*> out;
    for (const auto& c : children)
        if (c.name == childName) out.push_back(&c);
    return out;
}

bool parseXml(const std::string& xml, std::vector<XmlElement>* roots, std::string* error) {
    Parser p{xml};
    for (;;) {
        p.skipMisc();
        if (p.pos >= xml.size()) return true;
        XmlElement el;
        if (!p.parseElement(&el)) {
            if (error) *error = "malformed XML near offset " + std::to_string(p.pos);
            return false;
        }
        roots->push_back(std::move(el));
    }
}

std::string ChanInfo::group() const {
    std::string n = name;
    for (const char* suffix : {" Left", " Right", " Mid", " Side", " L", " R"}) {
        const size_t len = std::strlen(suffix);
        if (n.size() > len && n.compare(n.size() - len, len, suffix) == 0) {
            n.resize(n.size() - len);
            break;
        }
    }
    return n.empty() ? name : n;
}

const Articulation* Piece::findArticulation(const std::string& n) const {
    for (const auto& a : articulations)
        if (a.name == n) return &a;
    return nullptr;
}

// --- piece loading -------------------------------------------------------

namespace {

void parseChannels(const XmlElement& pieceInfo, Piece* out) {
    for (const XmlElement* arr : pieceInfo.childrenNamed("VObjectArray")) {
        if (arr->attrOr("name") != "audioChanInfo") continue;
        for (const XmlElement* ch : arr->childrenNamed("BFD2AudioChanInfo")) {
            ChanInfo c;
            c.name = ch->attrOr("chanName");
            c.type = ch->attrOr("chanType");
            c.encoding = ch->attrOr("encoding");
            c.micIndex = int(ch->attrNum("micIndex"));
            c.chanIndex = int(ch->attrNum("chanIndex"));
            out->channels.push_back(std::move(c));
        }
    }
    std::sort(out->channels.begin(), out->channels.end(),
              [](const ChanInfo& a, const ChanInfo& b) { return a.chanIndex < b.chanIndex; });
}

// Parse one BFDArticTweaks.xml into a velocity-layer list + round-robin groups.
// `dir` is the folder holding the XML; `out` receives the primary layers and
// any RR groups declared in the document (or discovered in sibling folders).
static void parseOneTweaks(const std::string& dir, std::vector<VelLayer>* layers,
                           std::vector<RrGroup>* rrGroups, float* trimDb) {
    std::string xml;
    if (!readFile(dir + "/BFDArticTweaks.xml", &xml)) return;
    std::vector<XmlElement> roots;
    if (!parseXml(xml, &roots)) return;
    const XmlElement* tweaks = nullptr;
    for (const auto& r : roots) {
        if (r.name == "BFD2ArticTweaks") tweaks = &r;
        if (const auto* inner = r.child("BFD2ArticTweaks")) tweaks = inner;
    }
    if (!tweaks) return;
    if (trimDb) *trimDb = float(tweaks->attrNum("trim_dB"));
    for (const XmlElement* arr : tweaks->childrenNamed("VObjectArray")) {
        const std::string aname = arr->attrOr("name");
        if (aname == "velocityLayerTweaks") {
            for (const XmlElement* t : arr->childrenNamed("BFD2VelLayerTweak")) {
                VelLayer l;
                l.velLo = float(t->attrNum("velLo"));
                l.velHi = float(t->attrNum("velHi"));
                l.dbTrim = float(t->attrNum("dbTrim"));
                l.loudness = float(t->attrNum("loudness", -100));
                l.fileName = t->attrOr("fileName");
                layers->push_back(std::move(l));
            }
            std::sort(layers->begin(), layers->end(),
                      [](const VelLayer& a, const VelLayer& b) { return a.velLo < b.velLo; });
        } else if (aname == "rrGroups") {
            // BFD2ArticRRGroup entries name round-robin groups. Each group
            // lives in its own folder with its own velocity map.
            for (const XmlElement* g : arr->childrenNamed("BFD2ArticRRGroup")) {
                RrGroup grp;
                grp.name = g->attrOr("name", g->attrOr("rr_name"));
                grp.directory = g->attrOr("directory", g->attrOr("dir"));
                rrGroups->push_back(std::move(grp));
            }
        }
    }
}

void parseArticTweaks(const std::string& dir, Articulation* out) {
    std::vector<VelLayer> primary;
    std::vector<RrGroup> groups;
    float trim = 0.0f;
    parseOneTweaks(dir, &primary, &groups, &trim);
    out->trimDb = trim;
    out->layers = primary;

    // Round-robin group 0 is always the primary set. When the XML named no
    // rrGroups (or named them without directories), discover sibling folders
    // that carry their own BFDArticTweaks.xml -- the shipped layout puts the
    // alternate set in "Alt/" with its own velocity map.
    RrGroup g0;
    g0.name = "Main";
    g0.directory = dir;
    g0.layers = primary;

    std::vector<RrGroup> discovered;
    if (!groups.empty()) {
        for (auto& g : groups) {
            if (g.directory.empty()) g.directory = dir + "/" + g.name;
            std::vector<VelLayer> l;
            parseOneTweaks(g.directory, &l, nullptr, nullptr);
            g.layers = std::move(l);
            discovered.push_back(std::move(g));
        }
    }
    // Note: BFD3's "Alt/" alternate sample set is a *sibling articulation*
    // (same piece root, folder named "Alt"), not a subfolder of the base
    // articulation. loadPiece() links it into the base articulation's RR
    // groups after all articulations are parsed (see linkRoundRobin).

    out->rrGroups.clear();
    out->rrGroups.push_back(std::move(g0));
    for (auto& g : discovered) {
        if (g.layers.empty()) continue;
        // avoid duplicating the primary
        if (g.directory == dir) continue;
        out->rrGroups.push_back(std::move(g));
    }
    // keep legacy altLayers mirror for existing callers/tests
    if (out->rrGroups.size() > 1) out->altLayers = out->rrGroups[1].layers;
}

// Link BFD3's "Alt/" sibling articulation into its base articulation's
// round-robin groups. In the shipped layout, "Hit" and "Alt" are two folders
// under the piece root: "Hit" is the primary sample set, "Alt" is the
// alternate set used for round-robin. After linking, the base articulation
// carries both sets and "Alt" is no longer a standalone articulation.
// kpi_dimensions: "18x20" (kick depth x diameter), "14x5.5", "20", "Unknown".
// The larger figure is the diameter for every shipped drum and cymbal.
static float parseDiameter(const std::string& dims) {
    float best = 0.0f;
    const char* p = dims.c_str();
    while (*p) {
        if (std::isdigit(static_cast<unsigned char>(*p))) {
            char* end = nullptr;
            const float v = std::strtof(p, &end);
            best = std::max(best, v);
            p = end;
        } else {
            ++p;
        }
    }
    return best >= 4.0f && best <= 30.0f ? best : 0.0f;
}

static void linkRoundRobin(Piece* piece) {
    Articulation* alt = nullptr;
    for (auto& a : piece->articulations)
        if (a.name == "Alt" && !a.layers.empty()) {
            alt = &a;
            break;
        }
    if (!alt) return;

    // Attach Alt/ as RR group 1 of every other articulation that has layers
    // (BFD3 gives each hit type its own Alt/ set; when the piece has a single
    // "Hit" articulation, that is the one Alt/ belongs to).
    int linked = 0;
    for (auto& a : piece->articulations) {
        if (&a == alt) continue;
        if (a.layers.empty()) continue;
        if (a.hasRoundRobin()) continue; // already has explicit RR groups
        RrGroup g;
        g.name = "Alt";
        g.directory = alt->directory;
        g.layers = alt->layers;
        a.rrGroups.push_back(std::move(g));
        a.altLayers = alt->layers;
        ++linked;
    }
    if (linked == 0) {
        // No other articulation to attach to: keep Alt/ as its own
        // articulation (it is the only sample set in the piece).
        return;
    }
    // Drop the standalone "Alt" articulation now that it is an RR group.
    piece->articulations.erase(
        std::remove_if(piece->articulations.begin(), piece->articulations.end(),
                       [alt](const Articulation& a) { return &a == alt; }),
        piece->articulations.end());
}

} // namespace

bool loadPiece(const std::string& pieceDir, Piece* out, std::string* error) {
    std::string xml;
    std::string path = pieceDir + "/Info.xml";
    if (!readFile(path, &xml)) {
        path = pieceDir + "/BFDInfo.xml";
        if (!readFile(path, &xml)) {
            if (error) *error = "no Info.xml / BFDInfo.xml in " + pieceDir;
            return false;
        }
    }
    std::vector<XmlElement> roots;
    if (!parseXml(xml, &roots, error)) return false;

    const XmlElement* info = nullptr;
    for (const auto& r : roots) {
        if (r.name == "BFD2KitPieceInfo") info = &r;
        if (const auto* inner = r.child("BFD2KitPieceInfo")) info = inner;
    }
    if (!info) {
        if (error) *error = "no BFD2KitPieceInfo in " + path;
        return false;
    }

    out->directory = pieceDir;
    out->name = info->attrOr("kpi_name");
    out->family = info->attrOr("kpi_family");
    out->manufacturer = info->attrOr("kpi_manufacturer");
    out->model = info->attrOr("kpi_model");
    out->dimensions = info->attrOr("kpi_dimensions");
    out->library = info->attrOr("info_library");
    out->libraryLong = info->attrOr("info_librarylong");
    parseChannels(*info, out);
    for (const XmlElement* vs : info->childrenNamed("VString"))
        if (vs->attrOr("name") == "kpi_class") out->klass = vs->attrOr("string", vs->text);
    out->diameter = parseDiameter(out->dimensions);

    for (const XmlElement* arr : info->childrenNamed("VObjectArray")) {
        if (arr->attrOr("name") != "articulation") continue;
        for (const XmlElement* vs : arr->childrenNamed("VString")) {
            Articulation a;
            a.name = vs->attrOr("string", vs->text);
            a.directory = pieceDir + "/" + a.name;
            parseArticTweaks(a.directory, &a);
            out->articOrder.push_back(a.name);
            out->articulations.push_back(std::move(a));
        }
    }
    if (out->articulations.empty()) {
        // fall back to scanning directories that carry BFDArticTweaks.xml
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(pieceDir, ec)) {
            if (!entry.is_directory()) continue;
            if (fs::exists(entry.path() / "BFDArticTweaks.xml")) {
                Articulation a;
                a.name = entry.path().filename().string();
                a.directory = entry.path().string();
                parseArticTweaks(a.directory, &a);
                out->articulations.push_back(std::move(a));
            }
        }
        std::sort(out->articulations.begin(), out->articulations.end(),
                  [](const Articulation& a, const Articulation& b) { return a.name < b.name; });
        for (const auto& a : out->articulations) out->articOrder.push_back(a.name);
    }
    // BFD3 round-robin: "Alt/" is a sibling articulation of "Hit" carrying the
    // alternate sample set. Link it in as RR group 1 of its base articulation.
    linkRoundRobin(out);
    return true;
}

// --- library scanning ----------------------------------------------------

std::string pieceSlotKey(const Library& lib, const Piece& piece) {
    // BFD slot references are "<group>/<piece>" where group is the directory
    // below Audio/ (or the library name for flat layouts).
    std::error_code ec;
    fs::path pieceDir = fs::path(piece.directory).lexically_normal();
    fs::path parent = pieceDir.parent_path();
    std::string groupName = parent.filename().string();
    fs::path audio = fs::path(lib.root) / "Audio";
    if (parent == audio.lexically_normal() || groupName == "Audio") groupName = lib.name;
    return groupName + "/" + pieceDir.filename().string();
}

bool scanLibrary(const std::string& root, Library* out, std::string* error) {
    const fs::path audioDir = fs::path(root) / "Audio";
    if (!fs::is_directory(audioDir)) {
        if (error) *error = "no Audio/ directory under " + root;
        return false;
    }
    out->root = root;
    out->name = fs::path(root).filename().string();
    out->pieces.clear();

    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(
             audioDir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); ++it) {
        if (ec) break;
        if (!it->is_directory()) continue;
        const fs::path dir = it->path();
        if (!fs::exists(dir / "Info.xml") && !fs::exists(dir / "BFDInfo.xml")) continue;
        auto piece = std::make_shared<Piece>();
        std::string err;
        if (loadPiece(dir.string(), piece.get(), &err)) out->pieces.push_back(std::move(piece));
        it.disable_recursion_pending(); // pieces never nest
    }
    return true;
}

// ---------------------------------------------------------------------------
// Kit / preset load + save
// ---------------------------------------------------------------------------

namespace {

// Serialise a subtree of the parsed XML back to text (used for pass-through
// of groove/song/mixer state that openbfd does not model).
std::string escapeAttr(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string serializeElement(const XmlElement& e, int indent) {
    std::string pad(size_t(indent) * 2, ' ');
    std::string o = pad + "<" + e.name;
    for (const auto& kv : e.attrs)
        o += " " + kv.first + "=\"" + escapeAttr(kv.second) + "\"";
    if (e.children.empty() && e.text.empty()) return o + " />\n";
    o += ">";
    if (!e.children.empty()) {
        o += "\n";
        for (const auto& c : e.children) o += serializeElement(c, indent + 1);
        if (!e.text.empty()) o += pad + "  " + escapeAttr(e.text) + "\n";
        o += pad;
    } else {
        o += escapeAttr(e.text);
    }
    o += "</" + e.name + ">\n";
    return o;
}

const XmlElement* findDeep(const XmlElement& root, const std::string& name) {
    if (root.name == name) return &root;
    for (const auto& c : root.children)
        if (const auto* f = findDeep(c, name)) return f;
    return nullptr;
}

// Deep-collect all elements named `name` in document order.
void collectDeep(const XmlElement& root, const std::string& name, std::vector<const XmlElement*>* out) {
    if (root.name == name) out->push_back(&root);
    for (const auto& c : root.children) collectDeep(c, name, out);
}

std::string normalizeSlotRef(std::string ref) {
    // BFD2-era kits use '/', BFD3-era kits use '\' as the group/piece separator.
    for (auto& ch : ref)
        if (ch == '\\') ch = '/';
    while (!ref.empty() && (ref.front() == '/' || std::isspace((unsigned char) ref.front())))
        ref.erase(ref.begin());
    while (!ref.empty() && (ref.back() == '/' || std::isspace((unsigned char) ref.back())))
        ref.pop_back();
    return ref;
}

std::string slotRefToFile(std::string ref) {
    // file output uses '\' as the BFD3-era separator
    for (auto& ch : ref)
        if (ch == '/') ch = '\\';
    return ref;
}

bool parseKitState(const XmlElement* info, const XmlElement* kit, Kit* out, std::string* error) {
    (void) error;
    if (info) {
        out->name = info->attrOr("info_name");
        out->library = info->attrOr("info_library");
        out->libraryLong = info->attrOr("info_librarylong");
        for (const XmlElement* vs : info->childrenNamed("VString")) {
            const std::string name = vs->attrOr("name");
            if (name.rfind("kit_slot_", 0) != 0) continue;
            const int slot = std::atoi(name.c_str() + 9);
            if (slot < 0 || slot >= 64) continue;
            if (out->slots.size() <= size_t(slot)) out->slots.resize(size_t(slot) + 1);
            out->slots[size_t(slot)] = normalizeSlotRef(vs->attrOr("string", vs->text));
        }
    }
    if (kit) {
        out->masterDynamics = float(kit->attrNum("masterDynamics", 0.5));
        out->size = int(kit->attrNum("kit_size"));
        if (const auto* se = kit->attr("SourceEngine")) out->sourceEngine = *se;
        if (const auto* sv = kit->attr("SourceEngineVersion")) out->sourceEngineVersion = *sv;
        for (const XmlElement* kp : kit->childrenNamed("BFD2KitPiece")) {
            KitPieceState st;
            st.slotRef = {};
            st.pan = float(kp->attrNum("pan", 0.5));
            st.trim = float(kp->attrNum("trim", 0.8));
            st.dampAmt = float(kp->attrNum("dampAmt", 0));
            st.dampFreq = float(kp->attrNum("dampFreq", 500));
            st.dampRatio = float(kp->attrNum("dampRatio", 1));
            st.gainOH = float(kp->attrNum("gainOH", 0.8));
            st.gainRM = float(kp->attrNum("gainRM", 0.8));
            st.gainPZM = float(kp->attrNum("gainPZM", 0.8));
            st.tune = float(kp->attrNum("tune", 0.5));
            st.dyn = float(kp->attrNum("dyn", 0.5));
            st.loudnessCurve = float(kp->attrNum("loudnessCurve", 0));
            st.loudnessDynRange = float(kp->attrNum("loudnessDynRange", 0.5));
            st.loudnessNorm = kp->attrOr("loudnessNorm") == "true";
            st.flipAmb = kp->attrOr("flip_amb") == "true";
            st.mute = kp->attrOr("mute") == "true";
            st.solo = kp->attrOr("solo") == "true";
            st.kickBleedLvl = float(kp->attrNum("kick_bl_lvl", 1));
            st.snareBleedLvl = float(kp->attrNum("snre_bl_lvl", 1));
            st.kickBleedDst = int(kp->attrNum("kick_bl_dst", 2));
            st.snareBleedDst = int(kp->attrNum("snre_bl_dst", 2));
            st.bleedSend = float(kp->attrNum("bleed_send", 1));
            st.bleedReturn = float(kp->attrNum("bleed_return", 1));
            st.bleedInputTrim = float(kp->attrNum("inputBleedTrim", 0.8));
            st.amb1Dest = kp->attrOr("amb1Dest", "OH");
            st.amb2Dest = kp->attrOr("amb2Dest", "Room");
            st.amb3Dest = kp->attrOr("amb3Dest", "Amb3");
            st.ambMasterGain = float(kp->attrNum("amb_master_gain", 0.8));
            st.ambMasterGainLink = kp->attrOr("amb_master_gain_link") == "true";
            st.micBlend = float(kp->attrNum("micBlend", 0.5));
            st.chokeGroup = int(kp->attrNum("chokeGroup", 0));
            st.stageX = float(kp->attrNum("ob_stageX", -1));
            st.stageY = float(kp->attrNum("ob_stageY", -1));
            for (const XmlElement* art : kp->childrenNamed("BFD2Artic")) {
                KitArticState a;
                a.articName = art->attrOr("artic_name");
                a.v2d = float(art->attrNum("v2d", 0.5));
                a.v2p = float(art->attrNum("v2p", 0.5));
                a.v2a = float(art->attrNum("v2a", 0.75));
                a.aTrim = float(art->attrNum("aTrim", 0.5));
                a.velVar = float(art->attrNum("velVar", 1));
                a.loadState = art->attrOr("artic_load_state") == "true";
                a.amgVarDepth = float(art->attrNum("AMGVarDepth", 1));
                st.arts.push_back(std::move(a));
            }
            // ambience sends (BFD2KitPieceAmbSend)
            for (const XmlElement* arr : kp->childrenNamed("VObjectArray")) {
                if (arr->attrOr("name") != "ambSends") continue;
                for (const XmlElement* snd : arr->childrenNamed("BFD2KitPieceAmbSend")) {
                    AmbSend as;
                    as.sourceMic = snd->attrOr("sourceMic");
                    as.destChan = snd->attrOr("destChan");
                    as.gain = float(snd->attrNum("gain", 0.8));
                    as.active = snd->attrOr("active") == "true";
                    st.ambSends.push_back(std::move(as));
                }
            }
            // piece model (BFD2KitPieceModel)
            if (const XmlElement* model = kp->child("BFD2KitPieceModel")) {
                st.model.chokeFadeActive = model->attrOr("chokeFade_active") == "true";
                st.model.chokeFadeBase = float(model->attrNum("chokeFade_base", 1));
                st.model.chokeFadeRange = float(model->attrNum("chokeFade_range", 1));
                st.model.hihatTightenDamp = float(model->attrNum("hihatTighten_damp", 0));
                st.model.hihatTightenPitch = float(model->attrNum("hihatTighten_pitch", 0));
                st.model.tomResActive = model->attrOr("tomRes_active") == "true";
                st.model.resTrim = float(model->attrNum("resTrim", 0.5));
                st.model.spillTrim = float(model->attrNum("spillTrim", 0.5));
            }
            // BFD3 kits carry 64 slots; pieces name their slot explicitly.
            const std::string pname = kp->attrOr("name");
            int slotIndex = -1;
            if (pname.rfind("kp_slot_", 0) == 0) slotIndex = std::atoi(pname.c_str() + 8);
            if (slotIndex < 0) slotIndex = int(out->pieces.size());
            if (slotIndex >= 64) slotIndex = 63;
            if (out->pieces.size() <= size_t(slotIndex)) out->pieces.resize(size_t(slotIndex) + 1);
            if (slotIndex < int(out->slots.size()))
                st.slotRef = out->slots[size_t(slotIndex)];
            out->pieces[size_t(slotIndex)] = std::move(st);
        }
    }
    for (size_t i = 0; i < out->pieces.size(); ++i)
        if (i < out->slots.size() && out->pieces[i].slotRef.empty())
            out->pieces[i].slotRef = out->slots[i];
    return true;
}

std::string pieceStateXml(const KitPieceState& p, size_t index, bool withLayout = false) {
    std::ostringstream o;
    o << "    <BFD2KitPiece name=\"kp_slot_" << index << "\" pan=\"" << p.pan << "\" trim=\""
      << p.trim << "\" dampAmt=\"" << p.dampAmt << "\" dampFreq=\"" << p.dampFreq
      << "\" dampRatio=\"" << p.dampRatio << "\" tune=\"" << p.tune << "\" dyn=\"" << p.dyn
      << "\" loudnessCurve=\"" << p.loudnessCurve << "\" loudnessNorm=\""
      << (p.loudnessNorm ? "true" : "false") << "\" loudnessDynRange=\"" << p.loudnessDynRange
      << "\" flip_amb=\"" << (p.flipAmb ? "true" : "false") << "\" kick_bl_lvl=\""
      << p.kickBleedLvl << "\" snre_bl_lvl=\"" << p.snareBleedLvl << "\" kick_bl_dst=\""
      << p.kickBleedDst << "\" snre_bl_dst=\"" << p.snareBleedDst << "\" bleed_send=\""
      << p.bleedSend << "\" bleed_return=\"" << p.bleedReturn << "\" inputBleedTrim=\""
      << p.bleedInputTrim << "\" amb1Dest=\"" << escapeAttr(p.amb1Dest) << "\" amb2Dest=\""
      << escapeAttr(p.amb2Dest) << "\" amb3Dest=\"" << escapeAttr(p.amb3Dest)
      << "\" amb_master_gain=\"" << p.ambMasterGain << "\" amb_master_gain_link=\""
      << (p.ambMasterGainLink ? "true" : "false") << "\" mute=\"" << (p.mute ? "true" : "false")
      << "\" solo=\"" << (p.solo ? "true" : "false") << "\" micBlend=\"" << p.micBlend
      << "\" chokeGroup=\"" << p.chokeGroup << "\"";
    if (withLayout) o << " ob_stageX=\"" << p.stageX << "\" ob_stageY=\"" << p.stageY << "\"";
    o << ">\n";
    for (size_t j = 0; j < p.arts.size(); ++j) {
        const auto& a = p.arts[j];
        o << "      <BFD2Artic name=\"artic_" << index << "_" << escapeAttr(a.articName)
          << "\" v2d=\"" << a.v2d << "\" v2p=\"" << a.v2p << "\" v2a=\"" << a.v2a
          << "\" aTrim=\"" << a.aTrim << "\" velVar=\"" << a.velVar << "\" artic_name=\""
          << escapeAttr(a.articName) << "\" artic_load_state=\""
          << (a.loadState ? "true" : "false") << "\" AMGVarDepth=\"" << a.amgVarDepth
          << "\" />\n";
    }
    if (!p.ambSends.empty()) {
        o << "      <VObjectArray name=\"ambSends\">\n";
        for (const auto& as : p.ambSends)
            o << "        <BFD2KitPieceAmbSend gain=\"" << as.gain << "\" active=\""
              << (as.active ? "true" : "false") << "\" sourceMic=\"" << escapeAttr(as.sourceMic)
              << "\" destChan=\"" << escapeAttr(as.destChan) << "\" />\n";
        o << "      </VObjectArray>\n";
    }
    o << "      <BFD2KitPieceModel name=\"model\" chokeFade_active=\""
      << (p.model.chokeFadeActive ? "true" : "false") << "\" chokeFade_base=\""
      << p.model.chokeFadeBase << "\" chokeFade_range=\"" << p.model.chokeFadeRange
      << "\" hihatTighten_damp=\"" << p.model.hihatTightenDamp << "\" hihatTighten_pitch=\""
      << p.model.hihatTightenPitch << "\" tomRes_active=\""
      << (p.model.tomResActive ? "true" : "false") << "\" resTrim=\"" << p.model.resTrim
      << "\" spillTrim=\"" << p.model.spillTrim << "\" />\n";
    o << "    </BFD2KitPiece>\n";
    return o.str();
}

} // namespace

bool loadKit(const std::string& path, Kit* out, std::string* error) {
    Preset preset;
    if (!loadPreset(path, &preset, error)) return false;
    *out = std::move(preset.kit);
    return true;
}

bool saveKit(const std::string& path, const Kit& kit, std::string* error) {
    // .bfd3 -> full program preset; anything else -> bare kit.
    const auto dot = path.find_last_of('.');
    const std::string ext = dot == std::string::npos ? std::string() : path.substr(dot);
    if (ext == ".bfd3") {
        Preset p;
        p.kit = kit;
        p.valid = true;
        return savePreset(path, p, error);
    }
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        if (error) *error = "cannot write " + path;
        return false;
    }
    f << kitToXml(kit, false);
    return true;
}

std::string kitToXml(const Kit& kit, bool withLayout) {
    std::ostringstream o;
    o << "<root MinimumRequiredEngineVersion=\"3.5\">\n";
    o << "  <BFD2KitInfo info_name=\"" << escapeAttr(kit.name) << "\" info_library=\""
      << escapeAttr(kit.library) << "\" info_librarylong=\"" << escapeAttr(kit.libraryLong)
      << "\">\n";
    for (size_t i = 0; i < kit.slots.size(); ++i)
        o << "    <VString name=\"kit_slot_" << i << "\" string=\""
          << escapeAttr(slotRefToFile(kit.slots[i])) << "\" />\n";
    o << "  </BFD2KitInfo>\n";
    o << "  <BFD2Kit masterDynamics=\"" << kit.masterDynamics << "\" kit_size=\"" << kit.size
      << "\" SourceEngine=\"" << escapeAttr(kit.sourceEngine) << "\" SourceEngineVersion=\""
      << escapeAttr(kit.sourceEngineVersion) << "\">\n";
    for (size_t i = 0; i < kit.pieces.size(); ++i) o << pieceStateXml(kit.pieces[i], i, withLayout);
    o << "  </BFD2Kit>\n</root>\n";
    return o.str();
}

bool kitFromXml(const std::string& xml, Kit* out, std::string* error) {
    std::vector<XmlElement> roots;
    if (!parseXml(xml, &roots, error)) return false;
    const XmlElement* info = nullptr;
    const XmlElement* kit = nullptr;
    for (const auto& r : roots) {
        if (!info) info = findDeep(r, "BFD2KitInfo");
        if (!kit) kit = findDeep(r, "BFD2Kit");
    }
    if (!info && !kit) {
        if (error) *error = "no kit in XML";
        return false;
    }
    *out = Kit{};
    return parseKitState(info, kit, out, error);
}

// ---------------------------------------------------------------------------
// Preset (.bfd3 program) round-trip
// ---------------------------------------------------------------------------

bool loadPreset(const std::string& path, Preset* out, std::string* error) {
    std::string xml;
    if (!readFile(path, &xml)) {
        if (error) *error = "cannot read " + path;
        return false;
    }
    std::vector<XmlElement> roots;
    if (!parseXml(xml, &roots, error)) return false;
    if (roots.empty()) {
        if (error) *error = "empty XML document";
        return false;
    }

    // Capture the root element's attributes verbatim so a save reproduces
    // them (loop/ppqn/bpm/timesig etc. live there).
    {
        std::ostringstream o;
        for (const auto& kv : roots[0].attrs)
            o << " " << kv.first << "=\"" << escapeAttr(kv.second) << "\"";
        out->rootAttrs = o.str();
    }

    const XmlElement* info = nullptr;
    const XmlElement* kit = nullptr;
    for (const auto& r : roots) {
        if (!info) info = findDeep(r, "BFD2KitInfo");
        if (!kit) kit = findDeep(r, "BFD2Kit");
    }
    if (!info && !kit) {
        if (error) *error = "no BFD2KitInfo / BFD2Kit in " + path;
        return false;
    }

    if (info) {
        std::ostringstream o;
        for (const auto& kv : info->attrs) o << " " << kv.first << "=\"" << escapeAttr(kv.second) << "\"";
        out->kitInfoAttrs = o.str();
        for (const auto& c : info->children) {
            if (c.name != "VString") continue;
            // slots are regenerated from the kit on save
            if (c.attrOr("name").rfind("kit_slot_", 0) == 0) continue;
            std::ostringstream vo;
            for (const auto& kv : c.attrs) vo << " " << kv.first << "=\"" << escapeAttr(kv.second) << "\"";
            out->kitInfoStrings.push_back("      <VString" + vo.str() + " />");
        }
    }

    // Pass-through subtrees we do not model but must preserve.
    auto grab = [&](const char* name, std::string* dest) {
        std::vector<const XmlElement*> found;
        for (const auto& r : roots) collectDeep(r, name, &found);
        if (found.empty()) return;
        std::ostringstream o;
        for (const auto* e : found) o << serializeElement(*e, 2);
        *dest = o.str();
    };
    grab("BFD2GrooveBundle", &out->grooveBundleXml);
    grab("BFD2PatternManagerSettings", &out->patternManagerXml);
    grab("BFD2MidiMap", &out->midiMapXml);
    grab("BFD2Synth", &out->synthXml);
    grab("BFD2Mixer", &out->mixerXml);
    grab("BFD2Song", &out->songXml);
    grab("BFD2AutomationMap", &out->automationXml);
    grab("BFD2ProgramInfo", &out->programInfoXml);

    out->kit = Kit{};
    if (!parseKitState(info, kit, &out->kit, error)) return false;
    out->valid = true;
    return true;
}

bool savePreset(const std::string& path, const Preset& preset, std::string* error) {
    const Kit& kit = preset.kit;
    std::ostringstream o;
    o << "<root" << (preset.rootAttrs.empty() ? " MinimumRequiredEngineVersion=\"3.5\"" : preset.rootAttrs)
      << ">\n";
    if (!preset.programInfoXml.empty()) o << "  " << preset.programInfoXml;
    o << "  <BFD2KitInfo" << (preset.kitInfoAttrs.empty()
                                  ? " name=\"kit_info\" info_inhrt=\"false\" info_name=\"" +
                                        escapeAttr(kit.name) + "\" info_library=\"" +
                                        escapeAttr(kit.library) + "\" info_librarylong=\"" +
                                        escapeAttr(kit.libraryLong) + "\""
                                  : preset.kitInfoAttrs)
      << ">\n";
    for (const auto& vs : preset.kitInfoStrings) o << vs << "\n";
    // slot list (kit_slot_N) -- regenerate so edits round-trip
    for (size_t i = 0; i < kit.slots.size(); ++i)
        o << "      <VString name=\"kit_slot_" << i << "\" string=\""
          << escapeAttr(slotRefToFile(kit.slots[i])) << "\" />\n";
    o << "  </BFD2KitInfo>\n";
    o << "  <BFD2Kit name=\"kit_meta_info\" SourceEngine=\"" << escapeAttr(kit.sourceEngine)
      << "\" masterDynamics=\"" << kit.masterDynamics << "\" kit_size=\"" << kit.size << "\">\n";
    for (size_t i = 0; i < kit.pieces.size(); ++i) o << pieceStateXml(kit.pieces[i], i);
    o << "  </BFD2Kit>\n";
    if (!preset.grooveBundleXml.empty()) o << preset.grooveBundleXml;
    if (!preset.patternManagerXml.empty()) o << preset.patternManagerXml;
    if (!preset.midiMapXml.empty()) o << preset.midiMapXml;
    if (!preset.synthXml.empty()) o << preset.synthXml;
    if (!preset.mixerXml.empty()) o << preset.mixerXml;
    if (!preset.songXml.empty()) o << preset.songXml;
    if (!preset.automationXml.empty()) o << preset.automationXml;
    o << "</root>\n";

    std::ofstream f(path, std::ios::binary);
    if (!f) {
        if (error) *error = "cannot write " + path;
        return false;
    }
    f << o.str();
    return true;
}

// --- keymap ---------------------------------------------------------------

std::vector<KeymapEntry> defaultGmKeymap() {
    // (note, slot, articulation). Slots follow the layout every shipped BFD
    // kit uses: 0 kick, 1 snare, 2 hihat, 3 floor tom, 4 mid tom, 5 high tom,
    // 6 crash 1, 7 crash 2, 8 ride, 9+ percussion.
    struct Raw {
        int note, slot;
        const char* art;
    };
    static const Raw raw[] = {
        {35, 0, "Hit"},  {36, 0, "Hit"},   {37, 1, "Rim Click"}, {38, 1, "Hit"},
        {39, 9, "Hit"},  {40, 1, "Rim"},   {41, 3, "Hit"},       {42, 2, "ClosedT"},
        {43, 3, "Hit"},  {44, 2, "Pedal"}, {45, 4, "Hit"},       {46, 2, "OpenT"},
        {47, 4, "Hit"},  {48, 5, "Hit"},   {49, 6, "Edge"},      {50, 5, "Hit"},
        {51, 8, "Bow"},  {52, 7, "Edge"},  {53, 8, "Bell"},      {54, 9, "Hit"},
        {55, 6, "Edge"}, {56, 9, "Hit"},   {57, 7, "Edge"},      {59, 8, "Bow"},
    };
    std::vector<KeymapEntry> out;
    for (const auto& r : raw) out.push_back({r.note, r.slot, r.art});
    return out;
}

// --- piece kinds ----------------------------------------------------------

namespace {
bool containsNoCase(const std::string& hay, const char* needle) {
    std::string h = hay, n = needle;
    for (auto& c : h) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (auto& c : n) c = char(std::tolower(static_cast<unsigned char>(c)));
    return h.find(n) != std::string::npos;
}
} // namespace

PieceKind pieceKind(const Piece& piece) {
    const std::string& k = piece.klass;
    const std::string label = piece.name + " " + piece.directory;
    if (k == "Kick") return PieceKind::Kick;
    if (k == "Snare") return PieceKind::Snare;
    if (k == "Hihat") return PieceKind::Hihat;
    if (k == "Tom") {
        if (containsNoCase(piece.name, "floor")) return PieceKind::FloorTom;
        if (containsNoCase(piece.name, "tom") || containsNoCase(piece.name, "rack"))
            return piece.diameter >= 14.0f ? PieceKind::FloorTom : PieceKind::Tom;
        // "Tom" is also BFD's catch-all class for hand percussion
        return piece.diameter >= 14.0f ? PieceKind::FloorTom
                                       : (piece.diameter > 0.0f ? PieceKind::Tom : PieceKind::Perc);
    }
    if (k == "Cymbal") {
        if (containsNoCase(piece.name, "ride")) return PieceKind::Ride;
        if (containsNoCase(piece.name, "splash")) return PieceKind::Splash;
        if (containsNoCase(piece.name, "china") || containsNoCase(piece.name, "stack"))
            return PieceKind::China;
        return PieceKind::Crash;
    }
    if (k == "Percussion") return PieceKind::Perc;
    // unclassified pieces: guess from the name
    if (containsNoCase(label, "kick")) return PieceKind::Kick;
    if (containsNoCase(label, "snare")) return PieceKind::Snare;
    if (containsNoCase(label, "hihat") || containsNoCase(label, "hi-hat")) return PieceKind::Hihat;
    if (containsNoCase(label, "ride")) return PieceKind::Ride;
    if (containsNoCase(label, "crash")) return PieceKind::Crash;
    if (containsNoCase(label, "tom")) return PieceKind::Tom;
    return PieceKind::Perc;
}

const char* pieceKindCode(PieceKind kind) {
    switch (kind) {
        case PieceKind::Kick: return "KICK";
        case PieceKind::Snare: return "SNR";
        case PieceKind::Hihat: return "HAT";
        case PieceKind::Tom: return "TOM";
        case PieceKind::FloorTom: return "FLR";
        case PieceKind::Crash: return "CRS";
        case PieceKind::Ride: return "RIDE";
        case PieceKind::Splash: return "SPL";
        case PieceKind::China: return "CHN";
        case PieceKind::Perc: return "PERC";
    }
    return "PERC";
}

float pieceDiameter(const Piece& piece) {
    if (piece.diameter > 0.0f) return piece.diameter;
    switch (pieceKind(piece)) {
        case PieceKind::Kick: return 22.0f;
        case PieceKind::Snare: return 14.0f;
        case PieceKind::Hihat: return 14.0f;
        case PieceKind::Tom: return 12.0f;
        case PieceKind::FloorTom: return 16.0f;
        case PieceKind::Crash: return 18.0f;
        case PieceKind::Ride: return 21.0f;
        case PieceKind::Splash: return 10.0f;
        case PieceKind::China: return 18.0f;
        case PieceKind::Perc: return 8.0f;
    }
    return 12.0f;
}

bool isOwnChannel(PieceKind kind, const ChanInfo& ch) {
    if (ch.isAmbient()) return false;
    if (kind == PieceKind::Kick) return ch.type == "dirKick";
    if (kind == PieceKind::Snare) return ch.type == "dirSnare";
    return ch.type == "dirOther";
}

// --- library discovery ------------------------------------------------------

std::vector<std::string> discoverLibraryRoots() {
    std::vector<std::string> out;
    const char* home = std::getenv("HOME");
    if (!home) return out;
    std::string xml;
    if (!readFile(std::string(home) + "/Library/Application Support/BFD Drums/BFD3/DataPaths.xml", &xml))
        return out;
    std::vector<XmlElement> roots;
    if (!parseXml(xml, &roots)) return out;
    std::vector<const XmlElement*> paths;
    for (const auto& r : roots) collectDeep(r, "DataPath", &paths);
    for (const XmlElement* dp : paths) {
        const std::string path = dp->attrOr("path");
        if (path.empty() || dp->attrOr("enabled", "true") != "true") continue;
        std::error_code ec;
        if (!fs::is_directory(fs::path(path) / "Audio", ec)) continue;
        if (std::find(out.begin(), out.end(), path) == out.end()) out.push_back(path);
    }
    return out;
}

std::vector<KitFileEntry> listKitFiles(const std::string& libraryRoot) {
    std::vector<KitFileEntry> out;
    const std::string libName = fs::path(libraryRoot).filename().string();
    for (const char* sub : {"Kits", "Presets"}) {
        std::error_code ec;
        const fs::path dir = fs::path(libraryRoot) / sub;
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& e : fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
            if (!e.is_regular_file(ec)) continue;
            const std::string ext = e.path().extension().string();
            const bool preset = ext == ".bfd3" || ext == ".bfd2";
            if (!preset && ext != ".bfd2kit" && ext != ".bfd3kit") continue;
            if (e.path().filename().string().rfind("._", 0) == 0) continue; // AppleDouble
            out.push_back({e.path().string(), e.path().stem().string(), libName, preset});
        }
    }
    std::sort(out.begin(), out.end(), [](const KitFileEntry& a, const KitFileEntry& b) {
        return a.preset != b.preset ? !a.preset : a.name < b.name;
    });
    return out;
}

// --- MIDI maps --------------------------------------------------------------

std::vector<KeymapEntry> parseMidiMap(const std::string& midiMapXml) {
    std::vector<KeymapEntry> out;
    if (midiMapXml.empty()) return out;
    std::vector<XmlElement> roots;
    if (!parseXml(midiMapXml, &roots)) return out;
    std::vector<const XmlElement*> notes;
    for (const auto& r : roots) collectDeep(r, "VObjectArray", &notes);
    for (const XmlElement* arr : notes) {
        const std::string name = arr->attrOr("name");
        if (name.rfind("midi_note_", 0) != 0) continue;
        const int note = std::atoi(name.c_str() + 10);
        if (note < 0 || note > 127) continue;
        for (const XmlElement* el : arr->childrenNamed("MIDIMapElement")) {
            if (el->attrOr("isgrv") == "true") continue;
            const XmlElement* req = el->child("RequestedArticulationID");
            if (!req) continue;
            KeymapEntry k;
            k.note = note;
            k.slot = int(req->attrNum("slot", -1));
            k.articIndex = int(req->attrNum("artic", -1));
            if (k.slot >= 0 && k.slot < Kit::kMaxSlots) out.push_back(k);
            break; // one target per note
        }
    }
    return out;
}

std::vector<KeymapEntry> autoKeymap(const Kit& kit, const std::vector<const Piece*>& pieceAt) {
    (void) kit;
    struct Item {
        int slot;
        const Piece* piece;
        PieceKind kind;
    };
    std::vector<Item> items;
    for (size_t s = 0; s < pieceAt.size(); ++s)
        if (pieceAt[s]) items.push_back({int(s), pieceAt[s], pieceKind(*pieceAt[s])});
    auto ofKind = [&](std::initializer_list<PieceKind> kinds) {
        std::vector<Item> v;
        for (const auto& it : items)
            for (PieceKind k : kinds)
                if (it.kind == k) v.push_back(it);
        return v;
    };
    std::vector<KeymapEntry> out;
    std::vector<char> used(128, 0);
    auto map = [&](int note, int slot, const char* art) {
        if (note < 0 || note > 127 || used[size_t(note)]) return;
        used[size_t(note)] = 1;
        out.push_back({note, slot, art, -1});
    };

    const auto kicks = ofKind({PieceKind::Kick});
    if (!kicks.empty()) {
        map(36, kicks[0].slot, "Hit");
        map(35, kicks.size() > 1 ? kicks[1].slot : kicks[0].slot, "Hit");
    }
    const auto snares = ofKind({PieceKind::Snare});
    if (!snares.empty()) {
        const int s = snares[0].slot;
        map(38, s, "Hit");
        map(40, s, "Rim");
        map(37, s, "Rim Click");
        map(39, snares.size() > 1 ? snares[1].slot : s, snares.size() > 1 ? "Hit" : "Flam");
    }
    const auto hats = ofKind({PieceKind::Hihat});
    if (!hats.empty()) {
        map(42, hats[0].slot, "ClosedT");
        map(44, hats[0].slot, "Pedal");
        map(46, hats[0].slot, "OpenT");
    }
    auto toms = ofKind({PieceKind::Tom, PieceKind::FloorTom});
    std::stable_sort(toms.begin(), toms.end(), [](const Item& a, const Item& b) {
        return pieceDiameter(*a.piece) < pieceDiameter(*b.piece);
    });
    if (!toms.empty()) {
        static const int kTomNotes[] = {50, 48, 47, 45, 43, 41}; // high -> low
        const int n = int(toms.size());
        for (int k = 0; k < 6; ++k) {
            const int i = n == 1 ? 0 : int(std::lround(double(k) * (n - 1) / 5.0));
            map(kTomNotes[k], toms[size_t(std::min(i, n - 1))].slot, "Hit");
        }
    }
    const auto rides = ofKind({PieceKind::Ride});
    if (!rides.empty()) {
        map(51, rides[0].slot, "Bow");
        map(53, rides[0].slot, "Bell");
        map(59, rides.size() > 1 ? rides[1].slot : rides[0].slot, rides.size() > 1 ? "Bow" : "Edge");
    }
    const auto crashes = ofKind({PieceKind::Crash});
    const auto splashes = ofKind({PieceKind::Splash});
    const auto chinas = ofKind({PieceKind::China});
    std::vector<Item> cymbalQueue = crashes;
    if (!crashes.empty()) map(49, crashes[0].slot, "Edge");
    if (crashes.size() > 1) map(57, crashes[1].slot, "Edge");
    if (!splashes.empty()) map(55, splashes[0].slot, "Edge");
    if (!chinas.empty()) map(52, chinas[0].slot, "Edge");
    // remaining cymbals onto the remaining cymbal notes
    {
        std::vector<Item> rest;
        for (size_t i = 2; i < crashes.size(); ++i) rest.push_back(crashes[i]);
        for (size_t i = 1; i < splashes.size(); ++i) rest.push_back(splashes[i]);
        for (size_t i = 1; i < chinas.size(); ++i) rest.push_back(chinas[i]);
        if (crashes.size() == 1) rest.insert(rest.begin(), crashes[0]);
        size_t r = 0;
        for (int note : {57, 55, 52}) {
            if (used[size_t(note)]) continue;
            if (r < rest.size()) map(note, rest[r++].slot, "Edge");
        }
    }
    const auto percs = ofKind({PieceKind::Perc});
    static const int kPercNotes[] = {56, 54, 39, 58, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70,
                                     75, 76, 77, 81, 82, 83, 84, 85, 86, 87};
    size_t pn = 0;
    for (const auto& it : percs) {
        while (pn < std::size(kPercNotes) && used[size_t(kPercNotes[pn])]) ++pn;
        if (pn >= std::size(kPercNotes)) break;
        map(kPercNotes[pn++], it.slot, "Hit");
    }
    std::sort(out.begin(), out.end(), [](const KeymapEntry& a, const KeymapEntry& b) { return a.note < b.note; });
    return out;
}

} // namespace bfdmodel
