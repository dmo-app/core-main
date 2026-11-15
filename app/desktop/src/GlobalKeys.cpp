#include "GlobalKeys.h"

#include <algorithm>

#include "Keymap.h"

namespace core::app {

GlobalKeys::GlobalKeys(std::function<void()> playStop) : playStop_(std::move(playStop)) {
  juce::Desktop::getInstance().addFocusChangeListener(this);
  globalFocusChanged(juce::Component::getCurrentlyFocusedComponent());
  startTimer(300);  // windows opened without taking the focus, and the play key let go elsewhere
}

GlobalKeys::~GlobalKeys() {
  stopTimer();
  juce::Desktop::getInstance().removeFocusChangeListener(this);
  if (focused_ != nullptr) focused_->removeKeyListener(this);
  for (auto& w : windows_)
    if (w != nullptr) w->removeKeyListener(this);
}

std::vector<std::pair<juce::Component::SafePointer<juce::Component>, std::function<void()>>>& GlobalKeys::owners() {
  static std::vector<std::pair<juce::Component::SafePointer<juce::Component>, std::function<void()>>> o;
  return o;
}

void GlobalKeys::ownPlayStop(juce::Component& window, std::function<void()> playStop) {
  auto& o = owners();
  o.erase(std::remove_if(o.begin(), o.end(), [&](const auto& e) { return e.first == nullptr || e.first == &window; }), o.end());
  if (playStop) o.emplace_back(&window, std::move(playStop));
}

bool GlobalKeys::focusTakesKeys(juce::Component* focused) {
  for (auto* c = focused; c != nullptr; c = c->getParentComponent()) {
    if (c->getProperties()[kTakesAllKeys]) return true;
    if (auto* text = dynamic_cast<juce::TextEditor*>(c); text != nullptr && !text->isReadOnly()) return true;
  }
  return false;
}

bool GlobalKeys::keyPressed(const juce::KeyPress& key, juce::Component* origin) {
  if (!Keymap::get().matches("transport.playStop", key)) return false;
  if (focusTakesKeys(juce::Component::getCurrentlyFocusedComponent())) return false;  // a space in the text
  if (heldCode_ == key.getKeyCode()) return true;  // the key held down repeats: one toggle per press
  heldCode_ = key.getKeyCode();
  // The window the key came from may have a play of its own; otherwise the song's.
  for (auto* c = origin; c != nullptr; c = c->getParentComponent())
    for (const auto& [window, own] : owners())
      if (window == c) { const auto play = own; play(); return true; }  // a copy: it may change the list
  if (playStop_) playStop_();
  return true;
}

bool GlobalKeys::keyStateChanged(bool isKeyDown, juce::Component*) {
  if (!isKeyDown && heldCode_ != 0 && !juce::KeyPress::isKeyCurrentlyDown(heldCode_)) heldCode_ = 0;
  return false;  // the others still see the key going up or down
}

void GlobalKeys::globalFocusChanged(juce::Component* focused) {
  if (focused_ != focused) {
    if (focused_ != nullptr && std::find(windows_.begin(), windows_.end(), focused_) == windows_.end())
      focused_->removeKeyListener(this);
    focused_ = focused;
    if (focused != nullptr) focused->addKeyListener(this);  // asked before the component's own keyPressed
  }
  listenToWindows();
}

void GlobalKeys::timerCallback() {
  if (heldCode_ != 0 && !juce::KeyPress::isKeyCurrentlyDown(heldCode_)) heldCode_ = 0;
  listenToWindows();
}

void GlobalKeys::listenToWindows() {
  windows_.erase(std::remove_if(windows_.begin(), windows_.end(), [](const auto& w) { return w == nullptr; }), windows_.end());
  auto& desktop = juce::Desktop::getInstance();
  for (int i = 0; i < desktop.getNumComponents(); ++i) {
    auto* c = desktop.getComponent(i);
    // Core's windows only (main, plugins, Settings): not the menus, which take Space to pick an item.
    if (dynamic_cast<juce::ResizableWindow*>(c) == nullptr) continue;
    if (std::find(windows_.begin(), windows_.end(), c) != windows_.end()) continue;
    c->addKeyListener(this);
    windows_.emplace_back(c);
  }
}

}  // namespace core::app
