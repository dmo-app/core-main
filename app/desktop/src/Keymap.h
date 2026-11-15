// Keyboard shortcuts, configurable in File > Settings > Keyboard shortcuts (kept in Core.settings as "keymap",
// only the ones changed from the defaults). Every shortcut of the window and of the editors is an action
// here with its keys; the code asks matches(action, key) instead of testing keys itself.
//
// A key is a key code with Ctrl / Shift / Alt, or a modifier alone (code 0) for the "hold" actions (held()).
// Text form, also what the menus and the settings show: "Ctrl+Shift+S", "Space", "PageUp", "Alt".
#pragma once
#include <map>
#include <string>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

namespace core::app {

struct Key {
  int code = 0;          // juce key code (letters upper case); 0: the modifiers alone (a "hold" key)
  bool ctrl = false, shift = false, alt = false;
  bool valid() const { return code != 0 || ctrl || shift || alt; }
  bool operator==(const Key& o) const { return code == o.code && ctrl == o.ctrl && shift == o.shift && alt == o.alt; }
  juce::String text() const;                         // "Ctrl+S"
  static Key parse(const juce::String& text);        // invalid when it can't be read
  static Key of(const juce::KeyPress& press);
};

struct KeyAction {
  std::string id;           // "file.save"
  juce::String name, group;  // shown in the settings
  std::vector<Key> defaults;
  bool hold = false;        // held while dragging (a modifier alone is allowed)
};

class Keymap {
 public:
  static Keymap& get();
  static const std::vector<KeyAction>& actions();
  static const KeyAction* action(const std::string& id);

  void load(juce::PropertySet& settings);
  void save(juce::PropertySet& settings) const;

  const std::vector<Key>& keys(const std::string& id) const;
  void set(const std::string& id, std::vector<Key> keys);
  void reset(const std::string& id);
  void resetAll();
  bool isDefault(const std::string& id) const;

  bool matches(const std::string& id, const juce::KeyPress& press) const;
  std::string actionFor(const Key& key, const std::string& except = {}) const;  // "" none
  bool held(const std::string& id) const;  // a key of this action is down now
  juce::String text(const std::string& id) const;  // the first key ("" none), for the menus

 private:
  Keymap();
  std::map<std::string, std::vector<Key>> keys_;
};

}  // namespace core::app
