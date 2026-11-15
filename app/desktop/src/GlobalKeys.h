// The keys that work in every window of Core, whatever has the keyboard focus: today the play / stop key
// (Space by default, File > Settings > Keyboard shortcuts).
//
// JUCE gives a key to the focused component, then to its parents, then stops at its window. So Space did
// nothing in the plugin windows (W-Core, Patterns, Core FX... are windows of their own, not children of the
// main window), and a list, a menu box or a slider holding the focus could keep it from the main window.
// This listens before all of them: it puts itself on the focused component (JUCE asks a component's key
// listeners before the component itself) and on every window of Core (what gets the key when nothing has
// the focus). It lets the key through only while typing in a text box, or while Settings records a new key.
#pragma once
#include <functional>
#include <utility>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

namespace core::app {

class GlobalKeys final : private juce::KeyListener, private juce::FocusChangeListener, private juce::Timer {
 public:
  explicit GlobalKeys(std::function<void()> playStop);
  ~GlobalKeys() override;

  // A window with a play / stop of its own (Patterns plays its pattern alone, looped): the play key there
  // calls this instead of the song's. Forgotten when the window is deleted.
  static void ownPlayStop(juce::Component& window, std::function<void()> playStop);

  // A component that takes every key itself while this is set on it (Settings' key recorder).
  static constexpr const char* kTakesAllKeys = "core.takesAllKeys";

  // True when the key belongs to the focused component, not to Core's global keys: a text box being typed in,
  // or a component (or one of its parents) with kTakesAllKeys.
  static bool focusTakesKeys(juce::Component* focused);

 private:
  bool keyPressed(const juce::KeyPress& key, juce::Component* origin) override;
  bool keyStateChanged(bool isKeyDown, juce::Component* origin) override;
  void globalFocusChanged(juce::Component* focused) override;
  void timerCallback() override;
  void listenToWindows();  // every window of Core on the desktop (the plugin windows are opened at any time)

  static std::vector<std::pair<juce::Component::SafePointer<juce::Component>, std::function<void()>>>& owners();

  std::function<void()> playStop_;
  juce::Component::SafePointer<juce::Component> focused_;           // the focused component it listens on
  std::vector<juce::Component::SafePointer<juce::Component>> windows_;  // the windows it listens on
  int heldCode_ = 0;  // the play key still down: its repeats do not toggle again
};

}  // namespace core::app
