#include "Motion.h"

namespace openbfd::ui::motion {

namespace {

// Repaints components whose values are still moving, once per frame.
class Repainter final : private juce::Timer {
public:
    static Repainter& get() {
        static Repainter instance;
        return instance;
    }

    void add(juce::Component& c, juce::Rectangle<int> area) {
        for (auto& [comp, a] : pending)
            if (comp.getComponent() == &c) {
                a = a.isEmpty() || area.isEmpty() ? juce::Rectangle<int>() : a.getUnion(area);
                return;
            }
        pending.emplace_back(juce::Component::SafePointer<juce::Component>(&c), area);
        if (!isTimerRunning()) startTimerHz(60);
    }

private:
    void timerCallback() override {
        auto due = std::exchange(pending, {});
        for (auto& [comp, area] : due)
            if (auto* c = comp.getComponent()) {
                if (area.isEmpty()) c->repaint();
                else c->repaint(area);
            }
        if (pending.empty()) stopTimer();
    }

    std::vector<std::pair<juce::Component::SafePointer<juce::Component>, juce::Rectangle<int>>> pending;
};

bool arrived(float value, float target) {
    const float tolerance = std::abs(target) > 2.0f || std::abs(value) > 2.0f ? 0.3f : 0.003f;
    return std::abs(target - value) <= tolerance;
}

} // namespace

float step(float value, float target, double dtMs, float ms) {
    const float tau = std::max(1.0f, ms / 4.0f); // ~98% of the way after `ms`
    return value + (target - value) * (1.0f - std::exp(-float(dtMs) / tau));
}

float current(juce::Component& c, const juce::String& key, float fallback) {
    const auto& v = c.getProperties()["~" + key];
    return v.isVoid() ? fallback : float(double(v));
}

float follow(juce::Component& c, const juce::String& key, float target, float ms, juce::Rectangle<int> area) {
    auto& props = c.getProperties();
    const juce::Identifier valueId("~" + key), timeId("~" + key + "@"), restId("~" + key + "!");
    const auto now = juce::Time::getMillisecondCounterHiRes();
    if (!props.contains(valueId)) {
        props.set(valueId, target);
        props.set(timeId, now);
        props.set(restId, true);
        return target;
    }
    auto value = float(double(props[valueId]));
    if (value == target) {
        props.set(timeId, now);
        props.set(restId, true);
        return value;
    }
    // A value at rest starts moving one frame from now; a moving one continues.
    const double dt = bool(props[restId]) ? 1000.0 / 60.0 : juce::jlimit(0.0, 50.0, now - double(props[timeId]));
    value = step(value, target, dt, ms);
    if (arrived(value, target)) value = target;
    else Repainter::get().add(c, area);
    props.set(valueId, value);
    props.set(timeId, now);
    props.set(restId, value == target);
    return value;
}

void reset(juce::Component& c, const juce::String& key, float value) {
    c.getProperties().set("~" + key, value);
    c.getProperties().set("~" + key + "!", true);
}

} // namespace openbfd::ui::motion
