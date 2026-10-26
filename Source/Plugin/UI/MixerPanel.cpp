#include "MixerPanel.h"

#include "../PluginProcessor.h"
#include "Motion.h"
#include "Theme.h"

namespace openbfd::ui {

using bfdengine::Jack;
using bfdengine::MicKind;
using bfdengine::StripMode;

namespace {


float toDb(float linear) { return linear > 1.0e-6f ? 20.0f * std::log10(linear) : -120.0f; }

juce::String linearDb(double v) { return formatDb(toDb(float(v))); }
juce::String faderText(double v) { return formatDb(bfdengine::faderDb(float(v))); }

juce::String panText(double v, bool balance) {
    const int amount = juce::roundToInt((v - 0.5) * 200.0);
    if (amount == 0) return "C";
    return (amount < 0 ? "L" : "R") + juce::String(std::abs(amount)) + (balance ? " BAL" : "");
}

const char* modeTag(StripMode m) {
    switch (m) {
        case StripMode::MonoL:
        case StripMode::MonoR: return "MONO";
        case StripMode::Stereo: return "ST";
        case StripMode::MidSide: return "M-S";
        case StripMode::Empty: break;
    }
    return "";
}

// Controls sit in a 42 px band centred in the row.
constexpr int kPad = (ChannelStrip::kMainH - 42) / 2;

void hatchCell(juce::Graphics& g, juce::Rectangle<int> r) {
    draw::hatch(g, r.reduced(1), pal().rule, 6);
}

} // namespace

juce::String formatDb(float db) {
    if (db <= -119.0f) return "-INF";
    const float r = std::round(db * 10.0f) / 10.0f;
    return (r > 0.0f ? "+" : "") + juce::String(r, 1);
}

//==============================================================================
ChannelStrip::ChannelStrip(UiHost& host, int s) : strip(s), host_(host) {
    auto hint = [this](const juce::String& h) { host_.setHint(h); };
    for (auto* k : {&kickBleed_, &snareBleed_, &send_, &ret_, &in_, &mKick_, &mSnare_, &mOther_, &pan_}) {
        k->onHint = hint;
        addChildComponent(*k);
    }
    for (auto* k : {&kickBleed_, &snareBleed_, &send_, &ret_}) k->format = linearDb;
    for (auto* k : {&in_, &mKick_, &mSnare_, &mOther_}) k->format = faderText;

    kickBleed_.onValueChange = [this] { editPiece([v = float(kickBleed_.getValue())](auto& p) { p.kickBleedLvl = v; }); };
    snareBleed_.onValueChange = [this] { editPiece([v = float(snareBleed_.getValue())](auto& p) { p.snareBleedLvl = v; }); };
    send_.onValueChange = [this] { editPiece([v = float(send_.getValue())](auto& p) { p.bleedSend = v; }); };
    ret_.onValueChange = [this] { editPiece([v = float(ret_.getValue())](auto& p) { p.bleedReturn = v; }); };
    in_.onValueChange = [this] { editPiece([v = float(in_.getValue())](auto& p) { p.bleedInputTrim = v; }); };
    mKick_.onValueChange = [this] { editMixer([v = float(mKick_.getValue())](auto& m) { m.masterKickBleed = v; }); };
    mSnare_.onValueChange = [this] { editMixer([v = float(mSnare_.getValue())](auto& m) { m.masterSnareBleed = v; }); };
    mOther_.onValueChange = [this] { editMixer([v = float(mOther_.getValue())](auto& m) { m.masterOtherBleed = v; }); };

    const juce::StringArray dstHints{"Off", "Into this channel (Dir)", "Into the kick / snare mics (On)"};
    kickDst_.hints = snareDst_.hints = dstHints;
    for (auto* sgm : {&kickDst_, &snareDst_}) {
        sgm->onHint = hint;
        addChildComponent(*sgm);
    }
    kickDst_.onChange = [this](int i) { editPiece([i](auto& p) { p.kickBleedDst = i; }); };
    snareDst_.onChange = [this](int i) { editPiece([i](auto& p) { p.snareBleedDst = i; }); };

    pan_.format = [this](double v) { return panText(v, mode_ == "ST" || mode_ == "M-S"); };
    pan_.onValueChange = [this] {
        const float v = float(pan_.getValue());
        editMixer([this, v](auto& m) { m.strips[size_t(strip)].pan = v; });
    };

    fader_.setSliderStyle(juce::Slider::LinearHorizontal);
    fader_.getProperties().set("fader", true);
    fader_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    fader_.setRange(0.0, 1.0, 0.0);
    fader_.setValue(0.8, juce::dontSendNotification);
    fader_.setDoubleClickReturnValue(true, 0.8);
    fader_.onValueChange = [this] {
        const float v = float(fader_.getValue());
        if (isMaster()) editMixer([v](auto& m) { m.masterGain = v; });
        else editMixer([this, v](auto& m) { m.strips[size_t(strip)].gain = v; });
        host_.setHint((isMaster() ? juce::String("MASTER") : "CH " + juce::String(strip + 1)) + "  " + faderText(v) + " DB");
        repaint(readout_);
    };
    addAndMakeVisible(fader_);

    for (auto* b : {&mute_, &solo_}) {
        b->setClickingTogglesState(true);
        addChildComponent(*b);
    }
    mute_.setTooltip("Mute");
    solo_.setTooltip("Solo");
    mute_.onClick = [this] { editMixer([this, on = mute_.getToggleState()](auto& m) { m.strips[size_t(strip)].mute = on; }); };
    solo_.onClick = [this] { editMixer([this, on = solo_.getToggleState()](auto& m) { m.strips[size_t(strip)].solo = on; }); };
    addAndMakeVisible(meter_);
    sync();
}

void ChannelStrip::editPiece(const std::function<void(bfdmodel::KitPieceState&)>& fn) {
    const int slot = pieceSlot_;
    if (slot < 0) return;
    host_.processor().mutateKit([&](bfdmodel::Kit& kit) {
        if (slot < int(kit.pieces.size())) fn(kit.pieces[size_t(slot)]);
    });
    if (onKitEdited) onKitEdited();
}

void ChannelStrip::editMixer(const std::function<void(bfdengine::MixerState&)>& fn) {
    host_.processor().mutateMixer(fn);
}

void ChannelStrip::sync() {
    auto& proc = host_.processor();
    const auto& mx = proc.mixer();
    auto setKnob = [](Knob& k, float v) {
        if (!k.isMouseButtonDown()) k.setValue(v, juce::dontSendNotification);
    };
    if (isMaster()) {
        mode_ = "ST";
        caption_ = "MAIN";
        sub_ = "MASTER";
        setKnob(mKick_, mx.masterKickBleed);
        setKnob(mSnare_, mx.masterSnareBleed);
        setKnob(mOther_, mx.masterOtherBleed);
        if (!fader_.isMouseButtonDown()) fader_.setValue(mx.masterGain, juce::dontSendNotification);
        updateVisibility();
        repaint();
        return;
    }
    const auto& mics = proc.mics();
    const auto lookup = proc.pieces();
    const auto* keyL = mx.micAt(strip, 0);
    const auto* keyR = mx.micAt(strip, 1);
    const auto* ml = keyL ? findMic(mics, *keyL) : nullptr;
    const auto* mr = keyR ? findMic(mics, *keyR) : nullptr;
    const auto* primary = ml ? ml : mr;
    const auto mode = mx.mode(strip, mics);
    active_ = keyL || keyR;
    micL_ = ml ? micCaption(*ml, nullptr) : keyL ? juce::String(*keyL) : juce::String();
    micR_ = mr ? micCaption(*mr, nullptr) : keyR ? juce::String(*keyR) : juce::String();
    mode_ = modeTag(mode);
    pieceSlot_ = primary && primary->kind == MicKind::Direct ? primary->slot : -1;
    const auto* piece = pieceSlot_ >= 0 && pieceSlot_ < int(lookup.size()) ? lookup[size_t(pieceSlot_)] : nullptr;
    if (!piece) pieceSlot_ = -1;
    const auto kind = piece ? bfdmodel::pieceKind(*piece) : bfdmodel::PieceKind::Perc;
    kickPiece_ = piece && kind == bfdmodel::PieceKind::Kick;
    snarePiece_ = piece && kind == bfdmodel::PieceKind::Snare;
    if (!primary) {
        caption_ = keyL ? juce::String(*keyL) : keyR ? juce::String(*keyR) : dashText();
        sub_ = "MISSING";
    } else if (ml && mr && ml->kind == MicKind::Ambient && mr->kind == MicKind::Ambient && ml->group == mr->group) {
        caption_ = juce::String(ml->group).toUpperCase() + (mode == StripMode::MidSide ? " M/S" : " L/R");
        sub_ = "AMBIENT";
    } else {
        caption_ = micCaption(*primary, piece);
        if (ml && mr) caption_ << " + " << micCaption(*mr, nullptr);
        sub_ = piece ? juce::String(piece->name) : primary->kind == MicKind::Ambient ? juce::String("AMBIENT")
                                                                                   : juce::String("BLEED");
    }

    if (piece && pieceSlot_ < int(proc.kit().pieces.size())) {
        const auto& st = proc.kit().pieces[size_t(pieceSlot_)];
        setKnob(kickBleed_, st.kickBleedLvl);
        setKnob(snareBleed_, st.snareBleedLvl);
        setKnob(send_, st.bleedSend);
        setKnob(ret_, st.bleedReturn);
        setKnob(in_, st.bleedInputTrim);
        kickDst_.setSelected(st.kickBleedDst);
        snareDst_.setSelected(st.snareBleedDst);
    }
    const auto& ss = mx.strips[size_t(strip)];
    pan_.caption = mode == StripMode::Stereo || mode == StripMode::MidSide ? "Balance" : "Pan";
    setKnob(pan_, ss.pan);
    if (!fader_.isMouseButtonDown()) fader_.setValue(ss.gain, juce::dontSendNotification);
    mute_.setToggleState(ss.mute, juce::dontSendNotification);
    solo_.setToggleState(ss.solo, juce::dontSendNotification);
    updateVisibility();
    resized();
    repaint();
}

void ChannelStrip::updateVisibility() {
    // only flips what changes: a control under the mouse stays put
    const bool front = !back_, channel = !isMaster();
    fader_.setVisible(front);
    meter_.setVisible(front);
    pan_.setVisible(front && channel);
    mute_.setVisible(front && channel);
    solo_.setVisible(front && channel);
    for (auto* k : {&mKick_, &mSnare_, &mOther_}) k->setVisible(back_ && !channel);
    const bool bleed = back_ && channel && active_ && pieceSlot_ >= 0;
    kickBleed_.setVisible(bleed && !kickPiece_);
    kickDst_.setVisible(bleed && !kickPiece_);
    snareBleed_.setVisible(bleed && !snarePiece_);
    snareDst_.setVisible(bleed && !snarePiece_);
    send_.setVisible(bleed);
    ret_.setVisible(bleed && (kickPiece_ || snarePiece_));
    in_.setVisible(bleed && (kickPiece_ || snarePiece_));
}

int ChannelStrip::preferredHeight() const {
    // a patched row is the same size on both faces
    return back_ && !active_ && !isMaster() ? kFreeH : kMainH;
}

void ChannelStrip::setBack(bool back) {
    if (back == back_) return;
    back_ = back;
    hotSide_ = hoverSide_ = -1;
    sync();
}

//------------------------------------------------------------------------------
// rear face: side jacks

juce::Point<float> ChannelStrip::socketCentre(int side) const {
    const float h = float(active_ || isMaster() ? kMainH : kFreeH);
    return {float(kSideW) * 0.5f, h * (side == 0 ? 0.31f : 0.71f)};
}

int ChannelStrip::jackAt(juce::Point<float> p) const {
    if (!back_ || isMaster()) return -1;
    for (int side = 0; side < 2; ++side)
        if (socketCentre(side).getDistanceFrom(p) <= 9.0f) return side;
    return -1;
}

void ChannelStrip::setHotSide(int side) {
    if (side == hotSide_) return;
    hotSide_ = side;
    repaint();
}

void ChannelStrip::paintBack(juce::Graphics& g) {
    const auto& p = pal();
    const int h = getHeight();
    g.fillAll(p.panel);
    g.setColour(p.rule);
    g.fillRect(0, h - 1, getWidth(), 1);
    // side face of the row, where the cables go in
    g.setColour(p.panelInset);
    g.fillRect(0, 0, kSideW, h);
    g.setColour(hotSide_ >= 0 ? p.accent : p.ruleStrong);
    g.fillRect(kSideW, 0, 1, h);
    if (hotSide_ >= 0) {
        g.setColour(p.accent);
        g.drawRect(getLocalBounds(), 1);
    }
    auto cellLabel = [&](juce::Rectangle<int> cell, const juce::String& text, bool on) {
        g.setColour(on ? p.textDim : p.textFaint);
        g.setFont(Theme::get().mono(8.5f));
        g.drawText(text, cell.withHeight(14 + kPad).withTrimmedTop(3 + kPad), juce::Justification::centredLeft, false);
    };
    auto hatchCtl = [&](juce::Rectangle<int> cell) { hatchCell(g, cell.withTrimmedTop(16 + kPad).withHeight(24)); };
    const int x = kSideW + 10;
    if (isMaster()) {
        g.setColour(p.text);
        g.setFont(Theme::get().mono(12.5f, true));
        g.drawText("MST", juce::Rectangle<int>(x, 0, 40, h), juce::Justification::centredLeft, false);
        g.setColour(p.textDim);
        g.setFont(Theme::get().mono(9.5f));
        g.drawText("BLEED", names_.withHeight(h), juce::Justification::centredLeft, false);
        cellLabel(kGroup_, "KICK", true);
        cellLabel(sGroup_, "SNARE", true);
        cellLabel(sendCell_, "OTHER", true);
        g.setFont(Theme::get().mono(9.5f));
        g.setColour(p.textDim);
        g.drawText("OUT 1-2", rearOut_.withHeight(h), juce::Justification::centredRight, false);
        return;
    }
    auto& proc = host_.processor();
    const auto& mx = proc.mixer();
    const int mainH = active_ ? kMainH : kFreeH;
    for (int side = 0; side < 2; ++side) {
        const auto c = socketCentre(side);
        const bool plugged = mx.micAt(strip, side) != nullptr;
        drawSocket(g, c, active_ ? 6.0f : 5.0f, plugged, hotSide_ == side || (hoverSide_ == side && plugged));
        // jack name + what is plugged in, on the jack's line
        g.setFont(Theme::get().mono(8.5f));
        g.setColour(p.textDim);
        g.drawText(side == 0 ? "L" : "R", juce::Rectangle<float>(float(names_.getX()), c.y - 6.0f, 10.0f, 12.0f),
                   juce::Justification::centredLeft, false);
        const auto& name = side == 0 ? micL_ : micR_;
        g.setFont(Theme::get().mono(active_ ? 10.0f : 9.0f));
        g.setColour(name.isEmpty() ? p.textFaint : p.text);
        g.drawText(name.isEmpty() ? dashText() : name,
                   juce::Rectangle<float>(float(names_.getX() + 14), c.y - 7.0f, float(names_.getWidth() - 14), 14.0f),
                   juce::Justification::centredLeft, true);
    }
    // number (= output channel) over its mode
    g.setColour(active_ ? p.text : p.textDim);
    g.setFont(Theme::get().mono(active_ ? 12.5f : 11.0f, active_));
    g.drawText(juce::String(strip + 1).paddedLeft('0', 2),
               active_ ? juce::Rectangle<int>(x, 6 + kPad, 24, 15) : juce::Rectangle<int>(x, 0, 24, mainH),
               juce::Justification::centredLeft, false);
    if (mode_.isNotEmpty() && active_) {
        g.setFont(Theme::get().mono(8.5f));
        const int tw = juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), mode_) + 8;
        draw::tag(g, juce::Rectangle<int>(x, 24 + kPad, tw, 12), mode_, p.textDim);
    }
    g.setColour(active_ ? p.textDim : p.textFaint);
    g.setFont(Theme::get().mono(9.5f));
    g.drawText("OUT " + juce::String(strip + 1), rearOut_.withHeight(mainH), juce::Justification::centredRight, false);
    if (!active_) return;
    // bleed settings of the piece this channel carries; hatched where they do not apply
    if (pieceSlot_ < 0) {
        cellLabel(bleedArea_, "NO BLEED", false);
        hatchCtl(bleedArea_);
        return;
    }
    cellLabel(kGroup_, "KICK", kickBleed_.isVisible());
    if (!kickBleed_.isVisible()) hatchCtl(kGroup_);
    cellLabel(sGroup_, "SNARE", snareBleed_.isVisible());
    if (!snareBleed_.isVisible()) hatchCtl(sGroup_);
    cellLabel(sendCell_, "SEND", true);
    cellLabel(retCell_, "RET", ret_.isVisible());
    if (!ret_.isVisible()) hatchCtl(retCell_);
    cellLabel(inCell_, "IN", in_.isVisible());
    if (!in_.isVisible()) hatchCtl(inCell_);
}

void ChannelStrip::mouseMove(const juce::MouseEvent& e) {
    if (!back_) return;
    const int side = jackAt(e.position);
    if (side == hoverSide_) return;
    hoverSide_ = side;
    repaint();
    const auto* key = side >= 0 ? host_.processor().mixer().micAt(strip, side) : nullptr;
    setMouseCursor(key ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
    if (side < 0) host_.setHint({});
    else host_.setHint("CH " + jackCode({strip, side}) + "  " + ((side == 0 ? micL_ : micR_).isEmpty() ? juce::String("empty")
                                                                                                   : (side == 0 ? micL_ : micR_)));
}

void ChannelStrip::mouseExit(const juce::MouseEvent&) {
    if (hoverSide_ < 0) return;
    hoverSide_ = -1;
    repaint();
}

void ChannelStrip::mouseDown(const juce::MouseEvent& e) {
    carrying_ = false;
    const int side = jackAt(e.position);
    if (side < 0) return;
    const auto* key = host_.processor().mixer().micAt(strip, side);
    if (!key) return;
    carrying_ = true;
    host_.cableBegin(*key, *this, socketCentre(side));
}

void ChannelStrip::mouseDrag(const juce::MouseEvent& e) {
    if (carrying_) host_.cableMove(*this, e.position);
}

void ChannelStrip::mouseUp(const juce::MouseEvent& e) {
    if (std::exchange(carrying_, false)) host_.cableEnd(*this, e.position);
}

void ChannelStrip::setMeter(float peakL, float peakR) {
    const float l = toDb(peakL), r = toDb(peakR);
    dbL_ = std::max(l, dbL_ - 1.6f);
    dbR_ = std::max(r, dbR_ - 1.6f);
    meter_.setLevels(dbL_, dbR_);
}

void ChannelStrip::resized() {
    const auto main = getLocalBounds().withHeight(kMainH);
    info_ = main.withWidth(kInfoW).reduced(12, 0);

    // front: [info] [pan] [fader + meter] [dB] [M][S] [OUT]
    auto m = main.withTrimmedLeft(kInfoW).withTrimmedRight(12);
    out_ = m.removeFromRight(isMaster() ? 50 : 42);
    m.removeFromRight(6);
    auto buttons = m.removeFromRight(46);
    m.removeFromRight(8);
    readout_ = m.removeFromRight(42);
    m.removeFromRight(6);
    pan_.setBounds(m.removeFromLeft(34).withSizeKeepingCentre(32, 32));
    m.removeFromLeft(10);
    // ticks above the track, meter under it
    auto fa = m.withTrimmedTop(13 + kPad).withHeight(18);
    fader_.setBounds(fa);
    const int indent = std::min(12, fa.getHeight() / 2); // JUCE's linear slider thumb indent
    meter_.setBounds(fa.getX() + indent, fa.getBottom() + 2, fa.getWidth() - 2 * indent, 5);
    auto bb = buttons.withSizeKeepingCentre(46, 20);
    mute_.setBounds(bb.removeFromLeft(21));
    bb.removeFromLeft(4);
    solo_.setBounds(bb);

    // rear: [side jacks] [number / mode] [L / R names] [KICK][SNARE][SEND][RET][IN] [OUT]
    auto r = main.withTrimmedLeft(kSideW + 10 + 34).withTrimmedRight(12);
    rearOut_ = r.removeFromRight(isMaster() ? 50 : 42);
    r.removeFromRight(10);
    auto cells = r;
    auto knobAt = [](juce::Rectangle<int> cell) { return cell.withTrimmedTop(14 + kPad).withWidth(26).withHeight(26); };
    if (isMaster()) {
        moCell_ = sendCell_ = cells.removeFromRight(40);
        cells.removeFromRight(10);
        msCell_ = sGroup_ = cells.removeFromRight(40);
        cells.removeFromRight(10);
        mkCell_ = kGroup_ = cells.removeFromRight(40);
        bleedArea_ = kGroup_.getUnion(sendCell_);
        names_ = cells.withTrimmedRight(10);
        mKick_.setBounds(knobAt(mkCell_));
        mSnare_.setBounds(knobAt(msCell_));
        mOther_.setBounds(knobAt(moCell_));
        return;
    }
    inCell_ = cells.removeFromRight(26);
    cells.removeFromRight(8);
    retCell_ = cells.removeFromRight(26);
    cells.removeFromRight(8);
    sendCell_ = cells.removeFromRight(30);
    cells.removeFromRight(12);
    sGroup_ = cells.removeFromRight(80);
    cells.removeFromRight(12);
    kGroup_ = cells.removeFromRight(80);
    bleedArea_ = kGroup_.getUnion(inCell_);
    // a free row has no bleed: its names run up to the output
    names_ = active_ ? cells.withTrimmedRight(10) : r;
    auto segAt = [](juce::Rectangle<int> cell) { return cell.withTrimmedTop(14 + kPad).withTrimmedLeft(29).withHeight(26).withSizeKeepingCentre(cell.getWidth() - 29, 15); };
    kickBleed_.setBounds(knobAt(kGroup_));
    kickDst_.setBounds(segAt(kGroup_));
    snareBleed_.setBounds(knobAt(sGroup_));
    snareDst_.setBounds(segAt(sGroup_));
    send_.setBounds(knobAt(sendCell_));
    ret_.setBounds(knobAt(retCell_));
    in_.setBounds(knobAt(inCell_));
}

void ChannelStrip::paint(juce::Graphics& g) {
    if (back_) {
        paintBack(g);
        return;
    }
    const auto& p = pal();
    g.fillAll(p.panel);
    g.setColour(p.rule);
    g.fillRect(0, getHeight() - 1, getWidth(), 1); // row separator

    // --- info: number + mode, mic, piece ---
    auto info = info_.withTrimmedTop(7 + kPad);
    auto top = info.removeFromTop(14);
    g.setColour(p.text);
    g.setFont(Theme::get().mono(12.5f, true));
    const auto number = isMaster() ? juce::String("MST") : juce::String(strip + 1).paddedLeft('0', 2);
    g.drawText(number, top.removeFromLeft(isMaster() ? 30 : 22), juce::Justification::centredLeft, false);
    top.removeFromLeft(6);
    if (mode_.isNotEmpty()) {
        g.setFont(Theme::get().mono(8.5f));
        const int tw = juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), mode_) + 8;
        draw::tag(g, top.removeFromLeft(tw).withSizeKeepingCentre(tw, 12), mode_, p.textDim);
    }
    info.removeFromTop(4);
    g.setColour(sub_ == "MISSING" ? p.accent : p.text); // a mic the kit no longer has
    g.setFont(Theme::get().mono(10.5f));
    g.drawFittedText(caption_, info.removeFromTop(13), juce::Justification::centredLeft, 1, 0.85f);

    // --- fader: dB ticks above the track, readout on the right ---
    const auto fb = fader_.getBounds();
    const int indent = std::min(12, fb.getHeight() / 2);
    const float x0 = float(fb.getX() + indent), x1 = float(fb.getRight() - indent);
    g.setFont(Theme::get().mono(8.0f));
    for (float db : {-48.0f, -24.0f, -12.0f, -6.0f, 0.0f, 5.0f}) {
        const float x = x0 + faderPosForDb(db) * (x1 - x0);
        g.setColour(db == 0.0f ? p.textDim : p.textFaint);
        g.fillRect(juce::Rectangle<float>(x - 0.5f, float(fb.getY()) + 1.0f, 1.0f, db == 0.0f ? 4.0f : 3.0f));
        g.drawText(db > 0.0f ? "+" + juce::String(int(db)) : juce::String(int(db)),
                   juce::Rectangle<float>(x - 14.0f, float(fb.getY()) - 10.0f, 28.0f, 10.0f), juce::Justification::centred,
                   false);
    }
    g.setColour(p.text);
    g.setFont(Theme::get().mono(10.5f));
    g.drawText(faderText(fader_.getValue()), readout_.withHeight(kMainH), juce::Justification::centredRight, false);

    // --- output number ---
    auto& proc = host_.processor();
    bool busOn = true;
    if (!isMaster())
        if (auto* bus = proc.getBus(false, strip + 1)) busOn = bus->isEnabled();
    g.setColour(busOn ? p.textDim : p.textFaint);
    g.setFont(Theme::get().mono(9.5f));
    g.drawText(isMaster() ? "OUT 1-2" : "OUT " + juce::String(strip + 1), out_.withHeight(kMainH),
               juce::Justification::centredRight, false);

}

//==============================================================================
MixerPanel::MixerPanel(UiHost& host) : host_(host) {
    autoPatch_.setTooltip("Patch loose mics");
    autoPatch_.onClick = [this] {
        juce::PopupMenu m;
        m.addItem(1, "Patch loose mics");
        m.addItem(2, "Repatch every mic");
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&autoPatch_), [this](int r) {
            if (r > 0) host_.processor().autoPatchAll(r == 2);
        });
    };
    flip_.setClickingTogglesState(true);
    flip_.setTooltip("Keep the jacks facing you");
    flip_.onClick = [this] { flipTo(flip_.getToggleState()); };
    cables_.onClick = [this] { showCables(!host_.processor().showCables); };
    addAndMakeVisible(autoPatch_);
    addAndMakeVisible(flip_);
    micLock_.onClick = [this] { lockMics(!host_.processor().micsLocked); };
    addChildComponent(cables_);
    addChildComponent(micLock_);
    showCables(host_.processor().showCables);
    lockMics(host_.processor().micsLocked);
    updateHeadButtons();

    viewport_.setViewedComponent(&holder_, false);
    viewport_.setScrollBarsShown(true, false);
    viewport_.setScrollBarThickness(8);
    addAndMakeVisible(viewport_);
    for (int s = 0; s < bfdengine::kNumStrips; ++s) {
        rows_[size_t(s)] = std::make_unique<ChannelStrip>(host_, s);
        rows_[size_t(s)]->onKitEdited = [this] {
            for (auto& r : rows_)
                if (r->isActive()) r->sync();
        };
        holder_.addChildComponent(*rows_[size_t(s)]);
    }
    master_ = std::make_unique<ChannelStrip>(host_, -1);
    addAndMakeVisible(*master_);
    sync();
}

MixerPanel::~MixerPanel() { viewport_.setViewedComponent(nullptr, false); }

void MixerPanel::showCables(bool show) {
    // the editor reads the flag every frame; a cable in the hand always shows
    host_.processor().showCables = show;
    cables_.setButtonText(show ? "HIDE CABLES" : "SHOW CABLES");
    cables_.setTooltip(show ? "Hide the plugged cables" : "Show the plugged cables");
}

void MixerPanel::lockMics(bool locked) {
    host_.setMicsLocked(locked);
    micLock_.setToggleState(locked, juce::dontSendNotification);
    micLock_.setButtonText(locked ? "UNLOCK MICS" : "LOCK MICS");
    micLock_.setTooltip(locked ? "Let the stage mics take the mouse again" : "Stage mics ignore the mouse");
}

void MixerPanel::updateHeadButtons() {
    cables_.setVisible(target_);
    micLock_.setVisible(!target_);
}

void MixerPanel::applyFace(bool back) {
    face_ = back;
    for (auto& r : rows_) r->setBack(back);
    master_->setBack(back);
    layoutRows();
}

void MixerPanel::layoutRows() {
    // channels in number order: the front shows the patched ones, the rear
    // every channel
    order_.clear();
    for (int s = 0; s < bfdengine::kNumStrips; ++s)
        if (face_ || rows_[size_t(s)]->isActive()) order_.push_back(s);
    // never hide a row that stays: it may hold the mouse (a fader drag)
    for (int s = 0; s < bfdengine::kNumStrips; ++s)
        rows_[size_t(s)]->setVisible(std::find(order_.begin(), order_.end(), s) != order_.end());
    const auto f = viewport_.getLocalBounds();
    int total = 0;
    for (int s : order_) total += rows_[size_t(s)]->preferredHeight();
    const int w = f.getWidth() - (total > f.getHeight() ? viewport_.getScrollBarThickness() : 0);
    holder_.setSize(std::max(w, 1), std::max(total, 1));
    int y = 0;
    for (int s : order_) {
        auto& r = *rows_[size_t(s)];
        r.setBounds(0, y, w, r.preferredHeight());
        y += r.preferredHeight();
    }
}

void MixerPanel::sync() {
    for (auto& r : rows_) r->sync();
    master_->sync();
    layoutRows();
    repaint();
}

void MixerPanel::updateMeters(bfdengine::Engine& engine) {
    std::array<float, bfdengine::Engine::kNumMeters> peaks{};
    for (int i = 0; i < bfdengine::Engine::kNumMeters; ++i) peaks[size_t(i)] = engine.takeMeter(i);
    if (face_) return;
    master_->setMeter(peaks[0], peaks[1]);
    for (int s : order_) rows_[size_t(s)]->setMeter(peaks[size_t(2 + s * 2)], peaks[size_t(3 + s * 2)]);
}

bfdengine::Jack MixerPanel::jackAt(juce::Point<float> local) const {
    if (!face_ || !viewport_.getBounds().toFloat().contains(local)) return {};
    for (int s : order_) {
        const auto& r = *rows_[size_t(s)];
        const int side = r.jackAt(r.getLocalPoint(this, local));
        if (side >= 0) return {s, side};
    }
    return {};
}

MixerPanel::Socket MixerPanel::liveSocket(bfdengine::Jack j) const {
    const auto vb = viewport_.getBounds().toFloat();
    if (!j.valid() || !rows_[size_t(j.strip)]->isVisible()) return {{vb.getX(), vb.getCentreY()}, false};
    const auto& r = *rows_[size_t(j.strip)];
    auto c = getLocalPoint(&r, r.socketCentre(j.side));
    const bool inView = face_ && c.y >= vb.getY() && c.y <= vb.getBottom();
    // a jack scrolled out of view: the cable runs to the edge of the list
    c.y = juce::jlimit(vb.getY() + 4.0f, vb.getBottom() - 4.0f, c.y);
    return {c, inView};
}

MixerPanel::Sockets MixerPanel::allSockets() const {
    Sockets out;
    for (int s = 0; s < bfdengine::kNumStrips; ++s)
        for (int side = 0; side < 2; ++side) out[size_t(s * 2 + side)] = liveSocket({s, side});
    return out;
}

MixerPanel::Socket MixerPanel::socket(bfdengine::Jack j) const {
    if (frontShot_.isNull() && backShot_.isNull()) return liveSocket(j);
    if (!j.valid()) return {};
    // turning: the jack rides on the face that is showing as it narrows
    const float shown = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::pi * angle_);
    const float sx = std::abs(std::cos(juce::MathConstants<float>::pi * shown));
    auto s = (shown > 0.5f ? backSockets_ : frontSockets_)[size_t(j.strip * 2 + j.side)];
    const float cx = body().toFloat().getCentreX();
    s.at.x = cx + (s.at.x - cx) * sx;
    return s;
}

bool MixerPanel::jackInView(bfdengine::Jack j) const { return socket(j).inView; }

juce::Point<float> MixerPanel::socketCentre(bfdengine::Jack j) const { return socket(j).at; }

void MixerPanel::setHotJack(bfdengine::Jack j) {
    if (j == hot_) return;
    if (hot_.valid()) rows_[size_t(hot_.strip)]->setHotSide(-1);
    hot_ = j;
    if (j.valid()) rows_[size_t(j.strip)]->setHotSide(j.side);
}

float MixerPanel::backAmount() const {
    const float shown = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::pi * angle_);
    return juce::jlimit(0.0f, 1.0f, (shown - 0.5f) * 2.0f);
}

juce::Rectangle<int> MixerPanel::body() const { return getLocalBounds().withTrimmedTop(36); }

void MixerPanel::flipTo(bool back) {
    if (pinned() && !back) back = true;
    const bool resting = angle_ <= 0.0f || angle_ >= 1.0f;
    if (back == target_ && resting) return;
    target_ = back;
    flip_.setToggleState(pinned(), juce::dontSendNotification);
    updateHeadButtons();
    if (resting) {
        // photograph both faces (same rows, same scroll), then turn
        auto before = createComponentSnapshot(body(), true, 2.0f);
        const auto socketsBefore = allSockets();
        applyFace(back);
        auto after = createComponentSnapshot(body(), true, 2.0f);
        const auto socketsAfter = allSockets();
        frontShot_ = back ? before : after;
        backShot_ = back ? after : before;
        frontSockets_ = back ? socketsBefore : socketsAfter;
        backSockets_ = back ? socketsAfter : socketsBefore;
        viewport_.setVisible(false);
        master_->setVisible(false);
    }
    lastTick_ = juce::Time::getMillisecondCounterHiRes();
    startTimerHz(60);
}

void MixerPanel::timerCallback() {
    const double now = juce::Time::getMillisecondCounterHiRes();
    const float dt = float(juce::jlimit(0.0, 50.0, now - lastTick_));
    lastTick_ = now;
    const float speed = dt / 380.0f;
    angle_ = target_ ? std::min(1.0f, angle_ + speed) : std::max(0.0f, angle_ - speed);
    if (target_ ? angle_ >= 1.0f : angle_ <= 0.0f) {
        stopTimer();
        if (face_ != target_) applyFace(target_); // reversed mid-turn
        frontShot_ = backShot_ = {};
        viewport_.setVisible(true);
        master_->setVisible(true);
    }
    repaint();
}

void MixerPanel::resized() {
    auto r = getLocalBounds();
    auto head = r.removeFromTop(36).reduced(14, 7);
    flip_.setBounds(head.removeFromRight(58));
    head.removeFromRight(6);
    autoPatch_.setBounds(head.removeFromRight(58));
    head.removeFromRight(6);
    cables_.setBounds(head.removeFromRight(92));
    micLock_.setBounds(cables_.getBounds());
    master_->setBounds(r.removeFromBottom(ChannelStrip::kMainH));
    r.removeFromBottom(1);
    viewport_.setBounds(r);
    layoutRows();
}

void MixerPanel::paint(juce::Graphics& g) {
    const auto& p = pal();
    g.fillAll(p.panel);
    auto head = getLocalBounds().removeFromTop(36).reduced(14, 0);
    g.setColour(p.text);
    g.setFont(Theme::get().mono(11.0f, true));
    g.drawText("MIXER", head, juce::Justification::centredLeft, false);
    g.setColour(p.textDim);
    g.setFont(Theme::get().mono(10.5f));
    int active = 0;
    for (auto& r : rows_) active += r->isActive() ? 1 : 0;
    g.drawText(target_ ? juce::String("REAR  L = MONO  L+R = ST") : juce::String(active) + " CH", head.withTrimmedLeft(52),
               juce::Justification::centredLeft, false);
    if (!frontShot_.isNull() || !backShot_.isNull()) {
        // turning around: the facing side narrows to an edge, the other side widens out of it
        const auto b = body();
        const float shown = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::pi * angle_);
        const bool rear = shown > 0.5f;
        const float sx = std::abs(std::cos(juce::MathConstants<float>::pi * shown));
        g.setColour(p.canvas);
        g.fillRect(b);
        const auto dest = b.toFloat().withSizeKeepingCentre(std::max(1.0f, float(b.getWidth()) * sx), float(b.getHeight()));
        const auto& img = rear ? backShot_ : frontShot_;
        if (!img.isNull()) g.drawImage(img, dest, juce::RectanglePlacement::stretchToFit);
        g.setColour(p.ruleStrong);
        g.drawRect(dest, 1.0f);
        return;
    }
    g.setColour(p.ruleStrong);
    g.fillRect(0, master_->getY() - 1, getWidth(), 1);
    if (order_.empty()) {
        g.setColour(p.textDim);
        g.setFont(Theme::get().mono(11.0f));
        auto c = viewport_.getBounds().withSizeKeepingCentre(260, 40);
        g.drawText("NO CHANNELS", c.removeFromTop(20), juce::Justification::centred, false);
        g.drawText("DRAG A MIC ONTO A JACK", c, juce::Justification::centred, false);
    }
}

} // namespace openbfd::ui
