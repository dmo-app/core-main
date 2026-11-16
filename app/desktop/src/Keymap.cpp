#include "Keymap.h"

#include <algorithm>
#include "json.hpp"

namespace core::app {
namespace {

struct Named { int code; const char* name; };
const std::vector<Named>& named() {
  static const std::vector<Named> n = {
      {juce::KeyPress::spaceKey, "Space"},       {juce::KeyPress::returnKey, "Enter"},      {juce::KeyPress::tabKey, "Tab"},
      {juce::KeyPress::deleteKey, "Delete"},     {juce::KeyPress::backspaceKey, "Backspace"}, {juce::KeyPress::insertKey, "Insert"},
      {juce::KeyPress::escapeKey, "Esc"},        {juce::KeyPress::homeKey, "Home"},         {juce::KeyPress::endKey, "End"},
      {juce::KeyPress::pageUpKey, "PageUp"},     {juce::KeyPress::pageDownKey, "PageDown"}, {juce::KeyPress::leftKey, "Left"},
      {juce::KeyPress::rightKey, "Right"},       {juce::KeyPress::upKey, "Up"},             {juce::KeyPress::downKey, "Down"},
      {juce::KeyPress::F1Key, "F1"},   {juce::KeyPress::F2Key, "F2"},   {juce::KeyPress::F3Key, "F3"},   {juce::KeyPress::F4Key, "F4"},
      {juce::KeyPress::F5Key, "F5"},   {juce::KeyPress::F6Key, "F6"},   {juce::KeyPress::F7Key, "F7"},   {juce::KeyPress::F8Key, "F8"},
      {juce::KeyPress::F9Key, "F9"},   {juce::KeyPress::F10Key, "F10"}, {juce::KeyPress::F11Key, "F11"}, {juce::KeyPress::F12Key, "F12"},
      {'+', "Plus"},
  };
  return n;
}

int upper(int code) { return code >= 'a' && code <= 'z' ? code - 'a' + 'A' : code; }

Key k(const char* text) { return Key::parse(text); }

}  // namespace

juce::String Key::text() const {
  juce::StringArray parts;
  if (ctrl) parts.add("Ctrl");
  if (shift) parts.add("Shift");
  if (alt) parts.add("Alt");
  if (code != 0) {
    juce::String name;
    for (const auto& n : named()) if (n.code == code) name = n.name;
    if (name.isEmpty()) name = juce::String::charToString((juce::juce_wchar)upper(code));
    parts.add(name);
  }
  return parts.joinIntoString("+");
}

Key Key::parse(const juce::String& text) {
  Key key;
  juce::StringArray parts;
  parts.addTokens(text.trim(), "+", "");
  for (int i = 0; i < parts.size(); ++i) {
    const juce::String p = parts[i].trim();
    if (p.isEmpty()) continue;
    if (p.equalsIgnoreCase("ctrl") || p.equalsIgnoreCase("control") || p.equalsIgnoreCase("cmd")) { key.ctrl = true; continue; }
    if (p.equalsIgnoreCase("shift")) { key.shift = true; continue; }
    if (p.equalsIgnoreCase("alt")) { key.alt = true; continue; }
    if (key.code != 0) return {};  // two keys
    for (const auto& n : named()) if (p.equalsIgnoreCase(n.name)) key.code = n.code;
    if (key.code == 0 && p.length() == 1) key.code = upper((int)p[0]);
    if (key.code == 0) return {};
  }
  return key;
}

Key Key::of(const juce::KeyPress& press) {
  Key key;
  key.code = upper(press.getKeyCode());
  const auto m = press.getModifiers();
  key.ctrl = m.isCtrlDown() || m.isCommandDown();
  key.shift = m.isShiftDown();
  key.alt = m.isAltDown();
  return key;
}

const std::vector<KeyAction>& Keymap::actions() {
  static const std::vector<KeyAction> all = {
      {"transport.playStop", "Play / stop", "Transport", {k("Space")}},
      {"transport.home", "Go to the start", "Transport", {k("Home")}},
      {"transport.end", "Go to the end", "Transport", {k("End")}},
      {"transport.loop", "Loop on / off", "Transport", {k("L")}},
      {"transport.loopSelection", "Loop around the selected clips", "Transport", {k("Ctrl+L")}},
      {"transport.loopHold", "Hold to mark a loop on the ruler (drag with the left mouse button)", "Transport", {k("Ctrl")}, true},
      {"transport.barBack", "Back one bar", "Transport", {k("Left")}},
      {"transport.barForward", "Forward one bar", "Transport", {k("Right")}},
      {"transport.beatBack", "Back one beat", "Transport", {k("Shift+Left")}},
      {"transport.beatForward", "Forward one beat", "Transport", {k("Shift+Right")}},
      {"transport.bar4Back", "Back four bars", "Transport", {k("Ctrl+Left")}},
      {"transport.bar4Forward", "Forward four bars", "Transport", {k("Ctrl+Right")}},
      {"view.pageBack", "View one screen back", "View", {k("PageUp")}},
      {"view.pageForward", "View one screen on", "View", {k("PageDown")}},
      {"view.wholeSong", "View the whole song", "View", {k("Ctrl+0")}},
      {"view.panHold", "Hold to drag the view sideways (with the left mouse button)", "View", {k("Alt")}, true},
      {"view.mixer", "Mixer", "View", {k("Ctrl+M")}},
      {"view.patterns", "Patterns window (channel rack, templates)", "View", {k("Ctrl+P")}},
      {"view.effects", "Effects of the selected track", "View", {k("Ctrl+F")}},
      {"file.new", "New empty song", "File", {k("Ctrl+N")}},
      {"file.newTemplate", "Templates (in the Library)", "File", {k("Ctrl+Shift+N")}},
      {"file.open", "Open", "File", {k("Ctrl+O")}},
      {"file.save", "Save", "File", {k("Ctrl+S")}},
      {"file.saveAs", "Save as", "File", {k("Ctrl+Shift+S")}},
      {"file.export", "Export WAV", "File", {k("Ctrl+E")}},
      {"file.importAudio", "Import audio", "File", {k("Ctrl+I")}},
      {"file.settings", "Settings", "File", {k("Ctrl+,")}},
      {"edit.undo", "Undo", "Edit", {k("Ctrl+Z")}},
      {"edit.redo", "Redo", "Edit", {k("Ctrl+Y"), k("Ctrl+Shift+Z")}},
      {"edit.addTrack", "Add a track", "Edit", {k("Ctrl+T")}},
      {"edit.delete", "Delete the selection (clip, notes, audio)", "Edit", {k("Delete"), k("Backspace")}},
      {"edit.copy", "Copy the selected clips", "Edit", {k("Ctrl+C")}},
      {"edit.cut", "Cut the selected clips", "Edit", {k("Ctrl+X")}},
      {"edit.paste", "Paste the copied clips at the playhead", "Edit", {k("Ctrl+V")}},
      {"edit.duplicate", "Duplicate the selected notes or clips (right after them)", "Edit", {k("Ctrl+D")}},
      {"edit.selectAll", "Select all notes", "Edit", {k("Ctrl+A")}},
      {"keys.octaveUp", "Typing keyboard: octave up", "Notes", {k("Up")}},
      {"keys.octaveDown", "Typing keyboard: octave down", "Notes", {k("Down")}},
  };
  return all;
}

const KeyAction* Keymap::action(const std::string& id) {
  for (const auto& a : actions()) if (a.id == id) return &a;
  return nullptr;
}

Keymap& Keymap::get() {
  static Keymap map;
  return map;
}

Keymap::Keymap() { resetAll(); }

void Keymap::resetAll() {
  keys_.clear();
  for (const auto& a : actions()) keys_[a.id] = a.defaults;
}

void Keymap::reset(const std::string& id) {
  if (const KeyAction* a = action(id)) keys_[id] = a->defaults;
}

bool Keymap::isDefault(const std::string& id) const {
  const KeyAction* a = action(id);
  return a == nullptr || keys(id) == a->defaults;
}

const std::vector<Key>& Keymap::keys(const std::string& id) const {
  static const std::vector<Key> none;
  const auto it = keys_.find(id);
  return it == keys_.end() ? none : it->second;
}

void Keymap::set(const std::string& id, std::vector<Key> keys) {
  if (action(id) == nullptr) return;
  keys.erase(std::remove_if(keys.begin(), keys.end(), [](const Key& key) { return !key.valid(); }), keys.end());
  keys_[id] = std::move(keys);
}

void Keymap::load(juce::PropertySet& settings) {
  resetAll();
  const nlohmann::json j = nlohmann::json::parse(settings.getValue("keymap", "{}").toStdString(), nullptr, false);
  if (!j.is_object()) return;
  for (auto it = j.begin(); it != j.end(); ++it) {
    const KeyAction* a = action(it.key());
    if (a == nullptr || !it.value().is_array()) continue;  // an action that no longer exists
    std::vector<Key> keys;
    for (const auto& t : it.value())
      if (t.is_string())
        if (const Key key = Key::parse(juce::String::fromUTF8(t.get<std::string>().c_str())); key.valid() && (key.code != 0 || a->hold)) keys.push_back(key);
    keys_[a->id] = keys;
  }
}

void Keymap::save(juce::PropertySet& settings) const {
  nlohmann::json j = nlohmann::json::object();
  for (const auto& a : actions()) {
    if (isDefault(a.id)) continue;
    nlohmann::json list = nlohmann::json::array();
    for (const Key& key : keys(a.id)) list.push_back(key.text().toStdString());
    j[a.id] = list;
  }
  if (j.empty()) settings.removeValue("keymap");
  else settings.setValue("keymap", juce::String::fromUTF8(j.dump().c_str()));
}

bool Keymap::matches(const std::string& id, const juce::KeyPress& press) const {
  const Key key = Key::of(press);
  for (const Key& k2 : keys(id)) if (k2.code != 0 && k2 == key) return true;
  return false;
}

std::string Keymap::actionFor(const Key& key, const std::string& except) const {
  for (const auto& a : actions())
    if (a.id != except)
      for (const Key& k2 : keys(a.id)) if (k2 == key) return a.id;
  return {};
}

bool Keymap::held(const std::string& id) const {
  const auto m = juce::ModifierKeys::getCurrentModifiersRealtime();
  for (const Key& key : keys(id)) {
    if ((key.ctrl && !(m.isCtrlDown() || m.isCommandDown())) || (key.shift && !m.isShiftDown()) || (key.alt && !m.isAltDown())) continue;
    if (key.code == 0 || juce::KeyPress::isKeyCurrentlyDown(key.code)) return true;
  }
  return false;
}

juce::String Keymap::text(const std::string& id) const {
  const auto& list = keys(id);
  for (const Key& key : list) if (key.code != 0) return key.text();
  return list.empty() ? juce::String() : list.front().text();
}

}  // namespace core::app
