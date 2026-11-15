#include "LibraryPanel.h"
#include "Language.h"
#include <algorithm>
#include <cmath>
#include <map>
#include "Theme.h"
#include "sample_analysis.hpp"

namespace core::app {

using namespace theme;
using doc::json;

namespace {

juce::String u8(const std::string& t) { return juce::String::fromUTF8(t.c_str()); }

constexpr int kHeader = 34, kRowHeight = 22, kPreview = 152, kSoundRow = 18 + 26;

// The groups of the tree and the role chips: which sample roles each one holds.
struct Category { const char* label; const char* chip; std::vector<std::string> roles; };
const std::vector<Category>& categories() {
  static const std::vector<Category> list = {
      {"Kicks", "Kick", {"kick"}},
      {"Bass", "Bass", {"bass"}},
      {"Hats", "Hats", {"hat", "openhat"}},
      {"Percussion", "Perc", {"snare", "clap", "perc", "tom"}},
      {"Lead", "Lead", {"synth"}},
      {"FX", "FX", {"fx", "riser", "impact", "crash", "fill"}},
      {"Pads & vox", "Pads", {"pad", "vox", "orchestral"}},
      {"Other", nullptr, {"other"}},
  };
  return list;
}

int categoryOf(const std::string& role) {
  const auto& list = categories();
  for (size_t i = 0; i < list.size(); ++i)
    if (std::find(list[i].roles.begin(), list[i].roles.end(), role) != list[i].roles.end()) return (int)i;
  return (int)list.size() - 1;
}

// What a sample is, in one line: "kick · one-shot · 175 BPM · F#m · punchy".
juce::String describe(const doc::SampleInfo& i) {
  const json f = doc::sampleInfoJson(i);
  juce::StringArray parts{u8(i.role), i.loop ? "loop" : "one-shot"};
  if (i.bpm > 0) parts.add(juce::String(i.bpm, i.bpm == std::floor(i.bpm) ? 0 : 1) + " BPM");
  if (const std::string k = f["key"]; !k.empty()) parts.add(u8(k));
  for (const auto& w : i.styles) parts.add(u8(w));
  for (const auto& w : i.tags) parts.add(u8(w));
  return parts.joinIntoString(juce::String::fromUTF8(" \xc2\xb7 "));
}

juce::String originText(const std::string& o) {
  return o == "user" ? "yours" : o == "metadata file" ? "metadata file" : o == "file" ? "from the file's tags" : o == "name" ? "from the name" : "not known";
}

std::vector<std::string> splitWords(const juce::String& text) {
  std::vector<std::string> out;
  for (const auto& w : juce::StringArray::fromTokens(text, ",", "\"")) {
    const std::string t = w.trim().toLowerCase().toStdString();
    if (!t.empty()) out.push_back(t);
  }
  return out;
}

void styleTab(juce::TextButton& b, const juce::String& text) {
  b.setButtonText(text);
  b.setClickingTogglesState(false);
  b.setWantsKeyboardFocus(false);
}

} // namespace

LibraryPanel::LibraryPanel() {
  minimise_.getProperties().set(theme::kIconProperty, "left");
  minimise_.setTooltip("Minimise the Library (the rail on the left brings it back)");
  minimise_.setWantsKeyboardFocus(false);
  minimise_.onClick = [this] { if (onMinimise) onMinimise(); };
  addAndMakeVisible(minimise_);
  search_.setTextToShowWhenEmpty("Search sounds", kDim);
  search_.setTooltip(u8("Search by name, role (kick, bass...), style or key"));
  search_.setIndents(26, 6);
  search_.onTextChange = [this] { rebuild(); };
  search_.onEscapeKey = [this] { search_.clear(); rebuild(); };
  addAndMakeVisible(search_);

  styleTab(sounds_, "Sounds");
  styleTab(presetsTab_, "Templates");
  styleTab(grooves_, "Patterns");
  styleTab(projectTab_, "Project");
  projectTab_.setTooltip("This song's audio files: imported, or made by Core (reversed, stretched...); the unused ones can go");
  projectTab_.onClick = [this] { setTab(Tab::Project); };
  addAndMakeVisible(projectTab_);
  removeUnused_.setButtonText("Remove unused");
  removeUnused_.setWantsKeyboardFocus(false);
  removeUnused_.setTooltip("Takes the files no clip plays out of the song (Undo puts them back in the song) and sends them to the Recycle Bin");
  removeUnused_.onClick = [this] { if (onRemoveUnused) onRemoveUnused(); };
  addChildComponent(removeUnused_);
  sounds_.setTooltip("Samples of your library (Samples > Choose the library folder)");
  presetsTab_.setTooltip("Song templates, by style");
  grooves_.setTooltip("Patterns (KBBB, hats, cymbals, build-ups...) for any track: hear one on the selected track, then Apply");
  sounds_.onClick = [this] { setTab(Tab::Sounds); };
  presetsTab_.onClick = [this] { setTab(Tab::Templates); };
  grooves_.onClick = [this] { setTab(Tab::Grooves); };
  for (auto* b : {&sounds_, &presetsTab_, &grooves_}) addAndMakeVisible(b);

  // Templates tab: the chosen template's card.
  newSong_.setWantsKeyboardFocus(false);
  newSong_.setTooltip("A new song from this template (the song you have now closes: Core asks to save it first)");
  newSong_.onClick = [this] { if (const auto* p = currentPreset(); p && onOpenPreset) onOpenPreset(p->id); };
  saveTemplate_.setButtonText(juce::String(juce::CharPointer_UTF8("Save this song as template\xe2\x80\xa6")));
  saveTemplate_.setWantsKeyboardFocus(false);
  saveTemplate_.setTooltip("The song as it is now, with its sounds, becomes one of your templates (same name as one: replaces it)");
  saveTemplate_.onClick = [this] { if (onSaveTemplate) onSaveTemplate(); };
  deleteTemplate_.getProperties().set(kIconProperty, "erase");
  deleteTemplate_.setWantsKeyboardFocus(false);
  deleteTemplate_.setTooltip("Delete this template (Core asks first)");
  deleteTemplate_.onClick = [this] {
    if (const auto* p = currentPreset(); p && p->id != kBasicTemplate && onDeleteTemplate) onDeleteTemplate(p->id);
  };
  templatesInfo_.setTooltip("Templates are starting songs, grouped by style. Click one to choose it, double-click or "
                            "\"New song\" to start from it. Right click: delete, bring back the built-in ones you deleted, "
                            "or open your templates folder. \"From a song\": made by AICORE from a song it analysed. "
                            "Templates without a style are under \"No style\".");
  for (juce::Component* c : {(juce::Component*)&newSong_, (juce::Component*)&saveTemplate_, (juce::Component*)&deleteTemplate_,
                             (juce::Component*)&templatesInfo_})
    addChildComponent(c);

  for (const auto& c : categories()) {
    if (!c.chip) continue;
    auto b = std::make_unique<juce::TextButton>(c.chip);
    b->setClickingTogglesState(true);
    b->setWantsKeyboardFocus(false);
    b->setTooltip("Only these sounds (several can be on)");
    b->onClick = [this] { rebuild(); };
    addAndMakeVisible(*b);
    roles_.push_back(std::move(b));
  }

  for (const char* w : {"short", "long", "bright", "dark", "sub"}) {
    auto b = std::make_unique<juce::TextButton>(w);
    b->setClickingTogglesState(true);
    b->setWantsKeyboardFocus(false);
    b->setTooltip("Only sounds measured as this (short: under 0.3 s, long: over 2 s, bright / dark: where the spectrum sits, "
                  "sub: much under 120 Hz). Sounds not measured yet are left out while one is on.");
    b->onClick = [this] { rebuild(); };
    addAndMakeVisible(*b);
    soundChips_.push_back(std::move(b));
  }
  fit_.setClickingTogglesState(true);
  fit_.setToggleState(true, juce::dontSendNotification);
  fit_.setWantsKeyboardFocus(false);
  fit_.onClick = [this] { rebuild(); repaint(previewArea()); };
  addAndMakeVisible(fit_);
  list_.setRowHeight(kRowHeight);
  list_.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
  list_.setColour(juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
  addAndMakeVisible(list_);

  play_.getProperties().set(kIconProperty, "play");
  play_.setWantsKeyboardFocus(false);
  play_.onClick = [this] { preview(); };
  addAndMakeVisible(play_);
  use_.setWantsKeyboardFocus(false);
  use_.onClick = [this] { if (const auto* s = current(); s && onUseSample) onUseSample(*s); };
  addAndMakeVisible(use_);
  star_.getProperties().set(kIconProperty, "star");
  star_.setWantsKeyboardFocus(false);
  star_.setTooltip("Favourite");
  star_.onClick = [this] { if (const auto* s = current()) toggleFavourite(s->sha256); };
  addAndMakeVisible(star_);
  edit_.getProperties().set(kIconProperty, "pencil");
  edit_.setWantsKeyboardFocus(false);
  edit_.setTooltip("What this sound is: role, one-shot or loop, BPM, key, styles, tags (Core keeps it; the file is not changed)");
  edit_.onClick = [this] { editMetadata(); };
  addAndMakeVisible(edit_);
  const int barChoices[4] = {1, 4, 8, 16};
  for (int i = 0; i < 4; ++i) {
    const int b = barChoices[i];
    bars_[i].setButtonText(juce::String(b));
    bars_[i].setTooltip("Hear and apply over " + juce::String(b) + (b == 1 ? " bar" : " bars"));
    bars_[i].setClickingTogglesState(false);
    bars_[i].setWantsKeyboardFocus(false);
    bars_[i].onClick = [this, b] {
      previewBars_ = b;
      for (int j = 0; j < 4; ++j) bars_[j].setToggleState(bars_[j].getButtonText().getIntValue() == b, juce::dontSendNotification);
    };
    bars_[i].setToggleState(b == previewBars_, juce::dontSendNotification);
    addChildComponent(bars_[i]);
  }
  playPattern_.getProperties().set(kIconProperty, "play");
  playPattern_.setWantsKeyboardFocus(false);
  playPattern_.onClick = [this] { if (const auto* p = currentPatternPreset(); p && onPreviewPattern) onPreviewPattern(*p, previewBars_); };
  addChildComponent(playPattern_);
  applyPattern_.setButtonText("Apply");
  applyPattern_.setWantsKeyboardFocus(false);
  applyPattern_.setTooltip("Put it on the selected track from the first bar shown in the piano roll, over the bars chosen (the notes there go; Ctrl+Z brings them back)");
  applyPattern_.onClick = [this] { if (const auto* p = currentPatternPreset(); p && onApplyPattern) onApplyPattern(*p, previewBars_, false); };
  addChildComponent(applyPattern_);
  addPattern_.setButtonText("Add");
  addPattern_.setWantsKeyboardFocus(false);
  addPattern_.setTooltip("As Apply, but the notes already there stay (layered)");
  addPattern_.onClick = [this] { if (const auto* p = currentPatternPreset(); p && onApplyPattern) onApplyPattern(*p, previewBars_, true); };
  addChildComponent(addPattern_);
  setTab(Tab::Sounds);
}

LibraryPanel::~LibraryPanel() { list_.setModel(nullptr); }

void LibraryPanel::setTab(Tab tab) {
  tab_ = tab;
  sounds_.setToggleState(tab == Tab::Sounds, juce::dontSendNotification);
  presetsTab_.setToggleState(tab == Tab::Templates, juce::dontSendNotification);
  grooves_.setToggleState(tab == Tab::Grooves, juce::dontSendNotification);
  projectTab_.setToggleState(tab == Tab::Project, juce::dontSendNotification);
  removeUnused_.setVisible(tab == Tab::Project);
  for (juce::Component* c : {(juce::Component*)&newSong_, (juce::Component*)&saveTemplate_, (juce::Component*)&deleteTemplate_,
                             (juce::Component*)&templatesInfo_})
    c->setVisible(tab == Tab::Templates);
  projectSeen_.clear();
  const bool sounds = tab == Tab::Sounds;
  for (auto& b : roles_) b->setVisible(sounds);
  for (auto& b : soundChips_) b->setVisible(sounds);
  fit_.setVisible(sounds);
  search_.setVisible(tab != Tab::Project);
  play_.setVisible(sounds);
  use_.setVisible(sounds);
  star_.setVisible(sounds);
  edit_.setVisible(sounds);
  for (auto& b : bars_) b.setVisible(tab == Tab::Grooves);
  for (auto* b : {&playPattern_, &applyPattern_, &addPattern_}) b->setVisible(tab == Tab::Grooves);
  if (tab != Tab::Grooves && onStopPreview) onStopPreview();
  if (tab == Tab::Templates && presets) presetList_ = presets();
  if (tab == Tab::Grooves && patterns) patternList_ = patterns();
  search_.setTextToShowWhenEmpty(tab == Tab::Templates ? "Search templates" : tab == Tab::Grooves ? "Search patterns (kbbb, hats, build...)" : "Search sounds", kDim);
  refresh();
  rebuild();
  resized();
  repaint();
}

void LibraryPanel::templatesChanged() {
  if (tab_ != Tab::Templates) return;  // read again when the tab opens
  if (presets) presetList_ = presets();
  if (!currentPreset()) currentTemplate_.clear();
  rebuild();
  resized();
}

void LibraryPanel::showTemplates() { setTab(Tab::Templates); }

const LibraryPanel::Preset* LibraryPanel::currentPreset() const {
  for (const auto& p : presetList_)
    if (p.id == currentTemplate_) return &p;
  return nullptr;
}

juce::String LibraryPanel::groupKey(const Row& r) const { return (tab_ == Tab::Templates ? "template:" : "") + r.label; }

void LibraryPanel::indexChanged() {
  if (loadFavourites) favourites_ = loadFavourites();
  currentSample_ = -1;
  rebuild();
  refresh();
}

std::string LibraryPanel::projectSignature() const {
  const doc::Document* d = document ? document() : nullptr;
  if (!d) return "-";
  const auto dir = projectFolder ? projectFolder() : std::filesystem::path();
  const auto u = dir.u8string();
  std::string sig(u.begin(), u.end());
  for (const auto& a : d->assets) sig += "|" + a.id + ":" + a.name + ":" + a.made;
  for (const auto& t : d->tracks) for (const auto& c : t.audioClips) sig += "|" + c.assetId;
  return sig;
}

void LibraryPanel::poll() {
  if (tab_ == Tab::Sounds && isShowing()) {  // another track or tempo: the "fits" filter follows
    const auto [role, bpm] = fitNow();
    const std::string key = role + "|" + std::to_string(bpm);
    if (key != fitSeen_) {
      rebuild();
      refresh();
    }
    return;
  }
  if (tab_ != Tab::Project || !isShowing()) return;
  const std::string sig = projectSignature();
  if (sig == projectSeen_) return;
  projectSeen_ = sig;
  rebuild();
}

void LibraryPanel::catalogChanged() {
  rebuild();
  refresh();
}

const doc::LibrarySample* LibraryPanel::sample(int i) const {
  const doc::SampleIndex* idx = index ? index() : nullptr;
  return idx && i >= 0 && i < (int)idx->samples.size() ? &idx->samples[(size_t)i] : nullptr;
}

// The tree: Favourites first, then one group per kind of sound, each with its samples (name order).
void LibraryPanel::rebuild() {
  rows_.clear();
  const juce::String query = search_.getText().trim().toLowerCase();
  if (tab_ == Tab::Templates) {
    // One group per style, in the order the styles first appear (Core's basic song under "Basic"), as the sounds.
    juce::StringArray styles;
    for (const auto& p : presetList_) styles.addIfNotAlreadyThere(p.style.empty() ? juce::String("No style") : u8(p.style));
    for (const auto& style : styles) {
      std::vector<int> members;
      for (size_t i = 0; i < presetList_.size(); ++i) {
        const auto& p = presetList_[i];
        if ((p.style.empty() ? juce::String("No style") : u8(p.style)) != style) continue;
        if (query.isNotEmpty() && !(u8(p.name) + " " + style).toLowerCase().contains(query)) continue;
        members.push_back((int)i);
      }
      if (members.empty()) continue;
      Row g;
      g.group = true;
      g.label = style;
      g.count = (int)members.size();
      rows_.push_back(g);
      if (collapsed_.count(groupKey(g).toStdString()) && query.isEmpty()) continue;
      for (int i : members) {
        Row r;
        r.label = u8(presetList_[(size_t)i].name);
        r.preset = i;
        rows_.push_back(r);
      }
    }
  } else if (tab_ == Tab::Grooves) {
    // Diogo's own first, then by role; any pattern fits any track (the role only sorts them).
    static const std::vector<std::pair<std::string, juce::String>> order = {
        {"mine", "My patterns"}, {"bass", "Bass"}, {"kick", "Kick"}, {"hats", "Hats"}, {"cymbals", "Cymbals"},
        {"perc", "Perc"}, {"build", "Build-ups"}, {"lead", "Leads & arps"}, {"", "Other"}};
    std::vector<std::vector<int>> groups(order.size());
    for (size_t i = 0; i < patternList_.size(); ++i) {
      const auto& p = patternList_[i];
      if (query.isNotEmpty()) {
        juce::String text = u8(p.name) + " " + u8(p.role) + " " + u8(p.id) + " " + u8(p.description);
        for (const auto& w : p.styles) text << " " << u8(w);
        for (const auto& w : p.tags) text << " " << u8(w);
        text = text.toLowerCase();
        bool all = true;
        for (const auto& w : juce::StringArray::fromTokens(query, " ", "")) all = all && (w.isEmpty() || text.contains(w));
        if (!all) continue;
      }
      size_t g = order.size() - 1;
      if (p.origin == "user") g = 0;
      else for (size_t k = 1; k + 1 < order.size(); ++k) if (order[k].first == p.role) g = k;
      groups[g].push_back((int)i);
    }
    for (size_t k = 0; k < order.size(); ++k) {
      if (groups[k].empty()) continue;
      Row g;
      g.group = true;
      g.label = order[k].second;
      g.count = (int)groups[k].size();
      rows_.push_back(g);
      if (collapsed_.count(g.label.toStdString())) continue;
      for (int i : groups[k]) {
        Row r;
        r.label = u8(patternList_[(size_t)i].name);
        r.pattern = i;
        rows_.push_back(r);
      }
    }
  } else if (tab_ == Tab::Project) {
    const doc::Document* d = document ? document() : nullptr;
    const std::filesystem::path dir = projectFolder ? projectFolder() : std::filesystem::path();
    unusedCount_ = 0;
    if (d) {
      std::vector<Row> used, unused, stray;
      for (const auto& a : d->assets) {
        Row r;
        r.asset = a.id;
        r.label = u8(a.name) + (a.made.empty() ? juce::String() : juce::String::fromUTF8(" \xc2\xb7 ") + u8(a.made));
        (doc::clipsUsingAsset(*d, a.id) > 0 ? used : unused).push_back(r);
      }
      for (const auto& f : doc::strayAudioFiles(*d, dir)) {
        Row r;
        const auto u = f.u8string();
        r.file = std::string(u.begin(), u.end());
        r.label = juce::File(juce::String::fromUTF8(r.file.c_str())).getFileName();
        stray.push_back(r);
      }
      unusedCount_ = (int)(unused.size() + stray.size());
      auto addGroup = [&](const juce::String& label, const std::vector<Row>& members) {
        if (members.empty()) return;
        Row g;
        g.group = true;
        g.label = label;
        g.count = (int)members.size();
        rows_.push_back(g);
        if (collapsed_.count(label.toStdString())) return;
        for (const auto& r : members) rows_.push_back(r);
      };
      addGroup("In the song", used);
      addGroup("Not used", unused);
      addGroup("Files the song does not know", stray);
    }
    removeUnused_.setEnabled(unusedCount_ > 0 && onRemoveUnused != nullptr);
    removeUnused_.setButtonText(unusedCount_ > 0 ? "Remove " + juce::String(unusedCount_) + " unused" : juce::String("Nothing unused"));
  } else if (tab_ == Tab::Sounds) {
    if (const doc::SampleIndex* idx = index ? index() : nullptr) {
      std::set<int> chips;  // categories switched on
      int chip = 0;
      for (size_t c = 0; c < categories().size(); ++c)
        if (categories()[c].chip) { if (roles_[(size_t)chip]->getToggleState()) chips.insert((int)c); ++chip; }
      std::vector<std::vector<int>> groups(categories().size());
      std::vector<int> favourites;
      const auto [fitRole, fitBpm] = fitNow();
      fitSeen_ = fitRole + "|" + std::to_string(fitBpm);
      const bool fitOn = fit_.getToggleState() && !fitRole.empty();
      for (size_t i = 0; i < idx->samples.size(); ++i) {
        const auto& s = idx->samples[i];
        const int c = categoryOf(s.info.role);
        if (!chips.empty() && !chips.count(c)) continue;
        if (fitOn && !doc::sampleFit(s, fitRole, fitBpm).fits) continue;
        {
          const auto measured = doc::soundWords(s);
          bool fits = true;
          for (const auto& b : soundChips_)
            if (b->getToggleState()) fits = fits && std::find(measured.begin(), measured.end(), b->getButtonText().toStdString()) != measured.end();
          if (!fits) continue;
        }
        if (query.isNotEmpty()) {  // every word, in its paths, role, styles or tags
          juce::String text = u8(s.path) + " " + u8(s.displayName) + " " + u8(s.info.role);
          for (const auto& o : s.otherPaths) text << " " << u8(o);
          for (const auto& w : s.info.styles) text << " " << u8(w);
          for (const auto& w : s.info.tags) text << " " << u8(w);
          for (const auto& w : doc::soundWords(s)) text << " " << u8(w);
          text = text.toLowerCase();
          bool all = true;
          for (const auto& w : juce::StringArray::fromTokens(query, " ", "")) all = all && (w.isEmpty() || text.contains(w));
          if (!all) continue;
        }
        groups[(size_t)c].push_back((int)i);
        if (favourites_.count(s.sha256)) favourites.push_back((int)i);
      }
      auto addGroup = [&](const juce::String& label, const std::vector<int>& members) {
        if (members.empty()) return;
        Row g;
        g.group = true;
        g.label = label;
        g.count = (int)members.size();
        rows_.push_back(g);
        if (collapsed_.count(label.toStdString())) return;
        std::vector<int> sorted = members;  // by the name Core shows (then by path: stable)
        std::stable_sort(sorted.begin(), sorted.end(), [&](int a, int b) {
          return juce::String(u8(idx->samples[(size_t)a].displayName)).compareNatural(u8(idx->samples[(size_t)b].displayName)) < 0;
        });
        for (int i : sorted) {
          const auto& x = idx->samples[(size_t)i];
          Row r;
          r.label = u8(x.displayName.empty() ? x.name : x.displayName);
          r.sample = i;
          rows_.push_back(r);
        }
      };
      addGroup("Favourites", favourites);
      for (size_t c = 0; c < categories().size(); ++c) addGroup(categories()[c].label, groups[c]);
    }
  }
  list_.updateContent();
  int selectedRow = -1;
  for (size_t i = 0; i < rows_.size(); ++i)
    if (!rows_[i].group && ((rows_[i].sample >= 0 && rows_[i].sample == currentSample_) ||
                            (rows_[i].preset >= 0 && presetList_[(size_t)rows_[i].preset].id == currentTemplate_) ||
                            (rows_[i].pattern >= 0 && rows_[i].pattern == currentPattern_) ||
                            (tab_ == Tab::Project && !currentProjectItem_.empty() &&
                             (rows_[i].asset == currentProjectItem_ || rows_[i].file == currentProjectItem_)))) { selectedRow = (int)i; break; }
  if (selectedRow >= 0) list_.selectRow(selectedRow, true, true);
  else list_.deselectAllRows();
  repaint();
}

void LibraryPanel::refresh() {
  const auto* s = current();
  const juce::String track = trackName ? trackName() : juce::String();
  const bool fits = s && track.isNotEmpty() && trackTakesSamples && trackTakesSamples();
  play_.setEnabled(s != nullptr && onPreviewSample != nullptr);
  play_.setTooltip(s ? juce::String("Hear it (the song does not change)") : juce::String("Choose a sound"));
  use_.setEnabled(fits);
  use_.setTooltip(!s ? juce::String("Choose a sound")
                  : track.isEmpty() ? juce::String("Select a track first: the sound goes on it")
                  : !fits ? "\"" + track + "\" plays a synth: choose a drum, bass or FX track for samples"
                          : "Put on \"" + track + "\" and play it (Ctrl+Z puts the old sound back)");
  const auto [fitRole, fitBpm] = fitNow();
  fit_.setEnabled(!fitRole.empty());
  fit_.setTooltip(fitRole.empty() ? juce::String("Select a drum, bass or FX track: then only the sounds that suit it at the song's tempo show")
                                  : "Only sounds that suit \"" + track + "\" at " + juce::String(fitBpm, fitBpm == std::round(fitBpm) ? 0 : 1) +
                                        " BPM (a kick that ends before the bass, a loop at the tempo...). Off: every sound.");
  star_.getProperties().set(kIconProperty, s && favourites_.count(s->sha256) ? "starOn" : "star");
  star_.setEnabled(s != nullptr);
  star_.repaint();
  edit_.setEnabled(s != nullptr && onEditMetadata != nullptr);
  const doc::PatternPreset* p = currentPatternPreset();
  for (auto* b : {&playPattern_, &applyPattern_, &addPattern_}) b->setEnabled(p != nullptr && track.isNotEmpty());
  playPattern_.setTooltip(!p ? juce::String("Choose a pattern") : track.isEmpty() ? juce::String("Select a track first: the pattern plays on it")
                                                                                   : "Hear it on \"" + track + "\" (the song does not change)");
  repaint(previewArea());
}

void LibraryPanel::toggleFavourite(const std::string& sha) {
  if (!favourites_.erase(sha)) favourites_.insert(sha);
  if (saveFavourites) saveFavourites(favourites_);
  rebuild();
  refresh();
}

int LibraryPanel::getNumRows() { return (int)rows_.size(); }

void LibraryPanel::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) {
  if (row < 0 || row >= (int)rows_.size()) return;
  const Row& r = rows_[(size_t)row];
  auto area = juce::Rectangle<int>(width, height).reduced(2, 1);
  if (selected && !r.group) {
    g.setColour(kAccent.withAlpha(0.85f));
    g.fillRoundedRectangle(area.toFloat(), theme::kCorner);
  }
  g.setColour(selected && !r.group ? juce::Colours::white : r.group ? kText : kText.withAlpha(0.85f));
  g.setFont(juce::FontOptions(theme::kFontBody));
  if (r.group) {
    const bool open = !collapsed_.count(groupKey(r).toStdString());
    drawIcon(g, open ? "down" : "right", area.removeFromLeft(16).toFloat());
    g.setColour(kMuted);
    drawIcon(g, r.label == "Favourites" ? "starOn" : "folder", area.removeFromLeft(20).toFloat());
    g.setColour(kText);
    g.drawText(r.label, area.withTrimmedLeft(4), juce::Justification::centredLeft, true);
    g.setColour(kDim);
    g.setFont(juce::FontOptions(theme::kFontSmall));
    g.drawText(juce::String(r.count), area.withTrimmedRight(6), juce::Justification::centredRight);
    return;
  }
  if (r.pattern >= 0) {
    area.removeFromLeft(18);
    drawIcon(g, "pencil", area.removeFromLeft(18).toFloat().reduced(2));
    g.drawText(r.label, area.withTrimmedLeft(5), juce::Justification::centredLeft, true);
    return;
  }
  if (r.preset >= 0) {
    const auto& p = presetList_[(size_t)r.preset];
    area.removeFromLeft(18);
    drawIcon(g, "arrange", area.removeFromLeft(18).toFloat());
    if (p.bpm > 0) {  // the tempo on the right, as the sounds' star
      g.setFont(juce::FontOptions(theme::kFontSmall));
      g.setColour(selected ? juce::Colours::white.withAlpha(0.8f) : kDim);
      g.drawText(juce::String(p.bpm, p.bpm == std::round(p.bpm) ? 0 : 1), area.removeFromRight(40).withTrimmedRight(6),
                 juce::Justification::centredRight);
      g.setFont(juce::FontOptions(theme::kFontBody));
      g.setColour(selected ? juce::Colours::white : kText.withAlpha(0.85f));
    }
    g.drawText(r.label, area.withTrimmedLeft(5), juce::Justification::centredLeft, true);
    return;
  }
  area.removeFromLeft(18);
  drawIcon(g, "wave", area.removeFromLeft(18).toFloat());
  if (const auto* s = sample(r.sample); s && favourites_.count(s->sha256))
    drawIcon(g, "starOn", area.removeFromRight(18).toFloat().reduced(2));
  g.drawText(r.label, area.withTrimmedLeft(5), juce::Justification::centredLeft, true);
}

void LibraryPanel::listBoxItemClicked(int row, const juce::MouseEvent& e) {
  if (row < 0 || row >= (int)rows_.size()) return;
  const Row r = rows_[(size_t)row];
  if (e.mods.isPopupMenu() && !r.group && (r.sample >= 0 || !r.asset.empty())) {
    juce::PopupMenu menu;
    const doc::LibrarySample* s = r.sample >= 0 ? sample(r.sample) : nullptr;
    if (s) {
      menu.addItem(juce::PopupMenu::Item("Edit a copy (on a new track)").setID(1).setEnabled(onEditSample != nullptr));
    } else {
      menu.addItem(juce::PopupMenu::Item("Put on a new track").setID(2).setEnabled(onAssetToTrack != nullptr));
      menu.addItem(juce::PopupMenu::Item(juce::String(juce::CharPointer_UTF8("Save as sample\xe2\x80\xa6"))).setID(3).setEnabled(onSaveAsset != nullptr));
    }
    const std::string sha = s ? s->sha256 : std::string(), asset = r.asset;
    juce::Component::SafePointer<LibraryPanel> self(this);
    menu.showMenuAsync(juce::PopupMenu::Options(), [self, sha, asset](int chosen) {
      if (self == nullptr) return;
      const doc::SampleIndex* idx = self->index ? self->index() : nullptr;
      if (chosen == 1 && idx && self->onEditSample)
        if (const doc::LibrarySample* x = idx->findHash(sha)) self->onEditSample(*x);
      if (chosen == 2 && self->onAssetToTrack) self->onAssetToTrack(asset);
      if (chosen == 3 && self->onSaveAsset) self->onSaveAsset(asset);
    });
    return;
  }
  if (e.mods.isPopupMenu() && tab_ == Tab::Templates) {
    if (r.preset >= 0) {
      currentTemplate_ = presetList_[(size_t)r.preset].id;
      list_.selectRow(row, true, true);
      repaint(previewArea());
    }
    templateMenu(r.preset >= 0 ? &presetList_[(size_t)r.preset] : nullptr);
    return;
  }
  if (r.group) {
    const std::string key = groupKey(r).toStdString();
    if (!collapsed_.erase(key)) collapsed_.insert(key);
    rebuild();
    return;
  }
  // A click chooses a template (its card shows it); a double-click or "New song" starts from it.
}

// Right click in the Templates tab: what the File menu used to hold.
void LibraryPanel::templateMenu(const Preset* p) {
  juce::PopupMenu menu;
  const std::string id = p ? p->id : std::string();
  if (p) {
    menu.addItem(juce::PopupMenu::Item("New song from \"" + u8(p->name) + "\"").setID(1).setEnabled(onOpenPreset != nullptr));
    menu.addItem(juce::PopupMenu::Item(juce::String(juce::CharPointer_UTF8("Delete template\xe2\x80\xa6"))).setID(2)
                     .setEnabled(onDeleteTemplate != nullptr && id != kBasicTemplate));
    menu.addSeparator();
  }
  menu.addItem(juce::PopupMenu::Item(juce::String(juce::CharPointer_UTF8("Save this song as template\xe2\x80\xa6"))).setID(3).setEnabled(onSaveTemplate != nullptr));
  menu.addItem(juce::PopupMenu::Item("Bring back the built-in templates").setID(4).setEnabled(onRestoreTemplates != nullptr));
  menu.addItem(juce::PopupMenu::Item("Open my templates folder").setID(5).setEnabled(onOpenTemplatesFolder != nullptr));
  juce::Component::SafePointer<LibraryPanel> self(this);
  menu.showMenuAsync(juce::PopupMenu::Options(), [self, id](int chosen) {
    if (self == nullptr) return;
    if (chosen == 1 && self->onOpenPreset) self->onOpenPreset(id);
    if (chosen == 2 && self->onDeleteTemplate) self->onDeleteTemplate(id);
    if (chosen == 3 && self->onSaveTemplate) self->onSaveTemplate();
    if (chosen == 4 && self->onRestoreTemplates) self->onRestoreTemplates();
    if (chosen == 5 && self->onOpenTemplatesFolder) self->onOpenTemplatesFolder();
  });
}

juce::var LibraryPanel::getDragSourceDescription(const juce::SparseSet<int>& rows) {
  if (tab_ != Tab::Sounds || rows.isEmpty()) return {};
  const int row = rows[0];
  if (row < 0 || row >= (int)rows_.size() || rows_[(size_t)row].group) return {};
  const doc::LibrarySample* s = sample(rows_[(size_t)row].sample);
  return s ? juce::var(juce::String(kSampleDrag) + juce::String(s->sha256)) : juce::var();
}

void LibraryPanel::listBoxItemDoubleClicked(int row, const juce::MouseEvent&) {
  if (row >= 0 && row < (int)rows_.size() && rows_[(size_t)row].preset >= 0) {
    if (onOpenPreset) onOpenPreset(presetList_[(size_t)rows_[(size_t)row].preset].id);
    return;
  }
  if (row >= 0 && row < (int)rows_.size() && rows_[(size_t)row].pattern >= 0) {
    currentPattern_ = rows_[(size_t)row].pattern;
    playPattern_.triggerClick();
    return;
  }
  if (row < 0 || row >= (int)rows_.size() || rows_[(size_t)row].sample < 0) return;
  preview();  // double-click = hear it; putting it on the track is the "Use" button
}

// Any sound can be heard, whatever track is selected: as the selected track's role when it takes samples,
// otherwise as the sound's own role. Putting it on a track is the "Use" button.
void LibraryPanel::preview() {
  const auto* s = current();
  if (!s || !onPreviewSample) return;
  onPreviewSample(*s, fitNow().first);
}

void LibraryPanel::selectedRowsChanged(int row) {
  if (row >= 0 && row < (int)rows_.size() && tab_ == Tab::Templates && rows_[(size_t)row].preset >= 0) {
    currentTemplate_ = presetList_[(size_t)rows_[(size_t)row].preset].id;
    resized();  // the delete button follows the chosen one
    repaint(previewArea());
    return;
  }
  if (row >= 0 && row < (int)rows_.size() && tab_ == Tab::Grooves && rows_[(size_t)row].pattern >= 0) {
    currentPattern_ = rows_[(size_t)row].pattern;
    refresh();
    return;
  }
  if (row >= 0 && row < (int)rows_.size() && tab_ == Tab::Project && !rows_[(size_t)row].group) {
    currentProjectItem_ = !rows_[(size_t)row].asset.empty() ? rows_[(size_t)row].asset : rows_[(size_t)row].file;
    repaint(previewArea());
    return;
  }
  if (row < 0 || row >= (int)rows_.size() || rows_[(size_t)row].sample < 0) return;
  currentSample_ = rows_[(size_t)row].sample;
  refresh();
}

juce::Rectangle<int> LibraryPanel::previewArea() const {
  return tab_ == Tab::Sounds || tab_ == Tab::Project || tab_ == Tab::Templates || tab_ == Tab::Grooves ? getLocalBounds().reduced(8).removeFromBottom(kPreview) : juce::Rectangle<int>();
}

juce::Rectangle<int> LibraryPanel::rolesArea() const {
  auto r = getLocalBounds().reduced(8).withTrimmedTop(kHeader + 32 + 36);
  return r.removeFromTop(roles_.empty() ? 0 : 18 + 26 * (((int)roles_.size() + 2) / 3) + 4 + kSoundRow);
}

void LibraryPanel::resized() {
  auto r = getLocalBounds().reduced(8);
  minimise_.setBounds(r.withHeight(kHeader).removeFromRight(28).reduced(0, 4));
  r.removeFromTop(kHeader);
  search_.setBounds(r.removeFromTop(28));
  r.removeFromTop(4);
  auto tabs = r.removeFromTop(30);
  const int w = tabs.getWidth() / 4;
  // A little room between the buttons (Diogo 2026-11-09: "ligeira padding entre os buttons").
  sounds_.setBounds(tabs.removeFromLeft(w).reduced(3, 2));
  presetsTab_.setBounds(tabs.removeFromLeft(w).reduced(3, 2));
  grooves_.setBounds(tabs.removeFromLeft(w).reduced(3, 2));
  projectTab_.setBounds(tabs.reduced(3, 2));
  r.removeFromTop(6);
  if (tab_ == Tab::Sounds) {
    auto chips = rolesArea().withTrimmedTop(18);
    const int cw = chips.getWidth() / 3;
    for (size_t i = 0; i < roles_.size(); ++i) {
      if (i % 3 == 0 && i > 0) chips.removeFromTop(26);
      roles_[i]->setBounds(chips.getX() + (int)(i % 3) * cw, chips.getY(), cw, 26);
      roles_[i]->setBounds(roles_[i]->getBounds().reduced(4, 3));
    }
    auto sounds = rolesArea().removeFromBottom(26);
    const int sw = sounds.getWidth() / ((int)soundChips_.size() + 1);
    for (size_t i = 0; i < soundChips_.size(); ++i)
      soundChips_[i]->setBounds(juce::Rectangle<int>(sounds.getX() + (int)i * sw, sounds.getY(), sw, 26).reduced(3, 3));
    fit_.setBounds(juce::Rectangle<int>(sounds.getX() + (int)soundChips_.size() * sw, sounds.getY(), sounds.getRight() - sounds.getX() - (int)soundChips_.size() * sw, 26).reduced(3, 3));
    r.setTop(rolesArea().getBottom() + 6);
    r.removeFromBottom(kPreview + 8);
    auto preview = previewArea().reduced(8);
    auto head = preview.removeFromTop(22);
    star_.setBounds(head.removeFromRight(24));
    edit_.setBounds(head.removeFromRight(24));
    auto foot = preview.removeFromBottom(26);
    play_.setBounds(foot.removeFromLeft(34).reduced(0, 1));
    use_.setBounds(foot.removeFromRight(48).reduced(0, 1));
  }
  if (tab_ == Tab::Templates) {
    r.removeFromBottom(kPreview + 8);
    auto card = previewArea().reduced(8);
    auto head = card.removeFromTop(22);
    const auto* p = currentPreset();
    deleteTemplate_.setVisible(p != nullptr && p->id != kBasicTemplate);
    deleteTemplate_.setBounds(head.removeFromRight(24));
    saveTemplate_.setBounds(card.removeFromBottom(26));
    card.removeFromBottom(6);
    auto row = card.removeFromBottom(26);
    templatesInfo_.setBounds(row.removeFromLeft(InfoDot::kSlot + 4).withSizeKeepingCentre(InfoDot::kSlot, InfoDot::kSlot));
    newSong_.setBounds(row.removeFromRight(110));
    newSong_.setEnabled(p != nullptr && onOpenPreset != nullptr);
  }
  if (tab_ == Tab::Grooves) {
    r.removeFromBottom(kPreview + 8);
    auto foot = previewArea().reduced(8).removeFromBottom(26);
    playPattern_.setBounds(foot.removeFromLeft(34).reduced(0, 1));
    foot.removeFromLeft(6);
    for (auto& b : bars_) b.setBounds(foot.removeFromLeft(30).reduced(1, 1));
    addPattern_.setBounds(foot.removeFromRight(46).reduced(1, 1));
    applyPattern_.setBounds(foot.removeFromRight(56).reduced(1, 1));
  }
  if (tab_ == Tab::Project) {
    r.removeFromBottom(kPreview + 8);
    removeUnused_.setBounds(previewArea().reduced(8).removeFromBottom(26).removeFromRight(150));
  }
  list_.setBounds(r);
}

// The sound's catalogue entry: what Core uses and where each value comes from; Save keeps only what changed
// (as the user's own values), "Use the name again" takes the user's values away. One catalogue undo step.
void LibraryPanel::editMetadata() {
  const doc::SampleIndex* idx = index ? index() : nullptr;
  const doc::LibrarySample* s = current();
  if (!idx || !s || !onEditMetadata) return;
  const json now = doc::sampleMetadata(*idx, *s);
  const json& f = now["fields"];
  const json& origin = now["origin"];
  juce::String message;
  for (const char* k : {"role", "kind", "bpm", "key", "styles", "tags"})
    message << k << ": " << originText(origin[k]) << "    ";
  message << "\nFile: " << u8(s->path);
  if (s->embedded.is_object() && s->embedded.contains("info")) {  // what the file says about itself (read only)
    const json& info = s->embedded["info"];
    juce::StringArray said;
    for (const auto& [id, label] : std::initializer_list<std::pair<const char*, const char*>>{
             {"ISBJ", "Collection"}, {"IKEY", "Keywords"}, {"IART", "Artist"}, {"ICOP", "Copyright"}})
      if (info.contains(id) && info[id].is_string()) said.add(juce::String(label) + ": " + u8(info[id].get<std::string>()));
    if (!said.isEmpty()) message << "\n" << said.joinIntoString("    ");
  }
  if (s->embedded.is_object() && s->embedded.contains("loop")) message << "\nThe file has its own loop: the sampler loops it while a note is held.";
  if (now["locations"].size() > 1) message << "\n" << juce::String((int)now["locations"].size()) << " copies of this file in the library.";
  message << "\nThe WAV file is not changed.";
  auto* w = new juce::AlertWindow(lang::tr("Sound: " + u8(s->displayName.empty() ? s->name : s->displayName)), lang::tr(message), juce::MessageBoxIconType::NoIcon, this);
  w->addTextEditor("name", u8(f["name"].get<std::string>()), "Name (empty: Core makes one from the fields below):");
  static const juce::StringArray roles{"kick", "bass", "hat", "openhat", "snare", "clap", "perc", "tom", "crash", "impact",
                                       "riser", "fx", "vox", "pad", "synth", "orchestral", "fill", "kickbass", "other"};
  w->addComboBox("role", roles, "Role:");
  w->getComboBoxComponent("role")->setSelectedItemIndex(std::max(0, roles.indexOf(u8(f["role"].get<std::string>()))));
  w->addComboBox("kind", {"one-shot", "loop"}, "Kind:");
  w->getComboBoxComponent("kind")->setSelectedItemIndex(f["kind"] == "loop" ? 1 : 0);
  const double bpm = f["bpm"];
  w->addTextEditor("bpm", bpm > 0 ? juce::String(bpm) : juce::String(), "BPM (empty: none):");
  w->addTextEditor("key", u8(f["key"].get<std::string>()), "Key (F#m, C, E1; empty: none):");
  auto joined = [](const json& list) {
    juce::StringArray a;
    for (const auto& x : list) a.add(u8(x.get<std::string>()));
    return a.joinIntoString(", ");
  };
  w->addTextEditor("styles", joined(f["styles"]), "Styles (darkpsycore, hitech, slambient, psytrance...):");
  w->addTextEditor("tags", joined(f["tags"]), "Tags (comma separated):");
  w->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
  w->addButton("Use the name again", 2);
  w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
  juce::Component::SafePointer<juce::AlertWindow> safe(w);
  juce::Component::SafePointer<LibraryPanel> self(this);
  const std::string sha = s->sha256;
  w->enterModalState(true, juce::ModalCallbackFunction::create([safe, self, sha, f](int result) {
    if (result == 0 || safe == nullptr || self == nullptr) return;
    json fields = json::object();
    if (result == 2) {
      for (const char* k : {"role", "kind", "bpm", "key", "styles", "tags", "name"}) fields[k] = nullptr;
    } else {
      // Only what changed becomes the user's: a value left as it was keeps coming from the name or the file.
      auto put = [&](const char* k, const json& v) { if (v != f[k]) fields[k] = v; };
      put("role", safe->getComboBoxComponent("role")->getText().toStdString());
      put("kind", safe->getComboBoxComponent("kind")->getText().toStdString());
      const juce::String bpmText = safe->getTextEditorContents("bpm").trim();
      // Not a number: sent as it is, so the refusal says why.
      put("bpm", bpmText.isEmpty() ? json(0.0) : bpmText.containsOnly("0123456789.") ? json(bpmText.getDoubleValue()) : json(bpmText.toStdString()));
      json key = safe->getTextEditorContents("key").trim().toStdString();
      try { key = doc::normalizeSampleMetadata({{"key", key}})["key"]; } catch (const std::exception&) {}  // refused below, with why
      put("key", key);
      put("styles", splitWords(safe->getTextEditorContents("styles")));
      put("tags", splitWords(safe->getTextEditorContents("tags")));
      put("name", safe->getTextEditorContents("name").trim().toStdString());
      if (fields.empty()) return;
    }
    const std::string why = self->onEditMetadata(sha, fields);
    if (!why.empty())
      juce::AlertWindow::showAsync(juce::MessageBoxOptions().withIconType(juce::MessageBoxIconType::WarningIcon)
                                       .withTitle(juce::String("Not changed")).withMessage(u8(why)).withButton("OK", 0), nullptr);
  }), true);
}

// The chosen template: name, then style, tempo and whose it is.
void LibraryPanel::paintTemplateCard(juce::Graphics& g, juce::Rectangle<int> card) {
  g.setColour(kCard);
  g.fillRoundedRectangle(card.toFloat(), theme::kCorner);
  auto r = card.reduced(8);
  const auto* p = currentPreset();
  g.setColour(kText);
  g.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold));
  g.drawText(lang::tr(p ? u8(p->name) : juce::String("No template chosen")), r.removeFromTop(22).withTrimmedRight(28), juce::Justification::centredLeft, true);
  if (!p) return;
  g.setColour(kMuted);
  g.setFont(juce::FontOptions(theme::kFontSmall));
  juce::String line = p->style.empty() ? juce::String("No style") : u8(p->style);
  if (p->bpm > 0) line += juce::String::fromUTF8(" \xc2\xb7 ") + juce::String(p->bpm, p->bpm == std::round(p->bpm) ? 0 : 1) + " BPM";
  line += juce::String::fromUTF8(" \xc2\xb7 ") + (p->id == kBasicTemplate ? juce::String("Core's basic song") : p->builtin ? juce::String("built-in") : p->fromSong ? juce::String("from a song") : juce::String("yours"));
  g.drawText(line, r.removeFromTop(16), juce::Justification::centredLeft, true);
}

// The Project tab's card: the chosen file, where it came from, how many clips play it.
void LibraryPanel::paintProjectCard(juce::Graphics& g, juce::Rectangle<int> card) {
  g.setColour(kCard);
  g.fillRoundedRectangle(card.toFloat(), theme::kCorner);
  auto r = card.reduced(8);
  r.removeFromBottom(30);
  const doc::Document* d = document ? document() : nullptr;
  const doc::Asset* a = d ? d->findAsset(currentProjectItem_) : nullptr;
  juce::String title = "No file chosen";
  juce::StringArray lines;
  if (a) {
    title = u8(a->name);
    if (!a->madeFrom.empty()) {
      juce::String from = "another file";
      for (const auto& x : d->assets) if (x.sha256 == a->madeFrom) from = u8(x.name);
      lines.add("Made by Core: " + u8(a->made) + " of " + from);
    } else {
      lines.add("Imported");
    }
    const int clips = doc::clipsUsingAsset(*d, a->id);
    lines.add(clips == 0 ? juce::String("Not used by any clip") : "Played by " + juce::String(clips) + (clips == 1 ? " clip" : " clips"));
    juce::File f;
    if (projectFolder && !projectFolder().empty()) {
      const auto u = doc::assetFile(*a, projectFolder()).u8string();
      f = juce::File(juce::String::fromUTF8(std::string(u.begin(), u.end()).c_str()));
    }
    lines.add(juce::String(a->seconds(), a->seconds() < 10 ? 2 : 1) + " s" +
              (f.existsAsFile() ? "   " + juce::File::descriptionOfSizeInBytes(f.getSize()) : juce::String("   file missing")));
  } else if (!currentProjectItem_.empty()) {  // not an asset: a stray file's path
    const juce::File f(juce::String::fromUTF8(currentProjectItem_.c_str()));
    title = f.getFileName();
    lines.add("In the Audio folder, not in the song");
    if (f.existsAsFile()) lines.add(juce::File::descriptionOfSizeInBytes(f.getSize()));
  }
  g.setColour(kText);
  g.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold));
  g.drawText(title, r.removeFromTop(22), juce::Justification::centredLeft, true);
  g.setColour(kMuted);
  g.setFont(juce::FontOptions(theme::kFontSmall));
  for (const auto& l : lines) g.drawText(l, r.removeFromTop(16), juce::Justification::centredLeft, true);
}

void LibraryPanel::paintOverChildren(juce::Graphics& g) {
  if (!search_.isVisible()) return;  // the magnifier, inside the search field
  g.setColour(kDim);
  drawIcon(g, "search", search_.getBounds().removeFromLeft(26).toFloat().reduced(5));
}

void LibraryPanel::paint(juce::Graphics& g) {
  g.setColour(kPanel);
  g.fillRoundedRectangle(getLocalBounds().toFloat(), theme::kCorner);
  auto head = getLocalBounds().reduced(12, 8).removeFromTop(kHeader - 8);
  g.setColour(kMuted);
  drawIcon(g, "library", head.removeFromLeft(20).toFloat());
  g.setColour(kText);
  g.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold).withKerningFactor(0.08f));
  g.drawText(lang::tr("LIBRARY"), head.withTrimmedLeft(8), juce::Justification::centredLeft);
  if (tab_ == Tab::Sounds && !roles_.empty()) {
    g.setColour(kMuted);
    g.setFont(juce::FontOptions(theme::kFontSmall, juce::Font::bold).withKerningFactor(0.08f));
    g.drawText(lang::tr("ROLE"), rolesArea().removeFromTop(16), juce::Justification::centredLeft);
    g.drawText(lang::tr("SOUND"), rolesArea().removeFromBottom(kSoundRow).removeFromTop(16), juce::Justification::centredLeft);
  }
  const doc::SampleIndex* idx = index ? index() : nullptr;
  if ((tab_ == Tab::Grooves && rows_.empty()) || (tab_ == Tab::Sounds && !idx) || (tab_ == Tab::Templates && presetList_.empty())) {
    g.setColour(kMuted);
    g.setFont(juce::FontOptions(theme::kFontBody));
    const juce::String text = tab_ == Tab::Grooves ? (patternList_.empty() ? "No patterns found (the patterns folder beside Core.exe)." : "No pattern with these words.")
                              : tab_ == Tab::Templates ? "No templates found."
                                                     : "No sample library yet.\nSamples > Choose the library folder.";
    g.drawFittedText(text, list_.getBounds().reduced(8).removeFromTop(60), juce::Justification::centred, 3);
  }
  if (tab_ == Tab::Sounds && idx && rows_.empty()) {
    g.setColour(kMuted);
    g.setFont(juce::FontOptions(theme::kFontBody));
    g.drawFittedText(lang::tr(fit_.getToggleState() && !fitNow().first.empty()
                         ? "No sound fits this track at the song's tempo\nwith these filters. Switch \"fits\" off to see all."
                         : juce::String("No sound with these filters.")),
                     list_.getBounds().reduced(8).removeFromTop(60), juce::Justification::centred, 3);
  }
  if (tab_ == Tab::Grooves) {
    paintPatternCard(g, previewArea());
    return;
  }
  if (tab_ == Tab::Project) {
    if (rows_.empty()) {
      g.setColour(kMuted);
      g.setFont(juce::FontOptions(theme::kFontBody));
      g.drawFittedText(lang::tr("This song has no audio files yet.\nImported audio and the copies Core makes\n(reversed, stretched...) show here."),
                       list_.getBounds().reduced(8).removeFromTop(60), juce::Justification::centred, 3);
    }
    paintProjectCard(g, previewArea());
    return;
  }
  if (tab_ == Tab::Templates) {
    paintTemplateCard(g, previewArea());
    return;
  }
  if (tab_ != Tab::Sounds) return;
  // The preview card: name, waveform, format.
  const auto card = previewArea();
  g.setColour(kCard);
  g.fillRoundedRectangle(card.toFloat(), theme::kCorner);
  auto r = card.reduced(8);
  const auto* s = current();
  g.setColour(kText);
  g.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold));
  g.drawText(lang::tr(s ? u8(s->displayName.empty() ? s->name : s->displayName) : juce::String("No sound chosen")), r.removeFromTop(22).withTrimmedRight(52),
             juce::Justification::centredLeft, true);
  if (s) {
    g.setColour(kMuted);
    g.setFont(juce::FontOptions(theme::kFontSmall));
    g.drawText(describe(s->info), r.removeFromTop(16), juce::Justification::centredLeft, true);
    doc::SampleAnalysis a;
    const bool measured = doc::analysisFromJson(s->analysis, a) && a.sha256 == s->sha256;
    juce::String line = !measured ? juce::String("not measured yet")
                        : a.silent ? juce::String("silent")
                                   : "attack " + juce::String((int)a.attackMs) + " ms" +
                                         (a.tailMs >= 0 ? juce::String::fromUTF8(" \xc2\xb7 tail ") + juce::String((int)a.tailMs) + " ms"
                                                        : juce::String::fromUTF8(" \xc2\xb7 ") + juce::String(a.hits) + " hits") +
                                         juce::String::fromUTF8(" \xc2\xb7 ") + juce::String((int)a.centroidHz) + " Hz" +
                                         juce::String::fromUTF8(" \xc2\xb7 sub ") + juce::String((int)std::round(a.subRatio * 100)) + "%";
    g.drawText(line, r.removeFromTop(16), juce::Justification::centredLeft, true);
    if (const auto [fitRole, fitBpm] = fitNow(); !fitRole.empty()) {
      const doc::SampleFit f = doc::sampleFit(*s, fitRole, fitBpm);
      g.setColour(f.fits ? kAccent : kMuted);
      g.drawText(lang::tr((f.fits ? juce::String("Fits: ") : !f.known ? juce::String("Can't tell: ") : juce::String("Doesn't fit: ")) + u8(f.why)),
                 r.removeFromTop(16), juce::Justification::centredLeft, true);
    }
  }
  auto foot = r.removeFromBottom(26);
  auto wave = r.reduced(0, 4);
  g.setColour(kField);
  g.fillRoundedRectangle(wave.toFloat(), theme::kCorner);
  if (s && waveform) {
    if (auto w = waveform(*s)) {
      drawWaveform(g, *w, wave, 0.0, std::max(0.001, s->seconds), kAccent, 3.0f);
    }
  }
  if (s) {
    g.setColour(kMuted);
    g.setFont(juce::FontOptions(theme::kFontSmall));
    const juce::String info = u8(s->name) + "   " + (std::fmod(s->sampleRate, 1000.0) == 0 ? juce::String((int)(s->sampleRate / 1000)) : juce::String(s->sampleRate / 1000.0, 1)) + " kHz   " +
                              (s->channels == 1 ? "mono" : "stereo") + "   " + juce::String(s->seconds, s->seconds < 10 ? 2 : 1) + " s";
    g.drawText(info, foot.withTrimmedLeft(44).withTrimmedRight(tab_ == Tab::Sounds ? 54 : 0), juce::Justification::centredLeft, true);
  }
}

const doc::PatternPreset* LibraryPanel::currentPatternPreset() const {
  return currentPattern_ >= 0 && currentPattern_ < (int)patternList_.size() ? &patternList_[(size_t)currentPattern_] : nullptr;
}

void LibraryPanel::patternsChanged() {
  if (!patterns) return;
  const std::string id = currentPatternPreset() ? currentPatternPreset()->id : std::string();
  patternList_ = patterns();
  currentPattern_ = -1;
  for (size_t i = 0; i < patternList_.size(); ++i) if (patternList_[i].id == id) currentPattern_ = (int)i;
  if (tab_ == Tab::Grooves) rebuild();
  refresh();
}

// The Patterns tab's card: name, what it is, its styles and whether anyone heard it, and its notes drawn small.
void LibraryPanel::paintPatternCard(juce::Graphics& g, juce::Rectangle<int> card) {
  g.setColour(kCard);
  g.fillRoundedRectangle(card.toFloat(), theme::kCorner);
  auto r = card.reduced(8);
  r.removeFromBottom(30);
  const doc::PatternPreset* p = currentPatternPreset();
  g.setColour(kText);
  g.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold));
  g.drawText(lang::tr(p ? u8(p->name) : juce::String("No pattern chosen")), r.removeFromTop(22), juce::Justification::centredLeft, true);
  if (!p) return;
  g.setColour(kMuted);
  g.setFont(juce::FontOptions(theme::kFontSmall));
  g.drawFittedText(u8(p->description), r.removeFromTop(28), juce::Justification::topLeft, 2);
  juce::StringArray styles;
  for (const auto& st : p->styles) styles.add(u8(st));
  const int bars = (int)std::max<int64_t>(1, p->lengthTicks / (4 * doc::kPpq));
  g.drawText(lang::tr(juce::String(bars) + (bars == 1 ? " bar" : " bars") + juce::String::fromUTF8(" \xc2\xb7 ") +
                 (p->status == "approved" ? juce::String("approved") : p->status == "draft" ? juce::String("yours, draft") : juce::String("not heard yet")) +
                 (styles.isEmpty() ? juce::String() : juce::String::fromUTF8(" \xc2\xb7 ") + styles.joinIntoString(", "))),
             r.removeFromTop(16), juce::Justification::centredLeft, true);
  auto roll = r.reduced(0, 4);
  g.setColour(kField);
  g.fillRoundedRectangle(roll.toFloat(), theme::kCorner);
  if (p->notes.empty() || roll.getHeight() < 8) return;
  int lo = 127, hi = 0;
  for (const auto& n : p->notes) { lo = std::min(lo, n.pitch); hi = std::max(hi, n.pitch); }
  const float span = (float)std::max(1, hi - lo + 1), rowH = std::max(2.0f, std::min(6.0f, (float)(roll.getHeight() - 4) / span));
  const float w = (float)roll.getWidth() - 4, x0 = (float)roll.getX() + 2, y0 = (float)roll.getBottom() - 2;
  g.setColour(kDim.withAlpha(0.5f));
  for (int b = 1; b < bars; ++b) g.drawVerticalLine((int)(x0 + w * (float)b / (float)bars), (float)roll.getY() + 2, (float)roll.getBottom() - 2);
  for (const auto& n : p->notes) {
    const float x = x0 + w * (float)n.tick / (float)p->lengthTicks, nw = std::max(1.5f, w * (float)n.length / (float)p->lengthTicks);
    const float y = y0 - rowH * (float)(n.pitch - lo + 1);
    g.setColour(kAccent.withAlpha(0.35f + 0.65f * (float)n.velocity));
    g.fillRect(x, y, nw, rowH - 1);
  }
}

} // namespace core::app
