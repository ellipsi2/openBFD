#include "KeymapPanel.h"

#include "../PluginProcessor.h"
#include "KitArt.h"
#include "Motion.h"

namespace openbfd::ui {

using bfdmodel::PieceKind;

namespace {
constexpr int kRowH = 24, kHeaderH = 30, kIndent = 22;
constexpr int kChipW = 46, kChipGap = 4, kPlusW = 20, kLearnW = 54;
constexpr double kFlashMs = 260.0;

double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }
} // namespace

juce::String noteName(int note) { return juce::MidiMessage::getMidiNoteName(note, true, true, 3); }

juce::String articulationLabel(const std::string& name) {
    static const std::map<std::string, const char*> known = {
        {"Rim", "Rim shot"},          {"Rim Click", "Cross-stick"},  {"HalfEdge", "Half edge"},
        {"NoSnare", "Snares off"},    {"ClosedT", "Closed tip"},     {"ClosedS", "Closed shank"},
        {"QuarterT", "1/4 open tip"}, {"QuarterS", "1/4 open shank"}, {"HalfT", "Half open tip"},
        {"HalfS", "Half open shank"}, {"ThreeqT", "3/4 open tip"},   {"ThreeqS", "3/4 open shank"},
        {"OpenT", "Open tip"},        {"OpenS", "Open shank"},       {"BellT", "Bell tip"},
        {"Splash", "Foot splash"},
    };
    const auto it = known.find(name);
    return it != known.end() ? juce::String(it->second) : juce::String(name);
}

//==============================================================================
class KeymapPanel::RowList : public juce::Component {
public:
    explicit RowList(KeymapPanel& owner) : owner_(owner) {}

    std::vector<Row> rows;
    int learnSlot = -1;
    std::string learnArtic;
    std::array<double, 128> flashAt{};

    bool learning() const { return learnSlot >= 0; }

    void layout() {
        tops_.resize(rows.size());
        int y = 0;
        for (size_t i = 0; i < rows.size(); ++i) {
            tops_[i] = y;
            y += rowHeight(int(i));
        }
        setSize(getWidth(), std::max(y, 1));
        hover_ = {};
        repaint();
    }

    int rowHeight(int i) const { return rows[size_t(i)].header ? kHeaderH : kRowH; }

    // What sits under a point of an articulation row.
    enum class Part { none, name, chip, plus, learn };
    struct Hit {
        int row = -1;
        Part part = Part::none;
        int note = -1;
        bool operator==(const Hit& o) const { return row == o.row && part == o.part && note == o.note; }
    };

    juce::Rectangle<int> rowRect(int i) const { return {0, tops_[size_t(i)], getWidth(), rowHeight(i)}; }
    juce::Rectangle<int> learnRect(juce::Rectangle<int> r) const {
        return {r.getRight() - 14 - kLearnW, r.getY() + 3, kLearnW, r.getHeight() - 6};
    }
    juce::Rectangle<int> plusRect(juce::Rectangle<int> r) const {
        return learnRect(r).translated(-kPlusW - 6, 0).withWidth(kPlusW);
    }
    int nameWidth() const { return juce::jlimit(140, 230, getWidth() * 34 / 100); }
    juce::Rectangle<int> nameRect(juce::Rectangle<int> r) const {
        return r.withX(14 + kIndent).withWidth(nameWidth());
    }
    // Chip rectangles for the row's notes; the ones that do not fit are left out.
    std::vector<juce::Rectangle<int>> chipRects(int i) const {
        std::vector<juce::Rectangle<int>> out;
        const auto r = rowRect(i);
        int x = nameRect(r).getRight() + 8;
        const int limit = plusRect(r).getX() - 8;
        for (size_t n = 0; n < rows[size_t(i)].notes.size() && x + kChipW <= limit; ++n) {
            out.push_back({x, r.getY() + 4, kChipW, r.getHeight() - 8});
            x += kChipW + kChipGap;
        }
        return out;
    }

    Hit hitAt(juce::Point<int> pt) const {
        for (size_t i = 0; i < rows.size(); ++i) {
            const auto r = rowRect(int(i));
            if (!r.contains(pt)) continue;
            if (rows[i].header) return {int(i), Part::none, -1};
            if (learnRect(r).contains(pt)) return {int(i), Part::learn, -1};
            if (plusRect(r).contains(pt)) return {int(i), Part::plus, -1};
            const auto chips = chipRects(int(i));
            for (size_t c = 0; c < chips.size(); ++c)
                if (chips[c].contains(pt)) return {int(i), Part::chip, rows[i].notes[c]};
            if (nameRect(r).contains(pt)) return {int(i), Part::name, -1};
            return {int(i), Part::none, -1};
        }
        return {};
    }

    bool isLearnRow(const Row& row) const { return row.slot == learnSlot && row.articulation == learnArtic; }

    bool flashing() const {
        const double t = nowMs();
        return std::any_of(flashAt.begin(), flashAt.end(), [&](double f) { return t - f < kFlashMs; });
    }

    void paint(juce::Graphics& g) override {
        const auto& p = pal();
        const auto clip = g.getClipBounds();
        const PieceInk ink{p.text, p.textDim, p.panel};
        const double t = nowMs();
        auto flash = [&](int note) {
            return note >= 0 && note < 128 ? float(std::max(0.0, 1.0 - (t - flashAt[size_t(note)]) / kFlashMs)) : 0.0f;
        };
        for (size_t i = 0; i < rows.size(); ++i) {
            const auto& row = rows[i];
            const auto r = rowRect(int(i));
            if (!r.intersects(clip)) continue;
            if (row.header) {
                auto content = r.withTrimmedTop(6).reduced(14, 0);
                drawPieceIcon(g, PieceKind(row.kind), content.removeFromLeft(14).withSizeKeepingCentre(14, 14).toFloat(),
                              ink);
                content.removeFromLeft(8);
                g.setFont(Theme::get().mono(9.5f));
                g.setColour(p.textDim);
                const int cw = juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), row.sub) + 8;
                g.drawText(row.sub, content.removeFromLeft(cw), juce::Justification::centredLeft, false);
                g.setFont(Theme::get().mono(11.0f, true));
                g.setColour(p.text);
                const int nw = std::min(content.getWidth(),
                                        juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), row.text) + 10);
                g.drawText(row.text, content.removeFromLeft(nw), juce::Justification::centredLeft, true);
                const auto band = content.withSizeKeepingCentre(content.getWidth(), 6).translated(0, 1);
                if (band.getWidth() > 12) draw::hatch(g, band, p.rule, 5);
                continue;
            }
            const bool armed = isLearnRow(row);
            const bool hot = hover_.row == int(i);
            const float hover = motion::follow(*this, "h" + juce::String(int(i)), hot ? 1.0f : 0.0f, motion::quick, r);
            float rowFlash = 0.0f;
            for (int n : row.notes) rowFlash = std::max(rowFlash, flash(n));
            if (armed) {
                g.setColour(p.panelInset);
                g.fillRect(r);
            } else if (hover > 0.0f) {
                g.setColour(p.panelInset.withAlpha(hover));
                g.fillRect(r);
            }
            if (armed || rowFlash > 0.0f) {
                g.setColour(p.accent.withAlpha(armed ? 1.0f : rowFlash));
                g.fillRect(r.withWidth(2));
            }

            // name: label, the BFD code after it when they differ
            auto name = nameRect(r);
            const bool nameHot = hot && hover_.part == Part::name;
            g.setFont(Theme::get().mono(11.0f));
            g.setColour(row.notes.empty() ? p.textDim : p.text);
            if (nameHot && !row.notes.empty()) g.setColour(p.accent);
            const int lw = std::min(name.getWidth(), juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), row.text) + 8);
            g.drawText(row.text, name.removeFromLeft(lw), juce::Justification::centredLeft, true);
            if (row.sub.isNotEmpty()) {
                g.setFont(Theme::get().mono(9.0f));
                g.setColour(p.textFaint);
                g.drawText(row.sub, name, juce::Justification::centredLeft, true);
            }

            // note chips
            const auto chips = chipRects(int(i));
            for (size_t c = 0; c < chips.size(); ++c) {
                const int note = row.notes[c];
                const auto cr = chips[c];
                const float f = flash(note);
                const bool chipHot = hot && hover_.part == Part::chip && hover_.note == note;
                if (f > 0.0f) {
                    g.setColour(p.accent.withAlpha(f));
                    g.fillRect(cr);
                }
                g.setColour(chipHot ? p.accent : p.ruleStrong);
                g.drawRect(cr, 1);
                g.setFont(Theme::get().mono(9.5f));
                g.setColour(p.text.interpolatedWith(p.accentText, f));
                if (chipHot) g.setColour(p.accent);
                g.drawText(chipHot ? juce::String::fromUTF8("\xc3\x97 ") + juce::String(note)
                                   : juce::String(note) + " " + noteName(note),
                           cr, juce::Justification::centred, false);
            }
            if (chips.size() < row.notes.size()) {
                g.setFont(Theme::get().mono(9.5f));
                g.setColour(p.textDim);
                const int x = chips.empty() ? nameRect(r).getRight() + 8 : chips.back().getRight() + kChipGap;
                g.drawText("+" + juce::String(row.notes.size() - chips.size()), juce::Rectangle<int>(x, r.getY(), 30, r.getHeight()),
                           juce::Justification::centredLeft, false);
            }

            // "+" and LEARN, drawn like the panel buttons
            auto button = [&](juce::Rectangle<int> b, const juce::String& text, bool on, bool over) {
                const auto bf = b.toFloat().reduced(0.5f);
                g.setColour(on ? p.accent : p.panel.overlaidWith(p.text.withAlpha(over ? 0.05f : 0.0f)));
                g.fillRect(bf);
                g.setColour(on || over ? p.accent : p.ruleStrong);
                g.drawRect(bf, 1.0f);
                g.setColour(on ? p.accentText : p.text);
                g.setFont(Theme::get().mono(9.5f));
                g.drawText(text, b, juce::Justification::centred, false);
            };
            if (hot || armed) {
                button(plusRect(r), "+", false, hot && hover_.part == Part::plus);
                button(learnRect(r), armed ? "PLAY" : "LEARN", armed, hot && hover_.part == Part::learn);
            }
        }
    }

    void mouseMove(const juce::MouseEvent& e) override { setHover(hitAt(e.getPosition())); }
    void mouseExit(const juce::MouseEvent&) override { setHover({}); }

    void mouseUp(const juce::MouseEvent& e) override {
        if (e.mouseWasDraggedSinceMouseDown()) return;
        const auto h = hitAt(e.getPosition());
        if (h.row < 0 || rows[size_t(h.row)].header) return;
        const Row row = rows[size_t(h.row)];
        auto& proc = owner_.host_.processor();
        if (h.part != Part::learn && learning()) setLearn(-1, {});
        switch (h.part) {
            case Part::name:
                if (!row.notes.empty()) proc.auditionNote(row.notes.front(), 0.8f);
                break;
            case Part::chip:
                proc.clearNote(h.note);
                owner_.host_.setHint(juce::String(h.note) + " " + noteName(h.note) + "  unassigned");
                break;
            case Part::plus: showNoteMenu(row, rowRect(h.row)); break;
            case Part::learn:
                if (isLearnRow(row)) setLearn(-1, {});
                else setLearn(row.slot, row.articulation);
                break;
            case Part::none: break;
        }
    }

    void setLearn(int slot, std::string artic) {
        learnSlot = slot;
        learnArtic = std::move(artic);
        owner_.host_.setHint(slot >= 0 ? "Play a note to assign it" : juce::String());
        repaint();
    }

    // "SNR  Rim shot" for what `note` plays now, "" when it plays nothing.
    juce::String targetOf(int note) const {
        for (const auto& k : owner_.host_.processor().keymap())
            if (k.note == note)
                for (const auto& row : rows)
                    if (!row.header && row.slot == k.slot && std::find(row.notes.begin(), row.notes.end(), note) != row.notes.end())
                        return slotCode(row.slot) + "  " + row.text;
        return {};
    }

    juce::String slotCode(int slot) const {
        for (const auto& row : rows)
            if (row.header && row.slot == slot) return row.sub;
        return {};
    }

private:
    void setHover(Hit h) {
        if (h == hover_) return;
        hover_ = h;
        repaint();
        if (learning()) return; // the hint says what to do
        if (h.row < 0 || rows[size_t(h.row)].header) {
            owner_.host_.setHint({});
            return;
        }
        const auto& row = rows[size_t(h.row)];
        const auto what = slotCode(row.slot) + "  " + row.text;
        switch (h.part) {
            case Part::name:
                owner_.host_.setHint(row.notes.empty() ? what + "  no note" : what + "  click to play");
                break;
            case Part::chip:
                owner_.host_.setHint(juce::String(h.note) + " " + noteName(h.note) + arrowText() + what + "  click to unassign");
                break;
            case Part::plus: owner_.host_.setHint("Pick a note for " + what); break;
            case Part::learn: owner_.host_.setHint("Assign the next note played to " + what); break;
            case Part::none: owner_.host_.setHint(what); break;
        }
    }

    void showNoteMenu(const Row& row, juce::Rectangle<int> r) {
        juce::PopupMenu menu;
        for (int octave = -2; octave <= 8; ++octave) {
            juce::PopupMenu sub;
            bool any = false;
            for (int n = (octave + 2) * 12; n < (octave + 3) * 12 && n < 128; ++n) {
                const bool mine = std::find(row.notes.begin(), row.notes.end(), n) != row.notes.end();
                const auto target = mine ? juce::String() : targetOf(n);
                sub.addItem(n + 1, juce::String(n).paddedLeft(' ', 3) + "  " + noteName(n).paddedRight(' ', 4) +
                                       (target.isNotEmpty() ? "  now " + target : juce::String()),
                            true, mine);
                any = any || mine;
            }
            menu.addSubMenu("C" + juce::String(octave) + "  " + juce::String((octave + 2) * 12) + "+", sub, true, nullptr, any);
        }
        const int slot = row.slot;
        const auto artic = row.articulation;
        juce::Component::SafePointer<RowList> safe(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(localAreaToGlobal(plusRect(r))),
                           [safe, slot, artic](int result) {
                               if (!safe || result <= 0) return;
                               safe->owner_.host_.processor().assignNote(result - 1, slot, artic);
                           });
    }

    KeymapPanel& owner_;
    std::vector<int> tops_;
    Hit hover_;
};

//==============================================================================
KeymapPanel::KeymapPanel(UiHost& host) : host_(host) {
    list_ = std::make_unique<RowList>(*this);
    viewport_.setViewedComponent(list_.get(), false);
    viewport_.setScrollBarsShown(true, false);
    viewport_.setScrollBarThickness(8);
    addAndMakeVisible(viewport_);
    reset_.setTooltip("Back to the GM map built from the kit");
    reset_.onClick = [this] { host_.processor().resetKeymap(); };
    addAndMakeVisible(reset_);
    sync();
}

KeymapPanel::~KeymapPanel() { viewport_.setViewedComponent(nullptr, false); }

bool KeymapPanel::learning() const { return list_->learning(); }

void KeymapPanel::sync() {
    auto& proc = host_.processor();
    const auto pieces = proc.pieces();
    const auto& map = proc.keymap();
    std::vector<Row> rows;
    bool learnFound = false;
    for (size_t s = 0; s < pieces.size(); ++s) {
        const auto* piece = pieces[s];
        if (!piece) continue;
        const auto kind = bfdmodel::pieceKind(*piece);
        Row h;
        h.header = true;
        h.slot = int(s);
        h.kind = int(kind);
        h.text = piece->name;
        h.sub = bfdmodel::pieceKindCode(kind);
        rows.push_back(h);
        // Info.xml order first (the order BFD lists them in), then the rest
        std::vector<const bfdmodel::Articulation*> arts;
        for (const auto& n : piece->articOrder)
            if (const auto* a = piece->findArticulation(n); a && !a->layers.empty()) arts.push_back(a);
        for (const auto& a : piece->articulations)
            if (!a.layers.empty() && std::find(arts.begin(), arts.end(), &a) == arts.end()) arts.push_back(&a);
        for (const auto* a : arts) {
            Row r;
            r.slot = int(s);
            r.kind = int(kind);
            r.articulation = a->name;
            r.text = articulationLabel(a->name);
            if (r.text != juce::String(a->name)) r.sub = a->name;
            for (const auto& k : map)
                if (k.slot == int(s) && k.note >= 0 && k.note < 128 &&
                    bfdengine::resolveArticulation(*piece, k.articulation) == a)
                    r.notes.push_back(k.note);
            std::sort(r.notes.begin(), r.notes.end());
            learnFound = learnFound || list_->isLearnRow(r);
            rows.push_back(std::move(r));
        }
    }
    list_->rows = std::move(rows);
    if (!learnFound) list_->learnSlot = -1;
    list_->setSize(viewport_.getMaximumVisibleWidth(), 1);
    list_->layout();
    reset_.setEnabled(!proc.keymapIsAuto());
    repaint();
}

void KeymapPanel::noteIn(int note) {
    if (note < 0 || note > 127) return;
    if (list_->learning()) {
        const auto what = list_->slotCode(list_->learnSlot) + "  " + articulationLabel(list_->learnArtic);
        host_.processor().assignNote(note, list_->learnSlot, list_->learnArtic);
        list_->setLearn(-1, {});
        host_.setHint(juce::String(note) + " " + noteName(note) + arrowText() + what);
    }
    list_->flashAt[size_t(note)] = nowMs();
    const auto target = list_->targetOf(note);
    lastIn_ = juce::String(note) + " " + noteName(note) + (target.isNotEmpty() ? arrowText() + target : "  ---");
    repaint(0, 0, getWidth(), 36);
    if (!isTimerRunning()) startTimerHz(30);
}

void KeymapPanel::timerCallback() {
    list_->repaint();
    if (!list_->flashing() && !list_->learning()) stopTimer();
}

void KeymapPanel::resized() {
    auto r = getLocalBounds();
    auto head = r.removeFromTop(36);
    reset_.setBounds(head.removeFromRight(14 + 80).withTrimmedRight(14).withSizeKeepingCentre(80, 22));
    r.removeFromTop(1);
    viewport_.setBounds(r);
    list_->setSize(viewport_.getMaximumVisibleWidth(), list_->getHeight());
    list_->layout();
}

void KeymapPanel::paint(juce::Graphics& g) {
    const auto& p = pal();
    g.fillAll(p.panel);
    auto head = getLocalBounds().removeFromTop(36).reduced(14, 0).withTrimmedRight(80 + 14);
    g.setColour(p.text);
    g.setFont(Theme::get().mono(11.0f, true));
    g.drawText("KEYMAP", head, juce::Justification::centredLeft, false);
    g.setColour(p.textDim);
    g.setFont(Theme::get().mono(10.5f));
    g.drawText(host_.processor().keymapIsAuto() ? "AUTO GM" : "CUSTOM", head.withTrimmedLeft(58),
               juce::Justification::centredLeft, false);
    if (lastIn_.isNotEmpty()) {
        // the last note in from the host, right-aligned before the button
        g.setFont(Theme::get().mono(10.5f));
        const auto text = lastIn_.toUpperCase();
        const int w = juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), text);
        auto area = head.removeFromRight(std::min(w, head.getWidth() - 140));
        g.setColour(p.text);
        g.drawText(text, area, juce::Justification::centredRight, true);
        g.setColour(p.textDim);
        g.drawText("IN", area.translated(-30, 0).withWidth(24), juce::Justification::centredRight, false);
    }
    g.setColour(p.ruleStrong);
    g.fillRect(0, 36, getWidth(), 1);
}

} // namespace openbfd::ui
