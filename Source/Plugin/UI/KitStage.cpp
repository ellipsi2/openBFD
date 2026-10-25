#include "KitStage.h"

#include "../PluginProcessor.h"
#include "KitArt.h"
#include "Motion.h"
#include "Theme.h"
#include "Widgets.h"

namespace openbfd::ui {

using bfdengine::MicKind;
using bfdmodel::PieceKind;
using P = juce::Point<float>;

namespace {

constexpr float kPi = juce::MathConstants<float>::pi;
const P kDrummer{0.0f, 26.0f};
const P kKitCentre{0.0f, 2.0f};

// Default spots (inches) per kind, in the order pieces of that kind take them.
const std::vector<P>& spots(PieceKind k) {
    static const std::map<PieceKind, std::vector<P>> table = {
        {PieceKind::Kick, {{0, 2}, {-24, 0}, {24, 0}}},
        {PieceKind::Snare, {{-11, 14}, {-42, 16}, {-42, -2}, {14, 30}}},
        {PieceKind::Hihat, {{-27, 10}, {32, 28}, {-42, 26}}},
        {PieceKind::Tom, {{-7, -10}, {8, -10}, {21, -4}, {-20, -6}, {-2, -22}, {30, -16}}},
        {PieceKind::FloorTom, {{24, 15}, {42, 20}, {42, 3}}},
        {PieceKind::Ride, {{37, -7}, {46, -18}}},
        {PieceKind::Crash, {{-25, -20}, {17, -25}, {-40, -27}, {32, -30}}},
        {PieceKind::Splash, {{-6, -28}, {4, -33}, {-38, -8}}},
        {PieceKind::China, {{43, -27}, {-45, -14}}},
        {PieceKind::Perc, {}},
    };
    return table.at(k);
}

P percSpot(int i) {
    // row along the front of the riser, left of the throne then right of it
    const float xs[] = {-38, -29, -20, -12, 12, 20, 29, 38};
    if (i < 8) return {xs[i], 34.0f};
    return {-40.0f + float((i - 8) % 9) * 10.0f, 25.0f - float((i - 8) / 9) * 9.0f};
}

PieceKind guessKind(const juce::String& ref) {
    const auto s = ref.fromLastOccurrenceOf("/", false, false).toLowerCase();
    if (s.contains("kick") || s.contains("bass drum")) return PieceKind::Kick;
    if (s.contains("snare")) return PieceKind::Snare;
    if (s.contains("hat")) return PieceKind::Hihat;
    if (s.contains("floor")) return PieceKind::FloorTom;
    if (s.contains("tom")) return PieceKind::Tom;
    if (s.contains("ride")) return PieceKind::Ride;
    if (s.contains("splash")) return PieceKind::Splash;
    if (s.contains("china")) return PieceKind::China;
    if (s.contains("crash") || s.contains("cymbal")) return PieceKind::Crash;
    return PieceKind::Perc;
}

float fallbackDiameter(PieceKind k) {
    switch (k) {
        case PieceKind::Kick: return 22;
        case PieceKind::Snare: return 14;
        case PieceKind::Hihat: return 14;
        case PieceKind::Tom: return 12;
        case PieceKind::FloorTom: return 16;
        case PieceKind::Crash: return 18;
        case PieceKind::Ride: return 21;
        case PieceKind::Splash: return 10;
        case PieceKind::China: return 18;
        case PieceKind::Perc: return 8;
    }
    return 12;
}

juce::String twoDigits(int n) { return juce::String(n).paddedLeft('0', 2); }

juce::String noteName(int note) {
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return note < 0 ? dashText() : juce::String(note) + " " + names[note % 12] + juce::String(note / 12 - 2);
}

float glyphAngleTowards(P from, P to) {
    const auto d = to - from;
    return std::atan2(d.x, -d.y);
}

} // namespace

KitStage::KitStage(UiHost& host) : host_(host) { setRepaintsOnMouseActivity(false); }

//==============================================================================
// model -> view

void KitStage::refresh() {
    layoutKit();
    repaint();
}

void KitStage::layoutKit() {
    auto& proc = host_.processor();
    const auto& kit = proc.kit();
    const auto lookup = proc.pieces();
    pieces_.clear();
    std::map<PieceKind, int> used;
    int percUsed = 0;

    // toms take their spots high -> low (small -> large), so order by size
    std::vector<int> order;
    for (int s = 0; s < int(kit.pieces.size()); ++s)
        if (!kit.pieces[size_t(s)].slotRef.empty()) order.push_back(s);
    auto kindOf = [&](int s) {
        const auto* piece = s < int(lookup.size()) ? lookup[size_t(s)] : nullptr;
        return piece ? bfdmodel::pieceKind(*piece) : guessKind(kit.pieces[size_t(s)].slotRef);
    };
    auto diameterOf = [&](int s) {
        const auto* piece = s < int(lookup.size()) ? lookup[size_t(s)] : nullptr;
        return piece ? bfdmodel::pieceDiameter(*piece) : fallbackDiameter(kindOf(s));
    };
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const auto ka = kindOf(a), kb = kindOf(b);
        if (ka != kb) return int(ka) < int(kb);
        return diameterOf(a) < diameterOf(b);
    });

    for (int s : order) {
        const auto& st = kit.pieces[size_t(s)];
        const auto* piece = s < int(lookup.size()) ? lookup[size_t(s)] : nullptr;
        PieceView v;
        v.slot = s;
        v.kind = kindOf(s);
        v.diameter = diameterOf(s);
        v.missing = piece == nullptr;
        v.name = piece ? juce::String(piece->name)
                       : juce::String(st.slotRef).fromLastOccurrenceOf("/", false, false);
        v.code = twoDigits(s + 1) + " " + bfdmodel::pieceKindCode(v.kind);
        P auto_;
        if (v.kind == PieceKind::Perc) {
            auto_ = percSpot(percUsed++);
        } else {
            const auto& list = spots(v.kind);
            const int i = used[v.kind]++;
            auto_ = i < int(list.size()) ? list[size_t(i)] : percSpot(8 + percUsed++);
        }
        if (st.stageX >= 0.0f && st.stageY >= 0.0f)
            v.pos = {(st.stageX - 0.5f) * 2.0f * kHalfW, (st.stageY - 0.5f) * 2.0f * kHalfH};
        else
            v.pos = auto_;
        pieces_.push_back(v);
    }
    std::stable_sort(pieces_.begin(), pieces_.end(), [](const PieceView& a, const PieceView& b) {
        const int la = pieceLayer(a.kind), lb = pieceLayer(b.kind);
        if (la != lb) return la < lb;
        if (isCymbal(a.kind)) return a.diameter > b.diameter;
        return false;
    });

    // --- mics ---
    mics_.clear();
    const auto& mics = proc.mics();
    std::map<int, std::vector<const bfdengine::MicSource*>> direct;
    std::vector<const bfdengine::MicSource*> ambient;
    for (const auto& m : mics) {
        if (m.kind == MicKind::Direct) direct[m.slot].push_back(&m);
        else if (m.kind == MicKind::Ambient) ambient.push_back(&m);
        else {
            MicView mv;
            mv.key = m.key;
            mv.kind = m.kind;
            mv.pos = {-47.0f, 35.0f};
            mv.out = {1.0f, 0.0f};
            mv.code = "BL";
            mv.caption = "OTHER BL";
            mics_.push_back(mv);
        }
    }
    for (const auto& pv : pieces_) {
        auto it = direct.find(pv.slot);
        if (it == direct.end()) continue;
        const auto& list = it->second;
        const float r = pv.diameter * 0.5f;
        const int n = int(list.size());
        if (pv.kind == PieceKind::Kick) {
            const float front = pv.pos.y - kickDepth(pv.diameter) * 0.5f;
            int other = 0;
            for (const auto* m : list) {
                MicView mv;
                mv.key = m->key;
                mv.slot = pv.slot;
                const auto name = juce::String(m->chanName).toLowerCase();
                if (name.endsWith(" in") || name.contains("inside")) {
                    // inside the shell, through the port
                    mv.pos = {pv.pos.x - r * 0.45f, pv.pos.y + 1.0f};
                    mv.out = {-1.0f, 0.0f};
                } else if (name.endsWith(" out") || name.contains("outside")) {
                    // in front of the reso head, clear of the rack toms
                    mv.pos = {pv.pos.x + r + 3.0f, front + 2.0f};
                    mv.out = {1.0f, 0.0f};
                } else {
                    mv.pos = {pv.pos.x - r - 3.0f - float(other++) * 7.0f, front + 2.0f};
                    mv.out = {-1.0f, 0.0f};
                }
                mv.angle = kPi; // capsule faces the drummer's side: the heads
                mv.code = micCode(*m);
                mv.caption = micCaption(*m, nullptr);
                mics_.push_back(mv);
            }
            continue;
        }
        const auto away = pv.pos - kDrummer;
        const float base = std::atan2(away.y, away.x);
        int top = 0;
        for (int i = 0; i < n; ++i) {
            const auto* m = list[size_t(i)];
            MicView mv;
            mv.key = m->key;
            mv.slot = pv.slot;
            const auto name = juce::String(m->chanName).toLowerCase();
            float a;
            if (name.contains("bot")) {
                a = base - 2.35f; // under the drum, on the drummer's side
                mv.hidden = true;
            } else {
                a = base + (float(top++) - float(n - 1) * 0.5f) * 0.5f;
            }
            const P dir{std::cos(a), std::sin(a)};
            mv.pos = pv.pos + dir * (r + (pv.kind == PieceKind::Perc ? 3.5f : 2.6f));
            mv.angle = glyphAngleTowards(mv.pos, pv.pos);
            mv.out = dir;
            mv.code = micCode(*m);
            mv.caption = micCaption(*m, nullptr);
            mics_.push_back(mv);
        }
    }
    // ambient: overheads hang over the kit; every other ambient mic stands on
    // one rail along the front of the riser, in file order (pairs stay adjacent)
    std::vector<const bfdengine::MicSource*> rail;
    std::vector<std::string> overheadGroups;
    for (const auto* m : ambient) {
        const auto lower = juce::String(m->group).toLowerCase();
        const bool overhead = lower == "oh" || lower.startsWith("over");
        if (!overhead) {
            rail.push_back(m);
            continue;
        }
        // a second overhead pair hangs a little wider and further out
        auto og = std::find(overheadGroups.begin(), overheadGroups.end(), m->group);
        if (og == overheadGroups.end()) og = overheadGroups.insert(overheadGroups.end(), m->group);
        const float k = float(og - overheadGroups.begin());
        MicView mv;
        mv.key = m->key;
        mv.kind = MicKind::Ambient;
        const auto& enc = m->encoding;
        mv.pos = enc == "stereoLeft" ? P(-19 - 9 * k, -27 - 3 * k) : enc == "stereoRight" ? P(19 + 9 * k, -27 - 3 * k)
               : enc == "stereoSide" ? P(4, -29 - 4 * k) : P(-4, -29 - 4 * k);
        mv.angle = glyphAngleTowards(mv.pos, kKitCentre);
        mv.code = micCode(*m);
        mv.caption = micCaption(*m, nullptr);
        mics_.push_back(mv);
    }
    for (size_t i = 0; i < rail.size(); ++i) {
        const auto* m = rail[i];
        MicView mv;
        mv.key = m->key;
        mv.kind = MicKind::Ambient;
        const float span = kHalfW - 6.0f;
        if (rail.size() <= 2) mv.pos = {i == 0 ? -span : span, -36.0f};
        else mv.pos = {-span + 2.0f * span * float(i) / float(rail.size() - 1), -36.0f};
        mv.badgeRow = rail.size() > 4 ? int(i % 2) : 0;
        mv.angle = glyphAngleTowards(mv.pos, kKitCentre);
        mv.code = micCode(*m);
        mv.caption = micCaption(*m, nullptr);
        mics_.push_back(mv);
    }
    if (selected_ >= 0 && std::none_of(pieces_.begin(), pieces_.end(), [&](auto& p) { return p.slot == selected_; }))
        selected_ = -1;
}

//==============================================================================
// geometry + hit tests

void KitStage::resized() {
    auto r = getLocalBounds().toFloat();
    r.removeFromTop(36.0f);
    frame_ = r.reduced(22.0f, 0.0f).withTrimmedBottom(22.0f).withTrimmedTop(14.0f);
    ppi_ = std::min(frame_.getWidth() / (2.0f * kHalfW), frame_.getHeight() / (2.0f * kHalfH));
    origin_ = frame_.getCentre();
}

juce::Point<float> KitStage::piecePos(const PieceView& v) const {
    return gesture_ == Gesture::Piece && dragMoved_ && dragSlot_ == v.slot ? dragPosIn_ : v.pos;
}

const KitStage::PieceView* KitStage::pieceAt(P px) const {
    for (auto it = pieces_.rbegin(); it != pieces_.rend(); ++it)
        if (pieceContains(it->kind, toPx(piecePos(*it)), it->diameter, ppi_, px)) return &*it;
    return nullptr;
}

const KitStage::MicView* KitStage::micAt(P px) const {
    const MicView* best = nullptr;
    float bestD = 12.0f;
    for (const auto& m : mics_) {
        const float d = toPx(m.pos).getDistanceFrom(px);
        if (d < bestD) {
            bestD = d;
            best = &m;
        }
    }
    return best;
}

bool KitStage::micPoint(const std::string& key, P& out) const {
    for (const auto& m : mics_)
        if (m.key == key) {
            P pos = m.pos;
            // mics ride along with a piece being dragged
            if (gesture_ == Gesture::Piece && dragMoved_ && m.slot == dragSlot_)
                for (const auto& pv : pieces_)
                    if (pv.slot == dragSlot_) pos += dragPosIn_ - pv.pos;
            out = toPx(pos);
            return true;
        }
    return false;
}

//==============================================================================
// painting

void KitStage::paint(juce::Graphics& g) {
    g.fillAll(pal().panel);
    paintChrome(g);
    paintPieces(g);
    paintMics(g);
    paintInspector(g);
    paintDropGhost(g);
}

void KitStage::paintChrome(juce::Graphics& g) {
    const auto& p = pal();
    auto& proc = host_.processor();
    // head: caption + readouts
    auto head = getLocalBounds().removeFromTop(36).reduced(14, 0);
    g.setColour(p.text);
    g.setFont(Theme::get().mono(11.0f, true));
    g.drawText("STAGE", head, juce::Justification::centredLeft, false);
    g.setColour(p.textDim);
    g.setFont(Theme::get().mono(10.5f));
    g.drawText("TOP VIEW", head.withTrimmedLeft(52), juce::Justification::centredLeft, false);
    int patched = 0;
    for (const auto& m : mics_)
        if (proc.mixer().jackOf(m.key).valid()) ++patched;
    g.drawText(juce::String(pieces_.size()) + " PCS   " + juce::String(patched) + "/" + juce::String(mics_.size()) + " MICS",
               head, juce::Justification::centredRight, false);

    const auto f = frame_;
    // floor: a dot every 3 inches, aligned to the centre
    g.setColour(p.dotField);
    const float step = 3.0f * ppi_;
    for (float y = origin_.y - std::floor((origin_.y - f.getY()) / step) * step; y <= f.getBottom(); y += step)
        for (float x = origin_.x - std::floor((origin_.x - f.getX()) / step) * step; x <= f.getRight(); x += step)
            g.fillRect(juce::Rectangle<float>(x - 0.5f, y - 0.5f, 1.0f, 1.0f));
    // centre crosshair, broken around the origin
    g.setColour(p.rule);
    const float gap = 10.0f;
    g.fillRect(juce::Rectangle<float>(f.getX(), origin_.y - 0.5f, origin_.x - gap - f.getX(), 1.0f));
    g.fillRect(juce::Rectangle<float>(origin_.x + gap, origin_.y - 0.5f, f.getRight() - origin_.x - gap, 1.0f));
    g.fillRect(juce::Rectangle<float>(origin_.x - 0.5f, f.getY(), 1.0f, origin_.y - gap - f.getY()));
    g.fillRect(juce::Rectangle<float>(origin_.x - 0.5f, origin_.y + gap, 1.0f, f.getBottom() - origin_.y - gap));
    // rulers: a tick every 6 in, a label every 24 in
    const auto riser = juce::Rectangle<float>(2.0f * kHalfW * ppi_, 2.0f * kHalfH * ppi_).withCentre(origin_);
    g.setColour(p.ruleStrong);
    g.drawRect(riser, 1.0f);
    g.setFont(Theme::get().mono(8.5f));
    const int maxX = int(kHalfW) / 6 * 6, maxY = int(kHalfH) / 6 * 6;
    for (int i = -maxX; i <= maxX; i += 6) {
        const float x = origin_.x + float(i) * ppi_;
        const float len = i % 24 == 0 ? 5.0f : 3.0f;
        g.setColour(p.textFaint);
        g.fillRect(juce::Rectangle<float>(x - 0.5f, riser.getY() - len, 1.0f, len));
        g.fillRect(juce::Rectangle<float>(x - 0.5f, riser.getBottom(), 1.0f, len));
        if (i % 24 == 0)
            g.drawText(juce::String(i), juce::Rectangle<float>(x - 16.0f, riser.getY() - 16.0f, 32.0f, 10.0f),
                       juce::Justification::centred, false);
    }
    for (int i = -maxY; i <= maxY; i += 6) {
        const float y = origin_.y + float(i) * ppi_;
        const float len = i % 24 == 0 ? 5.0f : 3.0f;
        g.setColour(p.textFaint);
        g.fillRect(juce::Rectangle<float>(riser.getX() - len, y - 0.5f, len, 1.0f));
        g.fillRect(juce::Rectangle<float>(riser.getRight(), y - 0.5f, len, 1.0f));
        if (i % 24 == 0)
            g.drawText(juce::String(i), juce::Rectangle<float>(riser.getX() - 22.0f, y - 5.0f, 16.0f, 10.0f),
                       juce::Justification::centredRight, false);
    }
    draw::cornerMarks(g, riser);
    // audience / drummer orientation
    g.setColour(p.textFaint);
    g.drawText("FOH", juce::Rectangle<float>(riser.getCentreX() - 30.0f, riser.getY() + 4.0f, 60.0f, 10.0f),
               juce::Justification::centred, false);

    // drummer: throne (dashed), shoulders and head, very quiet
    const auto throne = toPx(kDrummer);
    g.setColour(p.textFaint);
    draw::dashedEllipse(g, juce::Rectangle<float>(14.0f * ppi_, 14.0f * ppi_).withCentre(throne), 1.0f, 3.0f, 3.0f);
    juce::Path shoulders;
    shoulders.addCentredArc(throne.x, throne.y + 1.0f * ppi_, 8.5f * ppi_, 4.0f * ppi_, 0.0f, -1.25f, 1.25f, true);
    g.strokePath(shoulders, juce::PathStrokeType(1.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.drawEllipse(juce::Rectangle<float>(4.6f * ppi_, 5.4f * ppi_).withCentre(throne.translated(0.0f, 1.2f * ppi_)), 1.0f);
    g.setFont(Theme::get().mono(8.5f));
    g.drawText("THRONE", juce::Rectangle<float>(throne.x - 30.0f, throne.y + 7.5f * ppi_ + 3.0f, 60.0f, 10.0f),
               juce::Justification::centred, false);

    if (pieces_.empty()) {
        g.setColour(p.textDim);
        g.setFont(Theme::get().mono(13.0f));
        const auto c = frame_.getCentre().translated(0.0f, -6.0f * ppi_);
        g.drawText("NO KIT", juce::Rectangle<float>(c.x - 200.0f, c.y - 18.0f, 400.0f, 18.0f), juce::Justification::centred, false);
        g.drawText("DROP PIECES OR LOAD A KIT", juce::Rectangle<float>(c.x - 200.0f, c.y + 2.0f, 400.0f, 18.0f),
                   juce::Justification::centred, false);
    }
}

void KitStage::paintPieces(juce::Graphics& g) {
    const auto& p = pal();
    const bool scanning = host_.processor().scanning();
    const double now = juce::Time::getMillisecondCounterHiRes();
    for (const auto& v : pieces_) {
        const auto c = toPx(piecePos(v));
        const bool sel = v.slot == selected_;
        const bool hover = v.slot == hoverSlot_ || v.slot == dropTarget_;
        if (v.missing) {
            g.setColour(p.textFaint);
            const auto b = pieceBounds(v.kind, c, v.diameter, ppi_);
            if (v.kind == PieceKind::Kick || v.kind == PieceKind::Perc) draw::dashedRect(g, b, 3.0f, 3.0f);
            else draw::dashedEllipse(g, b, 1.0f, 3.0f, 3.0f);
        } else {
            const float h = motion::follow(*this, "ph" + juce::String(v.slot), hover ? 1.0f : 0.0f, motion::quick);
            const PieceInk ink{p.text, p.textFaint.interpolatedWith(p.textDim, h), p.panel};
            drawPiece(g, v.kind, c, v.diameter, ppi_, ink);
        }
        auto bounds = pieceBounds(v.kind, c, v.diameter, ppi_);
        // hit flash: accent ring that fades in 380 ms
        if (auto it = flashes_.find(v.slot); it != flashes_.end()) {
            const float age = float(now - it->second.second) / 380.0f;
            if (age < 1.0f) {
                g.setColour(p.accent.withAlpha(juce::jlimit(0.0f, 1.0f, (0.35f + 0.65f * it->second.first) * (1.0f - age))));
                const auto ring = bounds.expanded(2.0f + 3.0f * age);
                if (v.kind == PieceKind::Kick || v.kind == PieceKind::Perc) g.drawRect(ring, 2.0f);
                else g.drawEllipse(ring, 2.0f);
            }
        }
        if (sel) {
            g.setColour(p.accent);
            const auto ring = bounds.expanded(5.0f);
            if (v.kind == PieceKind::Kick || v.kind == PieceKind::Perc) draw::dashedRect(g, ring, 4.0f, 3.0f);
            else draw::dashedEllipse(g, ring, 1.0f, 4.0f, 3.0f);
            g.setColour(p.text);
            draw::reticle(g, ring.expanded(5.0f));
        }
        // label under the piece: "03 TOM", the name while it is under the hand
        auto label = juce::Rectangle<float>(c.x - 60.0f, bounds.getBottom() + 3.0f, 120.0f, 10.0f);
        g.setFont(Theme::get().mono(8.5f));
        g.setColour(v.missing ? p.textFaint : p.textDim);
        if (v.kind == PieceKind::Kick) // the pedal sits under the kick: label goes beside the shell
            label = juce::Rectangle<float>(bounds.getRight() + 4.0f, bounds.getBottom() - 10.0f, 80.0f, 10.0f);
        g.drawText(v.code, label, v.kind == PieceKind::Kick ? juce::Justification::centredLeft : juce::Justification::centred,
                   false);
        if ((sel || hover) && !v.missing) {
            g.setColour(p.text);
            g.setFont(Theme::get().mono(10.0f));
            g.drawText(v.name, label.translated(0.0f, 11.0f).expanded(40.0f, 1.0f),
                       v.kind == PieceKind::Kick ? juce::Justification::centredLeft : juce::Justification::centred, false);
        }
        if (v.missing && !scanning) {
            const auto t = juce::Rectangle<int>(56, 13).withCentre(c.toInt());
            g.setColour(p.panel);
            g.fillRect(t);
            draw::tag(g, t, "Missing", p.accent);
        }
    }
}

void KitStage::paintMics(juce::Graphics& g) {
    const auto& p = pal();
    const auto& mixer = host_.processor().mixer();
    const auto& held = host_.cableKey();
    for (const auto& m : mics_) {
        P c;
        micPoint(m.key, c);
        const auto jack = mixer.jackOf(m.key);
        const bool isHeld = m.key == held;
        const bool hover = m.key == hoverMic_;
        const float h = motion::follow(*this, "mh" + juce::String(m.key), hover || isHeld ? 1.0f : 0.0f, motion::quick);
        auto ink = (jack.valid() ? p.text : p.textDim).interpolatedWith(p.accent, isHeld ? 1.0f : h);
        if (m.hidden) ink = ink.withMultipliedAlpha(0.6f);
        if (m.kind == MicKind::Bleed) drawBleedSource(g, c, ink, p.panel);
        else drawMic(g, c, m.angle, m.kind == MicKind::Ambient, ink, p.panel);

        // badge: mic code + jack ("IN 3L"), outward from the source
        g.setFont(Theme::get().mono(8.5f));
        const auto code = m.kind == MicKind::Direct ? m.code : m.caption;
        const auto jackText = jack.valid() ? jackCode(jack) : dashText();
        const int cw = juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), code);
        const int jw = juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), jackText);
        const int w = cw + jw + 13, bh = 13;
        P at;
        if (m.kind == MicKind::Direct) {
            const auto o = m.out;
            at = c + o * 15.0f + P(o.x >= 0.0f ? float(w) * 0.5f - 4.0f : -float(w) * 0.5f + 4.0f, 0.0f);
        } else {
            at = c.translated(0.0f, 16.0f + 14.0f * float(m.badgeRow));
        }
        const auto box = juce::Rectangle<float>(float(w), float(bh)).withCentre(at).toNearestInt();
        g.setColour(p.panel.withAlpha(0.92f));
        g.fillRect(box);
        if (jack.valid()) {
            g.setColour(isHeld ? p.accent : p.ruleStrong);
            g.drawRect(box, 1);
        }
        auto inner = box.reduced(4, 0);
        g.setColour(p.textDim);
        g.drawText(code, inner.removeFromLeft(cw), juce::Justification::centredLeft, false);
        inner.removeFromLeft(5);
        g.setColour(jack.valid() ? (isHeld ? p.accent : p.text) : p.textFaint);
        g.drawText(jackText, inner, juce::Justification::centredLeft, false);
    }
}

void KitStage::paintInspector(juce::Graphics& g) {
    const PieceView* v = nullptr;
    for (const auto& pv : pieces_)
        if (pv.slot == selected_) v = &pv;
    if (v == nullptr) return;
    const auto& p = pal();
    auto& proc = host_.processor();
    const auto lookup = proc.pieces();
    const auto* piece = v->slot < int(lookup.size()) ? lookup[size_t(v->slot)] : nullptr;
    juce::StringArray labels{"PIECE", "SLOT", "SIZE", "NOTE", "LIB"};
    juce::StringArray values{v->name, twoDigits(v->slot + 1) + " " + bfdmodel::pieceKindCode(v->kind),
                             piece && !piece->dimensions.empty() ? juce::String(piece->dimensions)
                                                                 : juce::String(juce::roundToInt(v->diameter)) + "\"",
                             noteName(proc.noteForSlot(v->slot)),
                             piece ? juce::String(piece->libraryLong.empty() ? piece->library : piece->libraryLong)
                                   : dashText()};
    const int w = 230, rowH = 15, h = labels.size() * rowH + 12;
    auto box = juce::Rectangle<int>(juce::roundToInt(origin_.x + kHalfW * ppi_) - 10 - w,
                                    juce::roundToInt(origin_.y + kHalfH * ppi_) - 10 - h, w, h);
    g.setColour(p.panel);
    g.fillRect(box);
    g.setColour(p.ruleStrong);
    g.drawRect(box, 1);
    auto r = box.reduced(10, 6);
    for (int i = 0; i < labels.size(); ++i) {
        auto line = r.removeFromTop(rowH);
        g.setFont(Theme::get().mono(9.5f));
        g.setColour(p.textDim);
        g.drawText(labels[i], line.removeFromLeft(44), juce::Justification::centredLeft, false);
        g.setFont(Theme::get().mono(10.5f));
        g.setColour(p.text);
        g.drawText(values[i], line, juce::Justification::centredLeft, true);
    }
}

void KitStage::paintDropGhost(juce::Graphics& g) {
    if (!dropActive_) return;
    const auto& p = pal();
    g.setColour(p.accent);
    if (dropTarget_ >= 0) {
        for (const auto& v : pieces_)
            if (v.slot == dropTarget_) {
                const auto b = pieceBounds(v.kind, toPx(v.pos), v.diameter, ppi_).expanded(6.0f);
                draw::reticle(g, b);
                g.setFont(Theme::get().mono(9.5f));
                g.drawText("REPLACE " + v.code, juce::Rectangle<float>(b.getCentreX() - 80.0f, b.getY() - 14.0f, 160.0f, 11.0f),
                           juce::Justification::centred, false);
            }
        return;
    }
    const auto r = juce::Rectangle<float>(12.0f * ppi_, 12.0f * ppi_).withCentre(dropPx_);
    draw::dashedEllipse(g, r, 1.0f, 4.0f, 3.0f);
    draw::reticle(g, r.expanded(6.0f));
    g.setFont(Theme::get().mono(9.5f));
    const auto in = toIn(dropPx_);
    g.drawText(juce::String(juce::roundToInt(in.x)) + ", " + juce::String(juce::roundToInt(in.y)),
               juce::Rectangle<float>(r.getCentreX() - 60.0f, r.getBottom() + 8.0f, 120.0f, 11.0f),
               juce::Justification::centred, false);
}

//==============================================================================
// interaction

void KitStage::hit(int slot, float velocity) {
    flashes_[slot] = {velocity, juce::Time::getMillisecondCounterHiRes()};
    if (!isTimerRunning()) startTimerHz(60);
}

void KitStage::timerCallback() {
    const double now = juce::Time::getMillisecondCounterHiRes();
    for (auto it = flashes_.begin(); it != flashes_.end();)
        it = now - it->second.second > 400.0 ? flashes_.erase(it) : std::next(it);
    repaint();
    if (flashes_.empty()) stopTimer();
}

void KitStage::mouseMove(const juce::MouseEvent& e) {
    const auto* mic = micAt(e.position);
    const auto* piece = mic ? nullptr : pieceAt(e.position);
    const std::string micKey = mic ? mic->key : std::string();
    const int slot = piece ? piece->slot : -1;
    if (micKey == hoverMic_ && slot == hoverSlot_) return;
    hoverMic_ = micKey;
    hoverSlot_ = slot;
    setMouseCursor(mic ? juce::MouseCursor::DraggingHandCursor
                       : piece ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    auto& proc = host_.processor();
    if (mic) {
        const auto jack = proc.mixer().jackOf(mic->key);
        host_.setHint(mic->caption + (mic->kind == MicKind::Direct && mic->slot >= 0 ? "  " + twoDigits(mic->slot + 1) : "") +
                      arrowText() + (jack.valid() ? "CH " + jackCode(jack) : juce::String("not patched")));
    } else if (piece) {
        host_.setHint(piece->code + "  " + piece->name + "  NOTE " + noteName(proc.noteForSlot(piece->slot)));
    } else {
        host_.setHint({});
    }
    repaint();
}

void KitStage::mouseExit(const juce::MouseEvent&) {
    if (hoverMic_.empty() && hoverSlot_ < 0) return;
    hoverMic_.clear();
    hoverSlot_ = -1;
    host_.setHint({});
    repaint();
}

void KitStage::mouseDown(const juce::MouseEvent& e) {
    gesture_ = Gesture::None;
    if (const auto* mic = micAt(e.position); mic && !e.mods.isPopupMenu()) {
        gesture_ = Gesture::Cable;
        host_.cableBegin(mic->key, *this, e.position);
        repaint();
        return;
    }
    const auto* piece = pieceAt(e.position);
    if (!piece) {
        selected_ = -1;
        repaint();
        return;
    }
    selected_ = piece->slot;
    if (e.mods.isPopupMenu()) {
        showMenu(*piece);
        repaint();
        return;
    }
    gesture_ = Gesture::Piece;
    dragSlot_ = piece->slot;
    dragMoved_ = false;
    dragOffsetIn_ = piece->pos - toIn(e.position);
    dragPosIn_ = piece->pos;
    if (!piece->missing) {
        // velocity from where the stick lands: centre loud, edge softer
        const float d = toPx(piece->pos).getDistanceFrom(e.position) / std::max(1.0f, piece->diameter * 0.5f * ppi_);
        host_.processor().audition(piece->slot, juce::jlimit(0.35f, 0.95f, 0.95f - 0.35f * d));
    }
    repaint();
}

void KitStage::mouseDrag(const juce::MouseEvent& e) {
    if (gesture_ == Gesture::Cable) {
        host_.cableMove(*this, e.position);
        return;
    }
    if (gesture_ != Gesture::Piece) return;
    if (!dragMoved_ && e.getDistanceFromDragStart() < 4) return;
    dragMoved_ = true;
    auto in = toIn(e.position) + dragOffsetIn_;
    in.x = juce::jlimit(-kHalfW + 2.0f, kHalfW - 2.0f, in.x);
    in.y = juce::jlimit(-kHalfH + 2.0f, kHalfH - 2.0f, in.y);
    dragPosIn_ = in;
    setMouseCursor(juce::MouseCursor::DraggingHandCursor);
    host_.setHint(juce::String(juce::roundToInt(in.x)) + ", " + juce::String(juce::roundToInt(in.y)) + " IN");
    repaint();
}

void KitStage::mouseUp(const juce::MouseEvent& e) {
    const auto g = std::exchange(gesture_, Gesture::None);
    if (g == Gesture::Cable) {
        host_.cableEnd(*this, e.position);
        repaint();
        return;
    }
    if (g == Gesture::Piece && dragMoved_) {
        const int slot = dragSlot_;
        const auto pos = dragPosIn_;
        dragMoved_ = false;
        dragSlot_ = -1;
        commitPosition(slot, pos);
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }
}

void KitStage::commitPosition(int slot, P in) {
    host_.processor().mutateKit([&](bfdmodel::Kit& kit) {
        if (slot < 0 || slot >= int(kit.pieces.size())) return;
        kit.pieces[size_t(slot)].stageX = juce::jlimit(0.0f, 1.0f, in.x / (2.0f * kHalfW) + 0.5f);
        kit.pieces[size_t(slot)].stageY = juce::jlimit(0.0f, 1.0f, in.y / (2.0f * kHalfH) + 0.5f);
    });
    refresh();
}

void KitStage::showMenu(const PieceView& v) {
    juce::PopupMenu m;
    m.addSectionHeader(v.code + "  " + v.name);
    m.addItem(1, "Audition", !v.missing);
    m.addItem(2, "Reset position");
    m.addSeparator();
    m.addItem(3, "Remove from kit");
    const int slot = v.slot;
    juce::Component::SafePointer<KitStage> safe(this);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(), [safe, slot](int r) {
        if (!safe) return;
        auto& proc = safe->host_.processor();
        if (r == 1) proc.audition(slot, 0.8f);
        if (r == 2) {
            proc.mutateKit([&](bfdmodel::Kit& kit) {
                if (slot < int(kit.pieces.size())) kit.pieces[size_t(slot)].stageX = kit.pieces[size_t(slot)].stageY = -1.0f;
            });
            safe->refresh();
        }
        if (r == 3) {
            proc.removePiece(slot);
            safe->selected_ = -1;
        }
    });
}

bool KitStage::isInterestedInDragSource(const SourceDetails& d) {
    return d.description.toString().startsWith("piece:");
}

void KitStage::itemDragEnter(const SourceDetails& d) { itemDragMove(d); }

void KitStage::itemDragMove(const SourceDetails& d) {
    dropActive_ = true;
    dropPx_ = d.localPosition.toFloat();
    const auto* target = pieceAt(dropPx_);
    dropTarget_ = target ? target->slot : -1;
    host_.setHint(target ? "Replace " + target->code + "  " + target->name : juce::String("Place here"));
    repaint();
}

void KitStage::itemDragExit(const SourceDetails&) {
    dropActive_ = false;
    dropTarget_ = -1;
    host_.setHint({});
    repaint();
}

void KitStage::itemDropped(const SourceDetails& d) {
    const auto ref = d.description.toString().fromFirstOccurrenceOf("piece:", false, false).toStdString();
    const auto px = d.localPosition.toFloat();
    const auto* target = pieceAt(px);
    auto& proc = host_.processor();
    int placed;
    if (target) {
        const auto pos = target->pos; // the replacement keeps the spot
        placed = proc.placePiece(target->slot, ref, pos.x / (2.0f * kHalfW) + 0.5f, pos.y / (2.0f * kHalfH) + 0.5f);
    } else {
        auto in = toIn(px);
        in.x = juce::jlimit(-kHalfW + 2.0f, kHalfW - 2.0f, in.x);
        in.y = juce::jlimit(-kHalfH + 2.0f, kHalfH - 2.0f, in.y);
        placed = proc.placePiece(-1, ref, in.x / (2.0f * kHalfW) + 0.5f, in.y / (2.0f * kHalfH) + 0.5f);
    }
    dropActive_ = false;
    dropTarget_ = -1;
    if (placed >= 0) selected_ = placed;
    refresh();
}

} // namespace openbfd::ui
