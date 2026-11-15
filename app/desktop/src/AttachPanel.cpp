#include "AttachPanel.h"
#include "Theme.h"
#include <algorithm>
#include <set>

namespace core::app {
using namespace theme;

namespace {
juce::String u8(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }
juce::String u8(const char* s) { return juce::String::fromUTF8(s); }
constexpr int kPad = 14, kRowH = 28, kLabelW = 72, kWidth = 400, kBrowserH = 320;
struct LibItem { std::string id, folder; juce::String name, tip; bool yours = false; };
std::vector<LibItem> library() {
  std::vector<LibItem> out;
  for (const auto& r : doc::attachRecipes()) out.push_back({r.id, doc::attachRecipeFolder(r.id), u8(r.name), u8(r.tip)});
  for (const auto& u : doc::userAttachments()) {
    const std::string name = u.value("name", std::string());
    out.push_back({"user:" + name, u.value("folder", std::string()), u8(name), u8("yours"), true});
  }
  return out;
}
const char* kPlaceNames[3] = {"Start", "Middle", "End"};
const char* kPlaceTips[3] = {"The way in: the first third of a note, the first part of a clip",
                             "The identity: the whole note or clip",
                             "The way out: the last third of a note (on to the next one), the last part of a clip"};
constexpr int64_t kEdges[] = {doc::kPpq, 2 * doc::kPpq, 4 * doc::kPpq, 8 * doc::kPpq, 16 * doc::kPpq};
const char* kEdgeNames[] = {"1 beat", "2 beats", "1 bar", "2 bars", "4 bars"};
const char* kGroupNames[4] = {"", "On this track", "New, from the library", "New, from yours"};
// The folders opened stay open the next time (for this run of Core).
std::set<std::string>& opened() {
  static std::set<std::string> keys{"1", "2", "3"};
  return keys;
}
}  // namespace

std::filesystem::path userAttachmentsFolderPath() {
  const juce::File f = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Core").getChildFile("Presets").getChildFile("attachments");
  return std::filesystem::path(f.getFullPathName().toWideCharPointer());
}

void reloadUserAttachments() {
  try { doc::setUserAttachments(doc::loadUserAttachments(userAttachmentsFolderPath())); } catch (...) {}
}

// ---------------------------------------------------------------- the library browser

std::vector<AttachBrowser::Item> attachBrowserItems(const std::vector<doc::AttachInfo>& own, bool none) {
  std::vector<AttachBrowser::Item> out;
  if (none) out.push_back({u8("None"), u8("Take the attachment off"), "", "", "", 0});
  for (const auto& a : own) out.push_back({u8(a.name), u8("on this track"), a.name, a.recipe, "", 1});
  for (const auto& l : library()) out.push_back({l.name, l.tip, "", l.id, l.folder, l.yours ? 3 : 2});
  return out;
}

AttachBrowser::AttachBrowser(std::vector<Item> items, const juce::String& heading) : items_(std::move(items)) {
  heading_.setText(heading, juce::dontSendNotification);
  heading_.setColour(juce::Label::textColourId, kText);
  heading_.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold));
  addAndMakeVisible(heading_);
  search_.setTextToShowWhenEmpty(u8("Search by name..."), kMuted);
  search_.onTextChange = [this] { rebuild(); };
  search_.onReturnKey = [this] {
    for (int i = 0; i < (int)rows_.size(); ++i) if (!rows_[(size_t)i].header) { pickRow(i); return; }
  };
  addAndMakeVisible(search_);
  list_.setModel(this);
  list_.setRowHeight(24);
  list_.setColour(juce::ListBox::backgroundColourId, kField);
  list_.setColour(juce::ListBox::outlineColourId, kOutline);
  list_.setOutlineThickness(1);
  addAndMakeVisible(list_);
  rebuild();
}

void AttachBrowser::rebuild() {
  rows_.clear();
  const juce::String find = search_.getText().trim();
  if (find.isNotEmpty()) {  // one flat list: every name (or folder) with the words in it
    for (size_t i = 0; i < items_.size(); ++i) {
      const auto& it = items_[i];
      const juce::String where = u8(it.folder);
      if (!it.label.containsIgnoreCase(find) && !where.containsIgnoreCase(find)) continue;
      Row r;
      r.item = (int)i;
      r.text = (it.group >= 2 ? u8(it.group == 2 ? "Library · " : "Yours · ") : juce::String()) + (where.isNotEmpty() ? where + u8(" · ") : juce::String()) + it.label;
      rows_.push_back(r);
    }
  } else {
    for (size_t i = 0; i < items_.size(); ++i) if (items_[i].group == 0) { Row r; r.item = (int)i; r.text = items_[i].label; rows_.push_back(r); }
    for (int group = 1; group <= 3; ++group) {
      std::vector<size_t> in;
      for (size_t i = 0; i < items_.size(); ++i) if (items_[i].group == group) in.push_back(i);
      if (in.empty()) continue;
      const std::string top = std::to_string(group);
      rows_.push_back({true, 0, top, u8(kGroupNames[group]), -1, (int)in.size()});
      if (!opened().count(top)) continue;
      // Folders as a tree: each folder path's parts become headers, opened one by one.
      std::string lastFolder = "\x01";
      std::vector<std::string> shownParts;
      bool hidden = false;
      // Items without a folder first, then by folder (as loaded).
      std::stable_sort(in.begin(), in.end(), [&](size_t a, size_t b) { return items_[a].folder.empty() > items_[b].folder.empty(); });
      for (size_t i : in) {
        const auto& it = items_[i];
        if (it.folder != lastFolder) {
          lastFolder = it.folder;
          std::vector<std::string> parts;
          juce::StringArray split;
          split.addTokens(u8(it.folder), "/", "");
          for (const auto& p : split) if (p.isNotEmpty()) parts.push_back(p.toStdString());
          hidden = false;
          std::string key = top;
          for (size_t d = 0; d < parts.size(); ++d) {
            key += "/" + parts[d];
            const bool same = d < shownParts.size() && shownParts[d] == parts[d] &&
                              std::equal(parts.begin(), parts.begin() + (long)d, shownParts.begin());
            if (!same && !hidden) {
              int count = 0;
              for (size_t j : in) {
                const std::string& f = items_[j].folder;
                const std::string prefix = key.substr(top.size() + 1);
                count += f == prefix || f.rfind(prefix + "/", 0) == 0;
              }
              rows_.push_back({true, (int)d + 1, key, u8(parts[d]), -1, count});
            }
            if (!opened().count(key)) { hidden = true; break; }
          }
          shownParts = parts;
          if (hidden) continue;
        } else if (hidden) {
          continue;
        }
        Row r;
        r.item = (int)i;
        r.depth = 1 + (int)std::count(it.folder.begin(), it.folder.end(), '/') + (it.folder.empty() ? 0 : 1);
        r.text = it.label;
        rows_.push_back(r);
      }
    }
  }
  list_.updateContent();
  list_.repaint();
}

void AttachBrowser::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool) {
  if (row < 0 || row >= (int)rows_.size()) return;
  const Row& r = rows_[(size_t)row];
  if (list_.isMouseOver(true) && list_.getRowContainingPosition(list_.getMouseXYRelative().x, list_.getMouseXYRelative().y) == row)
    g.fillAll(kAccent.withAlpha(0.18f));
  auto area = juce::Rectangle<int>(0, 0, width, height).reduced(8, 0).withTrimmedLeft(r.depth * 14);
  if (r.header) {
    const bool open = opened().count(r.key) > 0;
    g.setColour(kMuted);
    g.setFont(juce::FontOptions(theme::kFontBody));
    g.drawText(open ? u8("▾") : u8("▸"), area.removeFromLeft(14), juce::Justification::centredLeft);
    g.setColour(kText);
    g.setFont(juce::FontOptions(theme::kFontBody, r.depth == 0 ? juce::Font::bold : juce::Font::plain));
    g.drawText(r.text + u8("  (") + juce::String(r.count) + u8(")"), area, juce::Justification::centredLeft);
    return;
  }
  const Item& it = items_[(size_t)r.item];
  g.setColour(kText);
  g.setFont(juce::FontOptions(theme::kFontBody));
  const int nameW = std::min(area.getWidth(), juce::GlyphArrangement::getStringWidthInt(juce::Font(juce::FontOptions(theme::kFontBody)), r.text) + 12);
  g.drawText(r.text, area.removeFromLeft(nameW), juce::Justification::centredLeft);
  g.setColour(kMuted);
  g.setFont(juce::FontOptions(theme::kFontBody));
  g.drawText(it.tip, area, juce::Justification::centredLeft, true);
}

void AttachBrowser::pickRow(int row) {
  if (row < 0 || row >= (int)rows_.size()) return;
  const Row r = rows_[(size_t)row];
  if (r.header) {
    if (!opened().erase(r.key)) opened().insert(r.key);
    rebuild();
    return;
  }
  if (onPick) onPick(items_[(size_t)r.item]);
}

juce::String AttachBrowser::getTooltipForRow(int row) {
  if (row < 0 || row >= (int)rows_.size() || rows_[(size_t)row].header) return {};
  return items_[(size_t)rows_[(size_t)row].item].tip;
}

void AttachBrowser::resized() {
  auto r = getLocalBounds();
  auto top = r.removeFromTop(26);
  heading_.setBounds(top.removeFromLeft(top.getWidth() / 2));
  search_.setBounds(top.reduced(0, 1));
  r.removeFromTop(4);
  list_.setBounds(r);
}

void AttachBrowser::paint(juce::Graphics&) {}

// ---------------------------------------------------------------- one note or clip: Start, Middle, End

AttachPanel::AttachPanel(const Start& start) : clip_(start.clip) {
  reloadUserAttachments();
  title_.setText(u8("Attachments · ") + start.what, juce::dontSendNotification);
  title_.setFont(juce::FontOptions(theme::kFontTitle, juce::Font::bold));
  title_.setColour(juce::Label::textColourId, kText);
  addAndMakeVisible(title_);
  for (int i = 0; i < 3; ++i) {
    shown_[i] = start.places[i];
    names_[i].setText(u8(kPlaceNames[i]), juce::dontSendNotification);
    names_[i].setColour(juce::Label::textColourId, kText);
    names_[i].setTooltip(u8(kPlaceTips[i]));
    addAndMakeVisible(names_[i]);
    fields_[i].setTooltip(u8(kPlaceTips[i]) + u8(". Click: choose from the library."));
    fields_[i].setConnectedEdges(juce::Button::ConnectedOnRight);
    fields_[i].onClick = [this, i] { browse(i); };
    addAndMakeVisible(fields_[i]);
    clears_[i].setButtonText(u8("✕"));
    clears_[i].setTooltip(u8("None: take it off"));
    clears_[i].setConnectedEdges(juce::Button::ConnectedOnLeft);
    clears_[i].onClick = [this, i] { set(i, ""); };
    addAndMakeVisible(clears_[i]);
  }
  if (clip_) {
    edgeName_.setText(u8("Length"), juce::dontSendNotification);
    edgeName_.setColour(juce::Label::textColourId, kText);
    edgeName_.setTooltip(u8("How long the clip's Start and End parts are"));
    addAndMakeVisible(edgeName_);
    int pick = 3;
    for (int i = 0; i < 5; ++i) {
      edge_.addItem(u8(kEdgeNames[i]), i + 1);
      if (kEdges[i] == start.edge) pick = i + 1;
    }
    edge_.setSelectedId(pick, juce::dontSendNotification);
    edge_.onChange = [this] {
      if (onEdge && edge_.getSelectedId() > 0) status_.setText(onEdge(kEdges[edge_.getSelectedId() - 1]), juce::dontSendNotification);
    };
    addAndMakeVisible(edge_);
  }
  status_.setColour(juce::Label::textColourId, kText.withAlpha(0.75f));
  addAndMakeVisible(status_);
  tip_.setText(u8("The same name on one End and the next Start joins them into one."), juce::dontSendNotification);
  tip_.setColour(juce::Label::textColourId, kText.withAlpha(0.55f));
  tip_.setFont(juce::FontOptions(theme::kFontBody));
  addAndMakeVisible(tip_);
  showFields();
  fit();
}

void AttachPanel::fit() {
  setSize(kWidth, kPad * 2 + 30 + (clip_ ? 4 : 3) * (kRowH + 6) + (browser_ ? kBrowserH + 8 : 0) + 24 + 22);
}

void AttachPanel::showFields() {
  for (int i = 0; i < 3; ++i) {
    fields_[i].setButtonText(shown_[i].empty() ? u8("None  ·  choose...") : u8(shown_[i]));
    fields_[i].setToggleState(browsing_ == i, juce::dontSendNotification);
    clears_[i].setEnabled(!shown_[i].empty());
  }
}

void AttachPanel::browse(int place) {
  if (browser_ && browsing_ == place) {  // the same field again: closed
    browser_.reset();
    browsing_ = -1;
  } else {
    browsing_ = place;
    reloadUserAttachments();  // a file just put in a folder shows at once
    browser_ = std::make_unique<AttachBrowser>(attachBrowserItems(attaches ? attaches() : std::vector<doc::AttachInfo>{}, true),
                                               u8("For ") + u8(kPlaceNames[place]));
    browser_->onPick = [this, place](const AttachBrowser::Item& item) {
      // Run after the click is over (the browser goes away).
      juce::Component::SafePointer<AttachPanel> safe(this);
      juce::MessageManager::callAsync([safe, place, item] { if (safe) safe->picked(place, item); });
    };
    addAndMakeVisible(*browser_);
  }
  showFields();
  fit();
  resized();
}

void AttachPanel::picked(int place, const AttachBrowser::Item& item) {
  browser_.reset();
  browsing_ = -1;
  showFields();
  fit();
  resized();
  if (item.group <= 1) { set(place, item.name); return; }  // None, or one the track has
  const std::string recipe = item.recipe;
  auto* w = new juce::AlertWindow(u8("New attachment"), item.tip + u8("\n\nIts name (any name you like):"), juce::MessageBoxIconType::NoIcon, this);
  w->addTextEditor("name", item.label, u8("Name:"));
  w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
  w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
  juce::Component::SafePointer<juce::AlertWindow> win(w);
  juce::Component::SafePointer<AttachPanel> safe(this);
  w->enterModalState(true, juce::ModalCallbackFunction::create([win, safe, recipe, place](int ok) {
    if (ok != 1 || !win || !safe || !safe->onCreate) return;
    const std::string name = safe->onCreate(recipe, win->getTextEditorContents("name").trim().toStdString());
    if (!name.empty()) safe->set(place, name);
  }), true);
}

void AttachPanel::set(int place, const std::string& name) {
  if (name == shown_[place]) return;
  shown_[place] = name;
  showFields();
  if (onSet) status_.setText(onSet(place, name), juce::dontSendNotification);
}

void AttachPanel::resized() {
  auto r = getLocalBounds().reduced(kPad);
  title_.setBounds(r.removeFromTop(30));
  for (int i = 0; i < 3; ++i) {
    auto row = r.removeFromTop(kRowH);
    names_[i].setBounds(row.removeFromLeft(kLabelW));
    clears_[i].setBounds(row.removeFromRight(30));
    fields_[i].setBounds(row);
    r.removeFromTop(6);
  }
  if (clip_) {
    auto row = r.removeFromTop(kRowH);
    edgeName_.setBounds(row.removeFromLeft(kLabelW));
    edge_.setBounds(row.removeFromLeft(140));
    r.removeFromTop(6);
  }
  if (browser_) {
    browser_->setBounds(r.removeFromTop(kBrowserH));
    r.removeFromTop(8);
  }
  status_.setBounds(r.removeFromTop(24));
  tip_.setBounds(r.removeFromTop(22));
}

void AttachPanel::paint(juce::Graphics& g) { g.fillAll(kPanel); }

// ---------------------------------------------------------------- many at once

AttachPlanPanel::AttachPlanPanel(const juce::String& what, std::vector<Choice> choices) : choices_(std::move(choices)) {
  title_.setText(u8("Apply attachments · ") + what, juce::dontSendNotification);
  title_.setFont(juce::FontOptions(theme::kFontTitle, juce::Font::bold));
  title_.setColour(juce::Label::textColourId, kText);
  addAndMakeVisible(title_);
  const auto label = [this](juce::Label& l, const char* text, const char* tip) {
    l.setText(u8(text), juce::dontSendNotification);
    l.setColour(juce::Label::textColourId, kText);
    l.setTooltip(u8(tip));
    addAndMakeVisible(l);
  };
  label(whereName_, "Where", "Which places of each note or clip get an attachment");
  label(modeName_, "Pattern", "Same: one attachment on both sides of a link. Alternate: every link goes from one to another. Random: total chaos");
  label(libName_, "Use", "The attachments to use: the track's own, and new ones from the library and yours");
  const char* wheres[] = {"End", "Start", "Start + End", "Start + Middle + End", "Middle"};
  for (int i = 0; i < 5; ++i) where_.addItem(u8(wheres[i]), i + 1);
  where_.setSelectedId(3, juce::dontSendNotification);
  addAndMakeVisible(where_);
  mode_.addItem(u8("Same (the link keeps one attachment)"), 1);
  mode_.addItem(u8("Alternate (every link changes)"), 2);
  mode_.addItem(u8("Random (total chaos)"), 3);
  mode_.setSelectedId(1, juce::dontSendNotification);
  addAndMakeVisible(mode_);
  if (!choices_.empty() && !choices_[0].name.empty()) chosen_.push_back(choices_[0]);  // the track's first, to start
  add_.setButtonText(u8("Add from the library..."));
  add_.setTooltip(u8("Open the library (in folders, with a search) and pick one more"));
  add_.onClick = [this] {
    if (browser_) { browser_.reset(); fit(); resized(); return; }
    reloadUserAttachments();
    std::vector<AttachBrowser::Item> items;
    for (const auto& c : choices_) {
      AttachBrowser::Item it{c.label, "", c.name, c.recipe, "", c.name.empty() ? (c.recipe.rfind("user:", 0) == 0 ? 3 : 2) : 1};
      if (!c.name.empty()) it.tip = u8("on this track");
      else if (c.recipe.rfind("user:", 0) == 0) {
        it.tip = u8("yours");
        it.label = u8(c.recipe.substr(5));
        for (const auto& u : doc::userAttachments()) if (u.value("name", std::string()) == c.recipe.substr(5)) it.folder = u.value("folder", std::string());
      } else {
        it.folder = doc::attachRecipeFolder(c.recipe);
        for (const auto& r : doc::attachRecipes()) if (c.recipe == r.id) { it.label = u8(r.name); it.tip = u8(r.tip); }
      }
      items.push_back(it);
    }
    browser_ = std::make_unique<AttachBrowser>(std::move(items), u8("Add one"));
    browser_->onPick = [this](const AttachBrowser::Item& item) {
      juce::Component::SafePointer<AttachPlanPanel> safe(this);
      juce::MessageManager::callAsync([safe, item] {
        if (!safe) return;
        bool have = false;
        for (const auto& c : safe->chosen_) have = have || (c.name == item.name && c.recipe == item.recipe);
        if (!have) safe->chosen_.push_back({item.label + (item.group == 1 ? juce::String() : item.group == 3 ? u8(" (yours, new)") : u8(" (library, new)")),
                                            item.name, item.recipe});
        safe->browser_.reset();
        safe->showChosen();
      });
    };
    addAndMakeVisible(*browser_);
    fit();
    resized();
  };
  addAndMakeVisible(add_);
  apply_.setButtonText(u8("Apply"));
  apply_.onClick = [this] { apply(false); };
  addAndMakeVisible(apply_);
  again_.setButtonText(u8("Again"));
  again_.setTooltip(u8("Another draw (Random), written over the last one"));
  again_.onClick = [this] { apply(true); };
  addAndMakeVisible(again_);
  status_.setColour(juce::Label::textColourId, kText.withAlpha(0.75f));
  addAndMakeVisible(status_);
  showChosen();
}

void AttachPlanPanel::showChosen() {
  chosenNames_.clear();
  chosenOff_.clear();
  for (size_t i = 0; i < chosen_.size(); ++i) {
    auto* l = chosenNames_.add(new juce::Label({}, chosen_[i].label));
    l->setColour(juce::Label::textColourId, kText);
    addAndMakeVisible(l);
    auto* off = chosenOff_.add(new juce::TextButton(u8("✕")));
    off->setTooltip(u8("Do not use this one"));
    off->onClick = [this, i] {
      juce::Component::SafePointer<AttachPlanPanel> safe(this);
      juce::MessageManager::callAsync([safe, i] {
        if (!safe || i >= safe->chosen_.size()) return;
        safe->chosen_.erase(safe->chosen_.begin() + (long)i);
        safe->showChosen();
      });
    };
    addAndMakeVisible(off);
  }
  if (chosen_.empty()) status_.setText(u8("Add at least one attachment."), juce::dontSendNotification);
  fit();
  resized();
}

void AttachPlanPanel::fit() {
  const int rows = std::max(1, (int)chosen_.size());
  setSize(460, kPad * 2 + 30 + 3 * (kRowH + 6) + rows * 26 + 6 + kRowH + 8 + (browser_ ? kBrowserH + 8 : 0) + kRowH + 6 + 24);
}

void AttachPlanPanel::apply(bool again) {
  if (!onApply) return;
  if (again) ++seed_;
  doc::AttachPlan plan;
  const int w = where_.getSelectedId();
  plan.start = w == 2 || w == 3 || w == 4;
  plan.end = w == 1 || w == 3 || w == 4;
  plan.middle = w == 4 || w == 5;
  plan.mode = mode_.getSelectedId() == 2 ? "alternate" : mode_.getSelectedId() == 3 ? "random" : "same";
  plan.seed = seed_;
  std::vector<std::string> recipes;
  for (const auto& c : chosen_) {
    if (!c.name.empty()) plan.names.push_back(c.name);
    else recipes.push_back(c.recipe);
  }
  if (plan.names.empty() && recipes.empty()) { status_.setText(u8("Add at least one attachment."), juce::dontSendNotification); return; }
  // A library pick is made on the track once: Again and the next Apply reuse it (doc::ensureAttach).
  status_.setText(onApply(plan, recipes), juce::dontSendNotification);
}

void AttachPlanPanel::resized() {
  auto r = getLocalBounds().reduced(kPad);
  title_.setBounds(r.removeFromTop(30));
  const auto row = [&](juce::Label& l, juce::Component& c) {
    auto line = r.removeFromTop(kRowH);
    l.setBounds(line.removeFromLeft(kLabelW));
    c.setBounds(line);
    r.removeFromTop(6);
  };
  row(whereName_, where_);
  row(modeName_, mode_);
  libName_.setBounds(r.removeFromTop(kRowH));
  r.removeFromTop(6);
  for (int i = 0; i < chosenNames_.size(); ++i) {
    auto line = r.removeFromTop(26);
    chosenOff_[i]->setBounds(line.removeFromRight(28).reduced(0, 2));
    chosenNames_[i]->setBounds(line);
  }
  if (chosenNames_.isEmpty()) r.removeFromTop(26);
  r.removeFromTop(6);
  add_.setBounds(r.removeFromTop(kRowH).removeFromLeft(200));
  r.removeFromTop(8);
  if (browser_) {
    browser_->setBounds(r.removeFromTop(kBrowserH));
    r.removeFromTop(8);
  }
  auto buttons = r.removeFromTop(kRowH);
  apply_.setBounds(buttons.removeFromLeft(100));
  buttons.removeFromLeft(8);
  again_.setBounds(buttons.removeFromLeft(100));
  r.removeFromTop(6);
  status_.setBounds(r.removeFromTop(24));
}

void AttachPlanPanel::paint(juce::Graphics& g) { g.fillAll(kPanel); }

std::vector<AttachPlanPanel::Choice> attachChoices(const doc::Document& doc, const std::string& trackId) {
  std::vector<AttachPlanPanel::Choice> out;
  std::vector<doc::AttachInfo> own;
  try { own = doc::trackAttaches(doc, trackId); } catch (...) {}
  for (const auto& a : own) out.push_back({u8(a.name), a.name, a.recipe});
  reloadUserAttachments();
  for (const auto& item : library()) {
    bool have = false;
    for (const auto& a : own) have = have || a.recipe == item.id;
    if (!have) out.push_back({item.name + (item.id.rfind("user:", 0) == 0 ? u8(" (yours)") : u8(" (library)")), "", item.id});
  }
  return out;
}

}  // namespace core::app
