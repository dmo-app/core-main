// Attachments (Diogo, 2026-11-07, doc::attach): the small window that opens on notes (Edit > Attachments...) or on a
// clip (right click > Attachments...): "aparece um plugin, start escolhes o attachment e end outro". Three boxes,
// Start, Middle, End; each change is applied at once (one Ctrl+Z each), so it can be tried while the song plays.
// Each place is a field: a click opens the library under it (AttachBrowser); a pick from the library or yours makes a
// new attachment on the track, named by the user.
#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>
#include "attach.hpp"

namespace core::app {

// The attachments to choose from, in folders (Diogo 2026-11-07: "um campo para ir à biblioteca buscar, porque com
// dropdown vai chegar uma altura que fica demais; na biblioteca depois posso estruturar por pastas"). Three groups:
// the track's own; the library's, in its folders (Glitch, Filter, Space...); yours, in the folders you make inside
// %APPDATA%\Core\Presets\attachments (or by saving "Folder/Name"). A search box finds any of them by name.
class AttachBrowser final : public juce::Component, private juce::ListBoxModel {
public:
  struct Item {
    juce::String label, tip;
    std::string name;    // the track's own attachment (made already)
    std::string recipe;  // or one to make: a library id or "user:<name>"
    std::string folder;  // "Glitch", "Mine/Rolls"
    int group = 0;       // 0 None, 1 the track's, 2 the library's, 3 yours
  };
  std::function<void(const Item&)> onPick;

  AttachBrowser(std::vector<Item> items, const juce::String& heading);
  void resized() override;
  void paint(juce::Graphics& g) override;

private:
  struct Row { bool header = false; int depth = 0; std::string key; juce::String text; int item = -1, count = 0; };
  void rebuild();
  int getNumRows() override { return (int)rows_.size(); }
  void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override;
  void listBoxItemClicked(int row, const juce::MouseEvent&) override { pickRow(row); }
  void returnKeyPressed(int row) override { pickRow(row); }
  void pickRow(int row);
  juce::String getTooltipForRow(int row) override;

  std::vector<Item> items_;
  std::vector<Row> rows_;
  juce::Label heading_;
  juce::TextEditor search_;
  juce::ListBox list_;
};

// The items for a browser: None (when asked), the track's own, the library's, yours.
std::vector<AttachBrowser::Item> attachBrowserItems(const std::vector<doc::AttachInfo>& own, bool none);

class AttachPanel final : public juce::Component {
public:
  struct Start {
    juce::String what;                 // "3 notes", "Clip at bar 9"
    std::string places[3];             // what the first target has now (start, middle, end)
    bool clip = false;                 // clips have an edge length
    int64_t edge = 0;                  // ticks
  };
  std::function<std::vector<doc::AttachInfo>()> attaches;                       // the track's, now
  std::function<std::string(const std::string& recipe, const std::string& name)> onCreate;  // "" = refused
  std::function<juce::String(int place, const std::string& name)> onSet;        // one place, all targets
  std::function<juce::String(int64_t edge)> onEdge;

  explicit AttachPanel(const Start& start);
  void resized() override;
  void paint(juce::Graphics& g) override;

private:
  void browse(int place);              // the library under the fields, for one place (again: closed)
  void picked(int place, const AttachBrowser::Item& item);
  void set(int place, const std::string& name);
  void showFields();
  void fit();

  std::string shown_[3];
  bool clip_ = false;
  int browsing_ = -1;
  juce::Label title_, status_, tip_;
  juce::Label names_[3], edgeName_;
  juce::TextButton fields_[3], clears_[3];
  juce::ComboBox edge_;
  std::unique_ptr<AttachBrowser> browser_;
};

// Edit > Apply attachments... (notes) and the clip menu's Apply attachments... (a track's clips in a section): many at
// once, like Patterns. Where (End, Start, Start + End, Start + Middle + End, Middle), which attachments (picked from the
// library browser: the track's own, the library's, yours), and the pattern (Same, Alternate, Random). Apply writes;
// Again writes another draw.
class AttachPlanPanel final : public juce::Component {
public:
  struct Choice { juce::String label; std::string name, recipe; };  // name: the track's own; recipe: from the library
  // The plan with the track's names ticked, and the library recipes ticked (the host makes them first). Returns the
  // line shown under the buttons.
  std::function<juce::String(const doc::AttachPlan& plan, const std::vector<std::string>& recipes)> onApply;

  AttachPlanPanel(const juce::String& what, std::vector<Choice> choices);
  void resized() override;
  void paint(juce::Graphics& g) override;

private:
  void apply(bool again);
  void showChosen();                   // the chosen ones, each with its ✕
  void fit();
  std::vector<Choice> choices_, chosen_;
  juce::Label title_, whereName_, modeName_, libName_, status_;
  juce::ComboBox where_, mode_;
  juce::OwnedArray<juce::Label> chosenNames_;
  juce::OwnedArray<juce::TextButton> chosenOff_;
  juce::TextButton add_, apply_, again_;
  std::unique_ptr<AttachBrowser> browser_;
  uint64_t seed_ = 1;
};
// Your attachments' folder (%APPDATA%\Core\Presets\attachments) and reading it again into doc::userAttachments.
std::filesystem::path userAttachmentsFolderPath();
void reloadUserAttachments();
// The boxes to tick: the track's attachments, then the library's items the track has none of yet.
std::vector<AttachPlanPanel::Choice> attachChoices(const doc::Document& doc, const std::string& trackId);

}  // namespace core::app
