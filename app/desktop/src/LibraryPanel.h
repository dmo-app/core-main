// The Library, on the left of the window: the sample library (search, roles, favourites, a tree of sounds and a
// preview card) and the song templates (a tree by style, a card to start a song, save the song as one or delete one;
// since 2026-11-05 they live only here, not in File > New). It only reads the index; what a click does (a sample on the
// selected track, a new song from a template, a correction of what a sample is) is the window's, through the
// callbacks: a song change is one undo step of the song, a correction one step of the sample catalogue.
#pragma once
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>
#include "Theme.h"
#include "Waveforms.h"
#include "assets.hpp"
#include "samples.hpp"
#include "pattern_tools.hpp"

namespace core::app {

class LibraryPanel final : public juce::Component, private juce::ListBoxModel {
public:
  // A song template. kBasicTemplate is Core's own starting song (kick, bass and hats), not a file.
  // fromSong: made by analysing a song (AICORE writes source.kind "analysis", AICORE#37); empty style = "No style".
  struct Preset { std::string id, name, style; double bpm = 0; bool builtin = true; bool fromSong = false; };
  static constexpr const char* kBasicTemplate = "core:basic";

  LibraryPanel();
  ~LibraryPanel() override;

  // What the window gives: the index (null while there is none), the selected track and whether it plays
  // samples, the templates, waveforms, favourites (sha256) kept in the user settings.
  std::function<const doc::SampleIndex*()> index;
  std::function<juce::String()> trackName;           // "" = no track selected
  std::function<bool()> trackTakesSamples;
  // The selected track's sample role (doc::trackSampleRole; "" when it takes no samples) and the song's fastest
  // tempo: the "fits" filter shows only the sounds that suit it (doc::sampleFit).
  std::function<std::pair<std::string, double>()> fitContext;
  std::function<std::vector<Preset>()> presets;
  std::function<std::shared_ptr<const Waveform>(const doc::LibrarySample&)> waveform;
  std::function<std::set<std::string>()> loadFavourites;
  std::function<void(const std::set<std::string>&)> saveFavourites;
  // What it asks for.
  // A sound dragged out of the list: this text, then the sample's sha256 (the window finds it in the index).
  static constexpr const char* kSampleDrag = "core-sample:";
  std::function<void()> onMinimise;  // ‹ in its header: the Library steps aside (the rail brings it back)
  std::function<void(const doc::LibrarySample&)> onUseSample;  // on the selected track (one undo step), then heard
  // Heard once, whatever track is selected (the song does not change); role "" = the sample's own role.
  std::function<bool(const doc::LibrarySample&, const std::string& role)> onPreviewSample;
  // Templates tab: a new song from one; the song as it is now saved as one (the window asks the name); one deleted
  // (the window asks first); the hidden built-in ones back; the user's templates folder opened.
  std::function<void(const std::string& templateId)> onOpenPreset;
  std::function<void()> onSaveTemplate, onRestoreTemplates, onOpenTemplatesFolder;
  std::function<void(const std::string& templateId)> onDeleteTemplate;
  std::function<void()> onChooseFolder;               // no library yet: Samples > Choose the library folder
  // The user corrects what a sample is (fields as in doc::setSampleMetadata; null clears). "" or why it was refused.
  std::function<std::string(const std::string& sha256, const doc::json& fields)> onEditMetadata;
  // The Project tab: the song's own audio files (imported, or made by Core from another: reversed, stretched...).
  std::function<const doc::Document*()> document;
  std::function<std::filesystem::path()> projectFolder;  // empty while the song is not saved
  std::function<void()> onRemoveUnused;  // the window asks, then removes (one undo step) and sends the files to the Recycle Bin
  // Right click on a sound: a copy of it on a new audio track of the song, to edit (the library's file stays).
  std::function<void(const doc::LibrarySample&)> onEditSample;
  // Right click on one of the song's audio files (Project tab): on a new track, or saved as a sample (the dialog).
  std::function<void(const std::string& assetId)> onAssetToTrack, onSaveAsset;

  // The Patterns tab (2026-11-05): the pattern library (Core's and Diogo's own), any pattern on any track. A click
  // chooses; the play button (or a double click) plays it on the selected track over 1, 4, 8 or 16 bars without
  // changing the song; Apply puts it on the track from the first bar shown in the piano roll (one undo step).
  std::function<std::vector<doc::PatternPreset>()> patterns;
  std::function<void(const doc::PatternPreset&, int bars)> onPreviewPattern;
  std::function<void()> onStopPreview;
  std::function<void(const doc::PatternPreset&, int bars, bool add)> onApplyPattern;
  void patternsChanged();  // one was saved: the list is read again

  void poll();  // now and then: the Project tab follows the song, the "fits" filter its track and tempo

  void indexChanged();  // a scan ended, or the index was loaded
  void catalogChanged();  // what samples are changed (same samples): the chosen one stays chosen
  void refresh();       // the selected track changed: the preview card's button follows it
  void templatesChanged();  // a template was saved, deleted or restored: the Templates tab reads them again
  void showTemplates();     // opens the Templates tab (Ctrl+Shift+N)

  void paint(juce::Graphics& g) override;
  void paintOverChildren(juce::Graphics& g) override;
  void resized() override;

private:
  enum class Tab { Sounds, Templates, Grooves, Project };
  struct Row {
    bool group = false;
    juce::String label;
    int count = 0;         // samples under a group
    int sample = -1;       // index in the library's samples
    int preset = -1;       // Templates tab
    int pattern = -1;      // Patterns tab: index in patternList_
    std::string asset;     // Project tab: an asset id, or
    std::string file;      // a file in the project's Audio folder that the song does not use
  };
  int getNumRows() override;
  void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override;
  void listBoxItemClicked(int row, const juce::MouseEvent& e) override;
  void listBoxItemDoubleClicked(int row, const juce::MouseEvent& e) override;
  void selectedRowsChanged(int row) override;
  juce::var getDragSourceDescription(const juce::SparseSet<int>& rows) override;  // a sound dragged onto a track

  void setTab(Tab tab);
  void rebuild();
  const doc::LibrarySample* sample(int i) const;
  const doc::LibrarySample* current() const { return sample(currentSample_); }
  void toggleFavourite(const std::string& sha);
  juce::Rectangle<int> previewArea() const;
  juce::Rectangle<int> rolesArea() const;

  Tab tab_ = Tab::Sounds;
  juce::TextEditor search_;
  juce::TextButton minimise_;
  juce::TextButton sounds_, presetsTab_, grooves_, projectTab_, removeUnused_;
  juce::TextButton newSong_{"New song"}, saveTemplate_, deleteTemplate_;  // Templates tab
  theme::InfoDot templatesInfo_;
  std::string currentTemplate_;        // Templates tab: the chosen template's id
  const Preset* currentPreset() const;
  void templateMenu(const Preset* p);
  void paintTemplateCard(juce::Graphics& g, juce::Rectangle<int> card);
  juce::String groupKey(const Row& r) const;
  std::vector<std::unique_ptr<juce::TextButton>> roles_;
  std::vector<std::unique_ptr<juce::TextButton>> soundChips_;  // short, long, bright, dark, sub (from the measurements)
  juce::TextButton fit_{"fits"};       // only what suits the selected track at the song's tempo (on by default)
  std::string fitSeen_;                // the track role and tempo the list was made for
  std::pair<std::string, double> fitNow() const { return fitContext ? fitContext() : std::pair<std::string, double>{}; }
  juce::ListBox list_{"Library", this};
  void editMetadata();
  juce::TextButton play_, use_{"Use"}, star_, edit_;
  void preview();
  std::vector<Row> rows_;
  std::vector<Preset> presetList_;
  std::vector<doc::PatternPreset> patternList_;
  int currentPattern_ = -1;
  int previewBars_ = 4;
  juce::TextButton bars_[4], playPattern_, applyPattern_, addPattern_;
  const doc::PatternPreset* currentPatternPreset() const;
  void paintPatternCard(juce::Graphics& g, juce::Rectangle<int> card);
  std::set<std::string> collapsed_;    // groups closed in the tree
  std::set<std::string> favourites_;
  int currentSample_ = -1;
  std::string currentProjectItem_;     // Project tab: the chosen asset id or file
  std::string projectSeen_;            // what the Project tab shows (redrawn when it changes)
  int unusedCount_ = 0;
  std::string projectSignature() const;
  void paintProjectCard(juce::Graphics& g, juce::Rectangle<int> card);

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LibraryPanel)
};

} // namespace core::app
