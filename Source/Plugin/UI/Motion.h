// Motion: values glide to their targets instead of jumping (instrument-ui).
// follow() is called from paint(): it returns the value to draw this frame and
// keeps the component repainting until it has arrived. State lives in the
// component's properties. Exponential decay, tau = ms / 4, no overshoot.
#pragma once

#include <JuceHeader.h>

namespace openbfd::ui::motion {

constexpr float quick = 110.0f, normal = 170.0f, slow = 240.0f;

float follow(juce::Component&, const juce::String& key, float target, float ms = normal,
             juce::Rectangle<int> area = {});
float current(juce::Component&, const juce::String& key, float fallback = 0.0f);
void reset(juce::Component&, const juce::String& key, float value);

// One exponential step of `ms`-style decay over `dtMs`.
float step(float value, float target, double dtMs, float ms);

} // namespace openbfd::ui::motion
