#include "Browser.h"

#include "../PluginProcessor.h"
#include "KitArt.h"
#include "Motion.h"

namespace openbfd::ui {

using bfdmodel::PieceKind;

namespace {
constexpr int kRowH = 24;
constexpr int kHeaderH = 26;

juce::String libraryLabel(const bfdmodel::Library& lib) {
    return lib.name.empty() ? juce::File(lib.root).getFileName() : juce::String(lib.name);
}

juce::String inches(float d) { return d > 0.0f ? juce::String(juce::roundToInt(d)) + "\"" : juce::String(); }
} // namespace

//==============================================================================
class Browser::RowList : public juce::Component {
public:
    explicit RowList(Browser& owner) : owner_(owner) {}

    std::vector<Row> rows;

    int rowTop(int i) const { return tops_[size_t(i)]; }
    int rowHeight(int i) const { return rows[size_t(i)].header ? kHeaderH : kRowH; }

    void layout() {
        tops_.resize(rows.size());
        int y = 0;
        for (size_t i = 0; i < rows.size(); ++i) {
            tops_[i] = y;
            y += rowHeight(int(i));
        }
        setSize(getWidth(), std::max(y, 1));
        hover_ = pressed_ = -1;
        repaint();
    }

    int rowAt(int y) const {
        for (size_t i = 0; i < rows.size(); ++i)
            if (y >= tops_[i] && y < tops_[i] + rowHeight(int(i))) return int(i);
        return -1;
    }

    void paint(juce::Graphics& g) override {
        const auto& p = pal();
        const auto clip = g.getClipBounds();
        const PieceInk ink{p.text, p.textDim, p.panel};
        for (size_t i = 0; i < rows.size(); ++i) {
            const auto& row = rows[i];
            const juce::Rectangle<int> r(0, tops_[i], getWidth(), rowHeight(int(i)));
            if (!r.intersects(clip)) continue;
            if (row.header) {
                g.setColour(p.textDim);
                g.setFont(Theme::get().mono(9.5f));
                const auto label = row.text.toUpperCase();
                const int tw = juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), label);
                const auto textArea = r.withTrimmedLeft(14).withTrimmedTop(6);
                g.drawText(label, textArea, juce::Justification::centredLeft, false);
                g.drawText(row.right, textArea.withTrimmedRight(14), juce::Justification::centredRight, false);
                const int cw = juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), row.right);
                // hatched band between the section name and its count
                const auto band = juce::Rectangle<int>(14 + tw + 8, r.getCentreY() + 1, 0, 6)
                                      .withRight(getWidth() - 14 - cw - 8);
                if (band.getWidth() > 12) draw::hatch(g, band, p.rule, 5);
                continue;
            }
            const float hover = motion::follow(*this, "h" + juce::String(int(i)), int(i) == hover_ ? 1.0f : 0.0f,
                                               motion::quick, r);
            if (row.loaded) {
                g.setColour(p.panelInset);
                g.fillRect(r);
                g.setColour(p.accent);
                g.fillRect(r.withWidth(2));
            } else if (hover > 0.0f) {
                g.setColour(p.panelInset.withAlpha(hover));
                g.fillRect(r);
            }
            auto content = r.withTrimmedLeft(14).withTrimmedRight(14);
            if (row.kind >= 0) {
                drawPieceIcon(g, PieceKind(row.kind), content.removeFromLeft(14).withSizeKeepingCentre(14, 14).toFloat(),
                              ink);
                content.removeFromLeft(8);
            }
            g.setFont(Theme::get().mono(9.5f));
            g.setColour(p.textDim);
            const int rw = row.right.isEmpty() ? 0 : juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), row.right) + 8;
            g.drawText(row.right, content.removeFromRight(rw), juce::Justification::centredRight, false);
            g.setFont(Theme::get().mono(11.0f));
            g.setColour(p.text);
            g.drawText(row.text, content, juce::Justification::centredLeft, true);
        }
    }

    void mouseMove(const juce::MouseEvent& e) override { setHover(rowAt(e.y)); }
    void mouseExit(const juce::MouseEvent&) override { setHover(-1); }

    void mouseDown(const juce::MouseEvent& e) override {
        pressed_ = rowAt(e.y);
        dragging_ = false;
    }

    void mouseDrag(const juce::MouseEvent& e) override {
        if (dragging_ || pressed_ < 0 || e.getDistanceFromDragStart() < 5) return;
        const auto& row = rows[size_t(pressed_)];
        if (row.header || row.kind < 0) return;
        if (auto* dnd = juce::DragAndDropContainer::findParentDragContainerFor(this)) {
            dragging_ = true;
            dnd->startDragging("piece:" + juce::String(row.payload), this, juce::ScaledImage(dragImage(row), 2.0),
                               true, nullptr, &e.source);
        }
    }

    void mouseUp(const juce::MouseEvent& e) override {
        if (dragging_ || pressed_ < 0 || rowAt(e.y) != pressed_ || e.mouseWasDraggedSinceMouseDown()) return;
        const auto& row = rows[size_t(pressed_)];
        if (row.header) return;
        if (row.kind < 0) owner_.host_.processor().loadKitFile(juce::String(row.payload));
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override {
        const int i = rowAt(e.y);
        if (i < 0 || rows[size_t(i)].kind < 0) return;
        owner_.host_.processor().placePiece(-1, rows[size_t(i)].payload, -1.0f, -1.0f);
    }

private:
    void setHover(int i) {
        if (i == hover_) return;
        hover_ = i;
        repaint();
        if (i >= 0 && !rows[size_t(i)].header) {
            const auto& row = rows[size_t(i)];
            owner_.host_.setHint(row.text + (row.sub.isNotEmpty() ? "  " + row.sub : juce::String()));
        } else {
            owner_.host_.setHint({});
        }
    }

    // Piece glyph on a sheet tile, the way it will land on the stage.
    static juce::Image dragImage(const Row& row) {
        const auto& p = pal();
        const int s = 2, w = 132, h = 56;
        juce::Image img(juce::Image::ARGB, w * s, h * s, true);
        juce::Graphics g(img);
        g.addTransform(juce::AffineTransform::scale(float(s)));
        const juce::Rectangle<float> tile(0.5f, 0.5f, float(w) - 1.0f, float(h) - 1.0f);
        g.setColour(p.panel.withAlpha(0.94f));
        g.fillRect(tile);
        g.setColour(p.accent);
        g.drawRect(tile, 1.0f);
        drawPieceIcon(g, PieceKind(row.kind), juce::Rectangle<float>(10.0f, 14.0f, 28.0f, 28.0f),
                      {p.text, p.textDim, p.panel});
        g.setColour(p.textDim);
        g.setFont(Theme::get().mono(8.5f));
        g.drawText(bfdmodel::pieceKindCode(PieceKind(row.kind)), juce::Rectangle<int>(46, 10, w - 52, 12),
                   juce::Justification::centredLeft, false);
        g.setColour(p.text);
        g.setFont(Theme::get().mono(10.5f));
        g.drawFittedText(row.text, juce::Rectangle<int>(46, 22, w - 52, 26), juce::Justification::topLeft, 2, 1.0f);
        return img;
    }

    Browser& owner_;
    std::vector<int> tops_;
    int hover_ = -1, pressed_ = -1;
    bool dragging_ = false;
};

//==============================================================================
Browser::Browser(UiHost& host) : host_(host) {
    list_ = std::make_unique<RowList>(*this);
    viewport_.setViewedComponent(list_.get(), false);
    viewport_.setScrollBarsShown(true, false);
    viewport_.setScrollBarThickness(8);
    addAndMakeVisible(viewport_);

    tabs_.hints = {"Kits", "Presets: kit + MIDI map + mixer", "Pieces: drag onto the stage"};
    tabs_.onHint = [this](const juce::String& h) { host_.setHint(h); };
    tabs_.onChange = [this](int) { rebuildRows(); };
    addAndMakeVisible(tabs_);

    search_.setTextToShowWhenEmpty("SEARCH", pal().textFaint);
    search_.setFont(Theme::get().mono(11.0f));
    search_.setIndents(8, 5);
    search_.setJustification(juce::Justification::centredLeft);
    search_.addListener(this);
    addAndMakeVisible(search_);

    addFolder_.onClick = [this] { chooseFolder(); };
    addFolder_.setTooltip("Add a BFD library folder");
    addAndMakeVisible(addFolder_);
    refresh();
}

Browser::~Browser() { viewport_.setViewedComponent(nullptr, false); }

void Browser::textEditorTextChanged(juce::TextEditor&) { rebuildRows(); }

void Browser::chooseFolder() {
    chooser_ = std::make_unique<juce::FileChooser>("Add a BFD library folder", juce::File(), "");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this](const juce::FileChooser& fc) {
                              const auto dir = fc.getResult();
                              if (!dir.isDirectory()) return;
                              auto roots = host_.processor().libraryRoots();
                              roots.addIfNotAlreadyThere(dir.getFullPathName());
                              host_.processor().scanLibrariesAsync(roots);
                          });
}

void Browser::refresh() {
    auto& proc = host_.processor();
    libraryCount_ = int(proc.libraries().size());
    pieceCount_ = 0;
    for (const auto& lib : proc.libraries()) pieceCount_ += int(lib.pieces.size());
    search_.setTextToShowWhenEmpty("SEARCH", pal().textFaint);
    rebuildRows();
    repaint();
}

void Browser::rebuildRows() {
    auto& proc = host_.processor();
    const auto query = search_.getText().trim().toLowerCase();
    auto matches = [&](const juce::String& a, const juce::String& b) {
        return query.isEmpty() || a.toLowerCase().contains(query) || b.toLowerCase().contains(query);
    };
    std::vector<Row> rows;
    const int tab = tabs_.selected();
    if (tab < 2) {
        const bool presets = tab == 1;
        const auto loaded = proc.loadedPath();
        std::map<juce::String, std::vector<const bfdmodel::KitFileEntry*>> byLib;
        for (const auto& e : proc.kitFiles())
            if (e.preset == presets && matches(e.name, e.library)) byLib[juce::String(e.library)].push_back(&e);
        for (auto& [lib, entries] : byLib) {
            std::sort(entries.begin(), entries.end(), [](auto* a, auto* b) {
                return juce::String(a->name).compareNatural(juce::String(b->name)) < 0;
            });
            Row h;
            h.header = true;
            h.text = lib;
            h.right = juce::String(entries.size());
            rows.push_back(h);
            for (auto* e : entries) {
                Row r;
                r.text = e->name;
                r.sub = lib;
                r.right = juce::File(e->path).getFileExtension().substring(1).toUpperCase();
                r.payload = e->path;
                r.loaded = loaded == juce::String(e->path);
                rows.push_back(r);
            }
        }
    } else {
        struct Item {
            Row row;
            float diameter;
        };
        std::map<int, std::vector<Item>> byKind;
        for (const auto& lib : proc.libraries())
            for (const auto& piece : lib.pieces) {
                const auto kind = bfdmodel::pieceKind(*piece);
                const juce::String name(piece->name);
                if (!matches(name, libraryLabel(lib) + " " + bfdmodel::pieceKindCode(kind))) continue;
                Item it;
                it.diameter = bfdmodel::pieceDiameter(*piece);
                it.row.text = name;
                it.row.sub = libraryLabel(lib) + (piece->dimensions.empty() ? "" : "  " + juce::String(piece->dimensions));
                it.row.right = piece->diameter > 0.0f ? inches(piece->diameter) : juce::String();
                it.row.payload = bfdmodel::pieceSlotKey(lib, *piece);
                it.row.kind = int(kind);
                byKind[int(kind)].push_back(std::move(it));
            }
        static const char* names[] = {"Kick", "Snare", "Hihat", "Tom", "Floor tom", "Crash", "Ride", "Splash", "China", "Perc"};
        for (auto& [kind, items] : byKind) {
            std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
                if (std::abs(a.diameter - b.diameter) > 0.01f) return a.diameter < b.diameter;
                return a.row.text.compareNatural(b.row.text) < 0;
            });
            Row h;
            h.header = true;
            h.text = names[kind];
            h.right = juce::String(items.size());
            rows.push_back(h);
            for (auto& it : items) rows.push_back(std::move(it.row));
        }
    }
    list_->rows = std::move(rows);
    list_->setSize(viewport_.getMaximumVisibleWidth(), 1);
    list_->layout();
}

void Browser::resized() {
    auto r = getLocalBounds();
    auto head = r.removeFromTop(40);
    addFolder_.setBounds(head.removeFromRight(14 + 74).withTrimmedRight(14).withSizeKeepingCentre(74, 22));
    r.removeFromTop(2);
    tabs_.setBounds(r.removeFromTop(22).reduced(14, 0));
    r.removeFromTop(8);
    search_.setBounds(r.removeFromTop(24).reduced(14, 0));
    r.removeFromTop(8);
    r.removeFromBottom(30); // library readout
    viewport_.setBounds(r);
    list_->setSize(viewport_.getMaximumVisibleWidth(), list_->getHeight());
}

void Browser::paint(juce::Graphics& g) {
    const auto& p = pal();
    g.fillAll(p.panel);
    draw::bracketTitle(g, juce::Rectangle<int>(14, 0, getWidth() - 120, 40), "Library");
    // footer readout: libraries / pieces, or the scan in progress
    auto foot = getLocalBounds().removeFromBottom(30);
    g.setColour(p.rule);
    g.fillRect(foot.withHeight(1));
    const auto& proc = host_.processor();
    if (proc.scanning()) {
        draw::statusChip(g, foot.reduced(14, 5).withWidth(108), "Scanning", false, true);
    } else {
        g.setFont(Theme::get().mono(9.5f));
        g.setColour(p.textDim);
        g.drawText(juce::String(libraryCount_) + " LIB  " + juce::String(pieceCount_) + " PCS",
                   foot.reduced(14, 0), juce::Justification::centredLeft, false);
    }
}

void Browser::paintOverChildren(juce::Graphics& g) {
    if (!list_->rows.empty()) return;
    const auto& p = pal();
    auto r = viewport_.getBounds().withHeight(80);
    g.setFont(Theme::get().mono(11.0f));
    g.setColour(p.textDim);
    const bool scanning = host_.processor().scanning();
    const juce::String a = scanning ? "SCANNING" : search_.isEmpty() ? "NO LIBRARIES" : "NO MATCH";
    const juce::String b = scanning ? "" : search_.isEmpty() ? "ADD ONE WITH + FOLDER" : "";
    g.drawText(a, r.removeFromTop(48).withTrimmedTop(24), juce::Justification::centred, false);
    g.drawText(b, r.removeFromTop(18), juce::Justification::centred, false);
}

} // namespace openbfd::ui
