#include "Mixer.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>

namespace bfdengine {
using bfdmodel::ChanInfo;
using bfdmodel::Piece;
using bfdmodel::PieceKind;

// --- MixerState -------------------------------------------------------------

Jack MixerState::jackOf(const std::string& key) const {
    auto it = patch.find(key);
    return it == patch.end() ? Jack{} : it->second;
}

const std::string* MixerState::micAt(int strip, int side) const {
    for (const auto& [key, jack] : patch)
        if (jack.strip == strip && jack.side == side) return &key;
    return nullptr;
}

void MixerState::plug(const std::string& key, Jack jack) {
    if (!jack.valid()) {
        unplug(key);
        return;
    }
    for (auto it = patch.begin(); it != patch.end();)
        it = (it->second == jack && it->first != key) ? patch.erase(it) : std::next(it);
    patch[key] = jack;
}

void MixerState::unplug(const std::string& key) { patch.erase(key); }

StripMode MixerState::mode(int strip, const std::vector<MicSource>& mics) const {
    const std::string* l = micAt(strip, 0);
    const std::string* r = micAt(strip, 1);
    if (!l && !r) return StripMode::Empty;
    if (l && !r) return StripMode::MonoL;
    if (!l && r) return StripMode::MonoR;
    auto encodingOf = [&](const std::string& key) -> std::string {
        for (const auto& m : mics)
            if (m.key == key) return m.encoding;
        return {};
    };
    if (encodingOf(*l) == "stereoMid" && encodingOf(*r) == "stereoSide") return StripMode::MidSide;
    return StripMode::Stereo;
}

// --- fader law ----------------------------------------------------------------

float faderGain(float pos) {
    const float x = std::clamp(pos, 0.0f, 1.0f) / 0.8f;
    return x * x * x;
}

float faderDb(float pos) {
    const float g = faderGain(pos);
    return g <= 1.0e-6f ? -120.0f : 20.0f * std::log10(g);
}

// --- sources ------------------------------------------------------------------

namespace {

int kindRank(PieceKind k) {
    switch (k) {
        case PieceKind::Kick: return 0;
        case PieceKind::Snare: return 1;
        case PieceKind::Hihat: return 2;
        case PieceKind::Tom: return 3;
        case PieceKind::FloorTom: return 4;
        case PieceKind::Crash: return 5;
        case PieceKind::Splash: return 6;
        case PieceKind::China: return 7;
        case PieceKind::Ride: return 8;
        case PieceKind::Perc: return 9;
    }
    return 9;
}

bool hasOtherBleed(const Piece& p) {
    const PieceKind k = bfdmodel::pieceKind(p);
    if (k != PieceKind::Kick && k != PieceKind::Snare) return false;
    for (const auto& ch : p.channels)
        if (ch.type == "dirOther") return true;
    return false;
}

std::string escapeAttr(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

} // namespace

std::vector<MicSource> listMics(const bfdmodel::Kit& kit, const PieceLookup& pieces) {
    (void) kit;
    std::vector<MicSource> out;

    std::vector<int> order;
    for (size_t s = 0; s < pieces.size(); ++s)
        if (pieces[s]) order.push_back(int(s));
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const Piece& pa = *pieces[size_t(a)];
        const Piece& pb = *pieces[size_t(b)];
        const int ra = kindRank(bfdmodel::pieceKind(pa)), rb = kindRank(bfdmodel::pieceKind(pb));
        if (ra != rb) return ra < rb;
        const float da = bfdmodel::pieceDiameter(pa), db = bfdmodel::pieceDiameter(pb);
        if (ra == kindRank(PieceKind::Tom) && da != db) return da < db; // high toms first
        return a < b;
    });

    bool otherBleed = false;
    for (int s : order) {
        const Piece& p = *pieces[size_t(s)];
        const PieceKind kind = bfdmodel::pieceKind(p);
        for (const ChanInfo& ch : p.channels) {
            if (!bfdmodel::isOwnChannel(kind, ch)) continue;
            MicSource m;
            m.kind = MicKind::Direct;
            m.slot = s;
            m.chanName = ch.name;
            m.group = bfdmodel::pieceKindCode(kind);
            m.encoding = "mono";
            m.micIndex = ch.micIndex;
            m.key = "D:" + std::to_string(s) + ":" + ch.name;
            out.push_back(std::move(m));
        }
        otherBleed = otherBleed || hasOtherBleed(p);
    }

    std::set<std::string> seen;
    for (int s : order) {
        for (const ChanInfo& ch : pieces[size_t(s)]->channels) {
            if (!ch.isAmbient() || !seen.insert(ch.name).second) continue;
            MicSource m;
            m.kind = MicKind::Ambient;
            m.chanName = ch.name;
            m.group = ch.group();
            m.encoding = ch.encoding;
            m.micIndex = ch.micIndex;
            m.key = "A:" + ch.name;
            out.push_back(std::move(m));
        }
    }

    if (otherBleed) {
        MicSource m;
        m.kind = MicKind::Bleed;
        m.chanName = "Other Bleed";
        m.group = "BLEED";
        m.encoding = "mono";
        m.key = "B:Other";
        out.push_back(std::move(m));
    }
    return out;
}

void autoPatch(const std::vector<MicSource>& mics, MixerState& state, bool prune,
               const std::set<std::string>* only) {
    if (prune) {
        std::set<std::string> live;
        for (const auto& m : mics) live.insert(m.key);
        for (auto it = state.patch.begin(); it != state.patch.end();)
            it = live.count(it->first) ? std::next(it) : state.patch.erase(it);
    }
    auto wanted = [&](const std::string& key) { return !only || only->count(key) > 0; };

    auto stripFree = [&](int s) { return !state.micAt(s, 0) && !state.micAt(s, 1); };
    auto nextFree = [&]() {
        for (int s = 0; s < kNumStrips; ++s)
            if (stripFree(s)) return s;
        return -1;
    };
    auto patched = [&](const std::string& key) { return state.patch.count(key) > 0; };

    for (size_t i = 0; i < mics.size(); ++i) {
        const MicSource& m = mics[i];
        if (patched(m.key) || !wanted(m.key)) continue;
        // stereo partner: an ambient Left/Mid followed by its Right/Side
        const MicSource* partner = nullptr;
        if (m.kind == MicKind::Ambient && (m.encoding == "stereoLeft" || m.encoding == "stereoMid")) {
            const char* want = m.encoding == "stereoLeft" ? "stereoRight" : "stereoSide";
            for (const auto& o : mics)
                if (o.kind == MicKind::Ambient && o.group == m.group && o.encoding == want && !patched(o.key) &&
                    wanted(o.key)) {
                    partner = &o;
                    break;
                }
        }
        if (m.kind == MicKind::Ambient && (m.encoding == "stereoRight" || m.encoding == "stereoSide")) {
            // the partner of a lone Right/Side may already sit on a strip with a free R jack
            const char* want = m.encoding == "stereoRight" ? "stereoLeft" : "stereoMid";
            for (const auto& o : mics)
                if (o.kind == MicKind::Ambient && o.group == m.group && o.encoding == want && patched(o.key)) {
                    const Jack j = state.jackOf(o.key);
                    if (j.side == 0 && !state.micAt(j.strip, 1)) {
                        state.plug(m.key, {j.strip, 1});
                        break;
                    }
                }
            if (patched(m.key)) continue;
        }
        const int s = nextFree();
        if (s < 0) return; // out of strips: the rest stays unpatched
        state.plug(m.key, {s, 0});
        if (partner) state.plug(partner->key, {s, 1});
    }
}

// --- BFD2Mixer import -------------------------------------------------------------

void importBfdMixer(const std::string& mixerXml, const std::vector<MicSource>& mics, MixerState& state) {
    if (mixerXml.empty()) return;
    std::vector<bfdmodel::XmlElement> roots;
    if (!bfdmodel::parseXml(mixerXml, &roots)) return;
    const bfdmodel::XmlElement* mixer = nullptr;
    std::vector<const bfdmodel::XmlElement*> stack;
    for (const auto& r : roots) stack.push_back(&r);
    while (!stack.empty() && !mixer) {
        const auto* e = stack.back();
        stack.pop_back();
        if (e->name == "BFD2Mixer") mixer = e;
        for (const auto& c : e->children) stack.push_back(&c);
    }
    if (!mixer) return;
    state.masterKickBleed = float(mixer->attrNum("masterKickBleed", state.masterKickBleed));
    state.masterSnareBleed = float(mixer->attrNum("masterSnareBleed", state.masterSnareBleed));
    state.masterOtherBleed = float(mixer->attrNum("masterOtherBleed", state.masterOtherBleed));

    std::set<int> touched;
    auto apply = [&](const MicSource& m, const bfdmodel::XmlElement& ch) {
        const Jack j = state.jackOf(m.key);
        if (!j.valid() || !touched.insert(j.strip).second) return;
        StripState& st = state.strips[size_t(j.strip)];
        st.gain = float(ch.attrNum("gain", 0.8));
        st.pan = float(ch.attrNum("pan", 0.5));
        st.mute = ch.attrOr("mute") == "true";
        st.solo = ch.attrOr("solo") == "true";
    };
    for (const auto& arr : mixer->children) {
        if (arr.name != "VObjectArray" || arr.attrOr("name") != "MixerChannels") continue;
        for (const auto& ch : arr.children) {
            if (ch.name != "BFD2MixerChan") continue;
            const int type = int(ch.attrNum("chanType", -1));
            const std::string name = ch.attrOr("chanName");
            if (type == 2) {
                const int slot = int(ch.attrNum("slotID", -1));
                const int micId = int(ch.attrNum("micID", -1));
                const MicSource* hit = nullptr;
                for (const auto& m : mics)
                    if (m.kind == MicKind::Direct && m.slot == slot && m.chanName == name) hit = &m;
                if (!hit)
                    for (const auto& m : mics)
                        if (m.kind == MicKind::Direct && m.slot == slot && m.micIndex == micId) {
                            hit = &m;
                            break;
                        }
                if (hit) apply(*hit, ch);
            } else if (type == 1) {
                for (const auto& m : mics)
                    if (m.kind == MicKind::Ambient && (m.group == name || m.chanName == name)) apply(m, ch);
            } else if (type == 4) {
                state.masterGain = float(ch.attrNum("gain", 0.8));
            }
        }
    }
}

// --- plugin state -----------------------------------------------------------------

std::string mixerToXml(const MixerState& state) {
    std::ostringstream o;
    o << "<OpenBfdMixer master=\"" << state.masterGain << "\" kickBleed=\"" << state.masterKickBleed
      << "\" snareBleed=\"" << state.masterSnareBleed << "\" otherBleed=\"" << state.masterOtherBleed
      << "\">\n";
    for (int s = 0; s < kNumStrips; ++s) {
        const StripState& st = state.strips[size_t(s)];
        o << "  <Strip index=\"" << s << "\" gain=\"" << st.gain << "\" pan=\"" << st.pan << "\" mute=\""
          << (st.mute ? "true" : "false") << "\" solo=\"" << (st.solo ? "true" : "false") << "\" />\n";
    }
    for (const auto& [key, jack] : state.patch)
        o << "  <Patch mic=\"" << escapeAttr(key) << "\" strip=\"" << jack.strip << "\" side=\"" << jack.side
          << "\" />\n";
    o << "</OpenBfdMixer>\n";
    return o.str();
}

bool mixerFromXml(const std::string& xml, MixerState* out) {
    std::vector<bfdmodel::XmlElement> roots;
    if (!bfdmodel::parseXml(xml, &roots) || roots.empty() || roots[0].name != "OpenBfdMixer") return false;
    const auto& r = roots[0];
    MixerState st;
    st.masterGain = float(r.attrNum("master", 0.8));
    st.masterKickBleed = float(r.attrNum("kickBleed", 0.8));
    st.masterSnareBleed = float(r.attrNum("snareBleed", 0.8));
    st.masterOtherBleed = float(r.attrNum("otherBleed", 0.8));
    for (const auto& c : r.children) {
        if (c.name == "Strip") {
            const int i = int(c.attrNum("index", -1));
            if (i < 0 || i >= kNumStrips) continue;
            auto& s = st.strips[size_t(i)];
            s.gain = float(c.attrNum("gain", 0.8));
            s.pan = float(c.attrNum("pan", 0.5));
            s.mute = c.attrOr("mute") == "true";
            s.solo = c.attrOr("solo") == "true";
        } else if (c.name == "Patch") {
            const Jack j{int(c.attrNum("strip", -1)), int(c.attrNum("side", 0))};
            if (j.valid()) st.patch[c.attrOr("mic")] = j;
        }
    }
    *out = std::move(st);
    return true;
}

} // namespace bfdengine
