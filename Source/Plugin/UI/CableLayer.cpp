#include "CableLayer.h"

#include "Theme.h"
#include "Widgets.h"

namespace openbfd::ui {

namespace {
// The timer runs at 60 Hz with two substeps: all rates below are per substep.
constexpr int kSubsteps = 2;
constexpr float kGravity = 0.42f;   // px per substep², +y is down
constexpr float kDamping = 0.988f;  // velocity kept per substep (air)
constexpr float kFloorDrag = 0.55f; // velocity kept along the floor once a cable touches it
constexpr int kIterations = 28;
constexpr float kStiffness = 0.06f; // a little bending stiffness: rounded, never kinked
constexpr float kSeatRate = 0.16f;  // plug slides home in ~100 ms

// Slack a cable keeps between its ends: long runs hang in deep loops.
float restLength(juce::Point<float> a, juce::Point<float> b) { return a.getDistanceFrom(b) * 1.12f + 42.0f; }

// Where the cable leaves the plug. A plug at a jack points +x (into the side
// face) and the cable leaves the back of its strain relief.
juce::Point<float> cableEnd(juce::Point<float> plug, bool onJack, float seat) {
    if (!onJack) return plug; // free plug: the cable runs under it to the tip
    return plug - juce::Point<float>(kPlugLength - seat * kPlugSeated, 0.0f);
}
} // namespace

CableLayer::CableLayer() {
    setInterceptsMouseClicks(false, false);
    setWantsKeyboardFocus(false);
}

CableLayer::Rope CableLayer::makeRope(P anchor, P plug, bool lying) {
    Rope r;
    r.anchor = anchor;
    r.plug = plug;
    if (!lying) {
        // fresh pull: the cable starts coiled at the mic
        r.pts = {anchor, plug};
        r.payout = anchor.getDistanceFrom(plug);
    } else {
        // already plugged: start from the parabola its slack hangs in
        const auto end = cableEnd(plug, true, 1.0f);
        const float span = anchor.getDistanceFrom(end);
        const float len = restLength(anchor, end);
        const float sag = std::sqrt(3.0f * span * std::max(0.0f, len - span) / 8.0f);
        const int n = std::max(2, int(std::ceil(len / kSeg)) + 1);
        for (int i = 0; i < n; ++i) {
            const float t = float(i) / float(n - 1);
            r.pts.push_back(anchor + (end - anchor) * t + P(0.0f, 4.0f * sag * t * (1.0f - t)));
        }
        r.payout = len - kSeg * float(n - 2);
        r.seat = 1.0f;
    }
    r.prev = r.pts;
    return r;
}

void CableLayer::step(Rope& r, float desired, float floor) {
    auto& pts = r.pts;
    r.seat = r.onJack && !r.held ? std::min(1.0f, r.seat + kSeatRate) : std::max(0.0f, r.seat - kSeatRate);
    const P end = cableEnd(r.plug, r.onJack, r.seat);

    // pay out or reel in at the mic end
    float length = r.payout + kSeg * float(pts.size() - 2);
    if (length < desired) {
        r.payout += std::min(12.0f, desired - length);
        while (r.payout > kSeg * 1.5f) {
            // new cable leaves the mic along the line to the next point
            const auto dir = pts[1] - pts[0];
            const float len = std::max(0.001f, dir.getDistanceFromOrigin());
            const auto at = pts[0] + dir * std::min(1.0f, (r.payout - kSeg) / len);
            pts.insert(pts.begin() + 1, at);
            r.prev.insert(r.prev.begin() + 1, at);
            r.payout -= kSeg;
        }
    } else if (length > desired * 1.08f + 10.0f) {
        r.payout -= std::min(6.0f, length - desired);
        while (r.payout < kSeg * 0.5f && pts.size() > 2) {
            pts.erase(pts.begin() + 1);
            r.prev.erase(r.prev.begin() + 1);
            r.payout += kSeg;
        }
        r.payout = std::max(1.0f, r.payout);
    }
    const size_t n = pts.size();

    // verlet: carry velocity, fall under gravity
    for (size_t i = 1; i + 1 < n; ++i) {
        auto v = (pts[i] - r.prev[i]) * kDamping;
        if (pts[i].y >= floor - 0.5f) v.x *= kFloorDrag;
        r.prev[i] = pts[i];
        pts[i] += v + P(0.0f, kGravity);
    }
    pts.front() = r.prev.front() = r.anchor;
    pts.back() = r.prev.back() = end;

    // a plug at a side jack leaves it horizontally, towards the stage
    const bool lead = r.onJack && n > 3;
    const P leadPt = end - P(kSeg, 0.0f);
    if (lead) pts[n - 2] = r.prev[n - 2] = leadPt;

    // bending stiffness, once per substep: corners round off, sag stays
    for (size_t i = 1; i + 1 < n; ++i) pts[i] += ((pts[i - 1] + pts[i + 1]) * 0.5f - pts[i]) * kStiffness;

    // inextensible rope: alternate sweep direction so neither end is favoured
    const auto relax = [&](size_t i) {
        const float rest = i == 0 ? r.payout : kSeg;
        const auto d = pts[i + 1] - pts[i];
        const float dist = std::max(0.001f, d.getDistanceFromOrigin());
        const auto corr = d * ((dist - rest) / dist);
        const bool pinA = i == 0, pinB = i + 1 >= n - (lead ? 2 : 1);
        if (pinA && pinB) return;
        if (pinA) pts[i + 1] -= corr;
        else if (pinB) pts[i] += corr;
        else {
            pts[i] += corr * 0.5f;
            pts[i + 1] -= corr * 0.5f;
        }
    };
    for (int it = 0; it < kIterations; ++it) {
        if (it % 2 == 0)
            for (size_t i = 0; i + 1 < n; ++i) relax(i);
        else
            for (size_t i = n - 1; i-- > 0;) relax(i);
        for (size_t i = 1; i + 1 < n; ++i) pts[i].y = std::min(pts[i].y, floor);
    }
    if (lead) pts[n - 2] = leadPt;
}

bool CableLayer::busy() const {
    return held_.has_value() || !retracting_.empty() || (reveal_ > 0.0f && !plugged_.empty());
}

void CableLayer::setPlugged(const std::vector<Ends>& ends) {
    std::map<std::string, Rope> next;
    for (const auto& e : ends) {
        if (e.key == heldKey_ && held_) continue;
        auto it = plugged_.find(e.key);
        if (it != plugged_.end()) {
            it->second.anchor = e.anchor;
            it->second.plug = e.jack;
            it->second.shown = e.shown;
            next.emplace(e.key, std::move(it->second));
            plugged_.erase(it);
        } else {
            auto rope = makeRope(e.anchor, e.jack, true);
            rope.onJack = true;
            rope.shown = e.shown;
            next.emplace(e.key, std::move(rope));
        }
    }
    // displaced or unplugged cables reel back into their mics
    for (auto& [key, rope] : plugged_) {
        rope.retract = 0.0f;
        rope.onJack = false;
        rope.shown = true;
        retracting_.push_back(std::move(rope));
    }
    plugged_ = std::move(next);
    if (busy() && !isTimerRunning()) startTimerHz(60);
}

void CableLayer::setReveal(float amount) {
    if (std::abs(amount - reveal_) < 0.001f) return;
    reveal_ = amount;
    if (busy() && !isTimerRunning()) startTimerHz(60);
    repaint();
}

void CableLayer::beginDrag(const std::string& key, P anchor, P plug) {
    auto it = plugged_.find(key);
    if (it != plugged_.end()) {
        // pull the cable that is already hanging there out of its jack
        held_ = std::move(it->second);
        plugged_.erase(it);
    } else {
        held_ = makeRope(anchor, plug, false);
    }
    heldKey_ = key;
    held_->shown = true;
    held_->held = true;
    held_->anchor = anchor;
    held_->plug = plug;
    held_->onJack = false;
    held_->seat = 0.0f;
    startTimerHz(60);
}

void CableLayer::dragTo(P anchor, P plug, bool onJack) {
    if (!held_) return;
    held_->anchor = anchor;
    held_->plug = plug;
    held_->onJack = onJack;
}

void CableLayer::endDrag(bool plugged) {
    if (!held_) return;
    held_->held = false;
    if (plugged) {
        held_->onJack = true;
        plugged_[heldKey_] = std::move(*held_);
    } else {
        held_->retract = 0.0f;
        held_->onJack = false;
        retracting_.push_back(std::move(*held_));
    }
    held_.reset();
    heldKey_.clear();
}

void CableLayer::timerCallback() {
    const float floor = float(getHeight()) - 3.0f;
    for (int sub = 0; sub < kSubsteps; ++sub) {
        if (held_) step(*held_, restLength(held_->anchor, held_->plug), floor);
        for (auto& [key, rope] : plugged_)
            step(rope, restLength(rope.anchor, cableEnd(rope.plug, true, rope.seat)), floor);
        for (auto& rope : retracting_) {
            // the plug is dragged back to the mic, which reels the cable in
            rope.retract += 1.0f / 40.0f;
            rope.plug = rope.plug + (rope.anchor - rope.plug) * 0.09f;
            step(rope, rope.anchor.getDistanceFrom(rope.plug) * 1.02f + 2.0f, floor);
        }
    }
    retracting_.erase(std::remove_if(retracting_.begin(), retracting_.end(),
                                     [](const Rope& r) { return r.anchor.getDistanceFrom(r.plug) < 6.0f || r.retract > 1.5f; }),
                      retracting_.end());
    repaint();
    if (!busy()) stopTimer();
}

void CableLayer::drawRope(juce::Graphics& g, const Rope& r, juce::Colour ink, float alpha, bool held) const {
    if (r.pts.size() < 2 || alpha <= 0.0f || !r.shown) return;
    const auto& pts = r.pts;
    juce::Path path;
    path.startNewSubPath(pts[0]);
    for (size_t i = 1; i + 1 < pts.size(); ++i) {
        const auto mid = (pts[i] + pts[i + 1]) * 0.5f;
        path.quadraticTo(pts[i], mid);
    }
    path.lineTo(pts.back());
    const auto& p = pal();
    // a gap in the sheet colour under the cable keeps it readable over line work
    g.setColour(p.panel.withAlpha(alpha * 0.9f));
    g.strokePath(path, juce::PathStrokeType(held ? 4.0f : 3.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour(ink.withMultipliedAlpha(alpha));
    g.strokePath(path, juce::PathStrokeType(held ? 2.0f : 1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    // plug: at a jack it points into the side face and sinks in as it seats;
    // anywhere else it hangs along the last stretch of cable
    float angle = -juce::MathConstants<float>::halfPi;
    if (!r.onJack) {
        const auto a = pts[pts.size() >= 3 ? pts.size() - 3 : 0];
        const auto d = r.plug - a;
        angle = std::atan2(-d.x, d.y);
    }
    drawPlug(g, r.plug, angle, ink.withMultipliedAlpha(alpha), p.panel.withMultipliedAlpha(alpha),
             r.onJack ? r.seat * kPlugSeated : 0.0f);
}

void CableLayer::paint(juce::Graphics& g) {
    const auto& p = pal();
    if (reveal_ > 0.0f)
        for (const auto& [key, rope] : plugged_) // the cable in the hand stands out
            drawRope(g, rope, p.cable, reveal_ * (held_ ? 0.38f : 1.0f), false);
    for (const auto& rope : retracting_)
        drawRope(g, rope, p.textDim, juce::jlimit(0.0f, 1.0f, 1.2f - rope.retract), false);
    if (held_) drawRope(g, *held_, held_->onJack ? p.accent : p.accent.withAlpha(0.85f), 1.0f, true);
}

} // namespace openbfd::ui
