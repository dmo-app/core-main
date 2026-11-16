#include "juce_host.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace core::plugins {

namespace {

constexpr int kMaxBlock = 512;   // the most a plugin is prepared for
// The graph calls with sub-blocks of 32 samples or less; a plugin gets them gathered into blocks of 128
// (audit 2.2: 1500 calls a second hurt plugins with FFTs or oversampling). That makes it 128 samples
// late, which latencySamples() reports, so the graph lines the other tracks up and an export still
// starts on the song's first sample.
constexpr int kPluginBlock = 128;
// M4b: an export may gather into bigger blocks (fewer calls; only the file's plugins, never the live ones).
// CORE_EXPORT_PLUGIN_BLOCK=256|512, 128 by default until it is measured with the plugins of the house.
int exportPluginBlock() {
  std::string v;
#ifdef _MSC_VER
  char* buf = nullptr;
  size_t len = 0;
  if (_dupenv_s(&buf, &len, "CORE_EXPORT_PLUGIN_BLOCK") == 0 && buf) { v = buf; free(buf); }
#else
  if (const char* e = std::getenv("CORE_EXPORT_PLUGIN_BLOCK")) v = e;
#endif
  if (!v.empty()) {
    const int n = std::atoi(v.c_str());
    if (n == 128 || n == 256 || n == 512) return n;
  }
  return kPluginBlock;
}
constexpr int kMaxNotes = kMaxPluginEvents * 8;  // per gathered block (each graph call sends at most 1024)

// Runs f on the JUCE message thread and waits (directly when already there).
void runSync(const std::function<void()>& f) {
  auto* mm = juce::MessageManager::getInstance();
  if (mm->isThisTheMessageThread()) { f(); return; }
  juce::MessageManager::callSync(f);
}

std::string utf8(const juce::String& s) { return s.toStdString(); }

std::function<void()>& afterPluginCode() { static std::function<void()> f; return f; }

// What the host is doing with which plugin, one line in the engine's log (stderr), written before the
// plugin's code runs: when a plugin takes the whole process down, the last line says where.
void trace(const std::string& what) {
  std::fprintf(stderr, "plugins: %s\n", what.c_str());
  std::fflush(stderr);
}

#if defined(_MSC_VER)
// M3b: a crash inside a plugin (an access violation) or a C++ exception it throws, caught where it happens,
// so the engine plays on without that plugin. Structured exception handling: MSVC's C++ try (/EHsc) does
// not catch a crash. The function holds no C++ objects, as __try requires.
bool guardedCall(void (*fn)(void*), void* context) {
  __try { fn(context); return true; }
  __except (1 /* EXCEPTION_EXECUTE_HANDLER */) { return false; }
}
#endif

// Plugin code run on the message thread (loading, state, editor): a crash or an exception inside it is
// caught (Windows), so it costs that plugin, not the engine. False if it crashed or threw. Afterwards the
// engine's own crash handler is put back (some plugins install theirs when they load, and then a crash
// elsewhere leaves no dump).
bool guarded(const std::function<void()>& f) {
  bool ok = true;
#if defined(_MSC_VER)
  ok = guardedCall([](void* c) { (*static_cast<const std::function<void()>*>(c))(); }, const_cast<std::function<void()>*>(&f));
#else
  try { f(); } catch (...) { ok = false; }
#endif
  if (afterPluginCode()) afterPluginCode()();
  return ok;
}

class JuceProcessor final : public PluginProcessor, private juce::AudioProcessorParameter::Listener {
public:
  JuceProcessor(std::unique_ptr<juce::AudioPluginInstance> instance, bool instrument, std::atomic<bool>* changed, int block = kPluginBlock)
      : instance_(std::move(instance)), instrument_(instrument), changed_(changed), block_(std::clamp(block, 1, kMaxBlock)) {
    const int channels = std::max({2, instance_->getTotalNumInputChannels(), instance_->getTotalNumOutputChannels()});
    buffer_.setSize(channels, kMaxBlock);
    midi_.ensureSize((size_t)kMaxNotes * 16);
    for (auto* p : instance_->getParameters()) {
      p->addListener(this);
      params_.push_back(p);
    }
  }

  ~JuceProcessor() override {
    for (auto* p : instance_->getParameters()) p->removeListener(this);
    // A plugin is released and deleted on the message thread.
    juce::AudioPluginInstance* raw = instance_.release();
    auto finish = [raw] {
      if (!guarded([raw] { raw->releaseResources(); delete raw; })) trace("a plugin crashed while it was deleted (left as it is)");
    };
    if (juce::MessageManager::getInstance()->isThisTheMessageThread()) finish();
    else juce::MessageManager::callAsync(finish);
  }

  juce::AudioPluginInstance& instance() { return *instance_; }

  // Gathers what the graph sends (audio for an effect, notes for an instrument) and gives back the
  // output of the previous block of 128 (block_): each sample comes out block_ samples after it went in.
  void process(float* left, float* right, int frames, const MidiNote* notes, int count) override {
    if (failed()) return;  // never called again (the graph silences it)
    int done = 0, next = 0;
    while (done < frames) {
      const int n = std::min(frames - done, block_ - fill_);
      // The notes of this part, at their place in the gathered block.
      for (; next < count && notes[next].offset < done + n; ++next) addNote(notes[next], fill_ + std::max(0, notes[next].offset - done));
      for (int i = 0; i < n; ++i) {
        if (!instrument_) { inL_[fill_ + i] = left[done + i]; inR_[fill_ + i] = right[done + i]; }
        if (instrument_) { left[done + i] += outL_[fill_ + i]; right[done + i] += outR_[fill_ + i]; }
        else { left[done + i] = outL_[fill_ + i]; right[done + i] = outR_[fill_ + i]; }
      }
      fill_ += n;
      done += n;
      if (fill_ == block_) { runBlock(); if (failed()) return; }
    }
    for (; next < count; ++next) addNote(notes[next], std::min(fill_, block_ - 1));  // past the end (never sent)
  }

  // The plugin's own latency, and the gathering.
  int latencySamples() const override { return instance_->getLatencySamples() + block_; }

  bool isInstrument() const override { return instrument_; }
  std::string installedVersion() const override { return utf8(instance_->getPluginDescription().version); }

  int parameterIndex(const std::string& id) const override {
    for (size_t i = 0; i < params_.size(); ++i)
      if (parameterId(*params_[i]) == id) return (int)i;
    return -1;
  }
  // The hosted parameter queues the value for the plugin's next block (JUCE caches it, lock free).
  void setParameter(int index, float normalized) override {
    if (index >= 0 && index < (int)params_.size()) params_[(size_t)index]->setValue(std::clamp(normalized, 0.0f, 1.0f));
  }

  // VST3: the ParamID as text; other formats: the index.
  static std::string parameterId(juce::AudioProcessorParameter& p) {
    if (auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*>(&p)) return utf8(hosted->getParameterID());
    return std::to_string(p.getParameterIndex());
  }

private:
  void addNote(const MidiNote& m, int at) {
    // Full (more than 8 graph calls' worth): a note-on is dropped; a note-off always goes in (the buffer is
    // prepared for kMaxNotes; past that JUCE grows it, which is better than a hanging note).
    if (notesInBlock_ >= kMaxNotes && m.on) return;
    at = std::clamp(at, 0, block_ - 1);
    if (m.pitch == kAllNotesOff) midi_.addEvent(juce::MidiMessage::allNotesOff(1), at);
    else if (m.on) midi_.addEvent(juce::MidiMessage::noteOn(1, m.pitch, (juce::uint8)std::clamp((int)std::lround(m.velocity * 127), 1, 127)), at);
    else midi_.addEvent(juce::MidiMessage::noteOff(1, m.pitch), at);
    ++notesInBlock_;
  }

  void runBlock() {
    juce::AudioBuffer<float> view(buffer_.getArrayOfWritePointers(), buffer_.getNumChannels(), block_);
    view.clear();
    if (!instrument_) {
      view.copyFrom(0, 0, inL_, block_);
      if (view.getNumChannels() > 1) view.copyFrom(1, 0, inR_, block_);
    }
#if defined(_MSC_VER)
    view_ = &view;
    if (!guardedCall([](void* self) { auto* p = static_cast<JuceProcessor*>(self); p->instance_->processBlock(*p->view_, p->midi_); }, this)) {
      markFailed(Failure::Crash);  // the graph sees it after this call, counts it and silences the plugin's track
      midi_.clear();
      notesInBlock_ = 0;
      fill_ = 0;
      return;
    }
#else
    instance_->processBlock(view, midi_);  // an exception reaches the graph, which marks the plugin failed
#endif
    const float* l = view.getReadPointer(0);
    const float* r = view.getNumChannels() > 1 ? view.getReadPointer(1) : l;
    std::copy(l, l + block_, outL_);
    std::copy(r, r + block_, outR_);
    midi_.clear();
    notesInBlock_ = 0;
    fill_ = 0;
  }

  // A parameter moved (in the plugin's editor, usually): the document needs this plugin's state again.
  void parameterValueChanged(int, float) override { if (changed_) changed_->store(true); }
  void parameterGestureChanged(int, bool) override {}

  std::unique_ptr<juce::AudioPluginInstance> instance_;
  bool instrument_;
  std::atomic<bool>* changed_;
  std::vector<juce::AudioProcessorParameter*> params_;  // owned by the instance; fixed once it is made
  juce::AudioBuffer<float> buffer_;
  juce::MidiBuffer midi_;
  // Gathering (audio thread only): what came in for the block being filled, and the last block's output.
  const int block_;  // samples per call to the plugin (128; an export may use more, M4b)
  float inL_[kMaxBlock] = {}, inR_[kMaxBlock] = {}, outL_[kMaxBlock] = {}, outR_[kMaxBlock] = {};
  int fill_ = 0, notesInBlock_ = 0;
  juce::AudioBuffer<float>* view_ = nullptr;  // the block being processed (guardedCall)
};

// The plugin's own editor in a window of the engine process; closing it only hides it.
class EditorWindow final : public juce::DocumentWindow {
public:
  EditorWindow(const juce::String& title, juce::AudioProcessorEditor* editor)
      : DocumentWindow(title, juce::Colours::darkgrey, DocumentWindow::closeButton | DocumentWindow::minimiseButton) {
    setUsingNativeTitleBar(true);
    setContentOwned(editor, true);
    setResizable(editor->isResizable(), false);
    centreWithSize(getWidth(), getHeight());
    setAlwaysOnTop(true);
  }
  void closeButtonPressed() override { setVisible(false); }
};

}  // namespace

struct JuceHost::Impl {
  juce::AudioPluginFormatManager formats;
  std::mutex mutex;  // live
  struct Slot {
    std::string identity;
    double sampleRate;
    std::string name;
    std::shared_ptr<JuceProcessor> processor;
  };
  std::map<std::string, Slot> live;
  std::map<std::string, std::unique_ptr<EditorWindow>> editors;  // message thread only
  std::atomic<bool> changed{false};

  // Closes a plugin's window (message thread); its editor is deleted with it, inside the guard: a plugin
  // that crashes while its editor goes (Surge XT's, on the second screen, when its track's instrument
  // changed) costs that window, not the engine. Taken out of the map first, so a crash leaves it intact.
  bool closeWindow(const std::string& key) {
    auto it = editors.find(key);
    if (it == editors.end()) return false;
    EditorWindow* window = it->second.release();
    editors.erase(it);
    if (!window) return false;
    trace("closing the window of " + key);
    if (!guarded([window] { delete window; })) trace("the plugin of " + key + " crashed while its window closed (left as it is)");
    return true;
  }
  void closeAllWindows() {
    std::vector<std::string> keys;
    for (const auto& [k, w] : editors) keys.push_back(k);
    for (const auto& k : keys) closeWindow(k);
  }

  // A new instance with the document's state (message thread).
  // A new instance with the document's state, or with the given state (message thread).
  std::shared_ptr<JuceProcessor> create(const json& plugin, double sampleRate, std::string* why, const juce::MemoryBlock* carried = nullptr, int block = kPluginBlock) {
    const std::string file = plugin.value("file", ""), id = plugin.value("id", "");
    if (file.empty()) { if (why) *why = "the plugin has no file"; return nullptr; }
    juce::OwnedArray<juce::PluginDescription> types;
    for (auto* format : formats.getFormats())
      if (format->getName() == "VST3") format->findAllTypesForFile(types, juce::String::fromUTF8(file.c_str()));
    const juce::PluginDescription* desc = nullptr;
    for (auto* t : types) if (id.empty() || utf8(t->createIdentifierString()) == id) { desc = t; break; }
    if (!desc) { if (why) *why = "not found in " + file; return nullptr; }
    juce::String error;
    std::unique_ptr<juce::AudioPluginInstance> instance = formats.createPluginInstance(*desc, sampleRate, kMaxBlock, error);
    if (!instance) { if (why) *why = utf8(error.isEmpty() ? juce::String("could not be loaded") : error); return nullptr; }
    // Stereo: an instrument plays into two channels, an effect takes and gives two.
    juce::AudioProcessor::BusesLayout layout = instance->getBusesLayout();
    if (!layout.outputBuses.isEmpty()) layout.outputBuses.getReference(0) = juce::AudioChannelSet::stereo();
    if (!layout.inputBuses.isEmpty() && !desc->isInstrument) layout.inputBuses.getReference(0) = juce::AudioChannelSet::stereo();
    if (instance->checkBusesLayoutSupported(layout)) instance->setBusesLayout(layout);
    const std::string state = plugin.value("state", "");
    if (carried && carried->getSize() > 0) instance->setStateInformation(carried->getData(), (int)carried->getSize());
    else if (!state.empty()) {
      juce::MemoryOutputStream data;  // standard base64, as other hosts write it
      if (juce::Base64::convertFromBase64(data, juce::String(state))) instance->setStateInformation(data.getData(), (int)data.getDataSize());
    }
    instance->setNonRealtime(false);
    instance->prepareToPlay(sampleRate, kMaxBlock);
    const bool instrument = desc->isInstrument || (instance->acceptsMidi() && instance->getTotalNumInputChannels() == 0);
    return std::make_shared<JuceProcessor>(std::move(instance), instrument, &changed, block);
  }
};

JuceHost::JuceHost() : impl_(std::make_unique<Impl>()) { juce::addDefaultFormatsToManager(impl_->formats); }

JuceHost::~JuceHost() {
  runSync([this] { impl_->closeAllWindows(); });
  std::lock_guard<std::mutex> g(impl_->mutex);
  impl_->live.clear();
}

std::shared_ptr<PluginProcessor> JuceHost::get(const std::string& key, const json& plugin, double sampleRate, bool offline, std::string* why) {
  // stateId changes when the document chooses a new state for this place (a preset, an undo of one): the
  // instance is made again from the document's state instead of keeping the one it has.
  const std::string identity = plugin.value("file", "") + "|" + plugin.value("id", "") + "|" + plugin.value("stateId", "");
  if (!offline) {
    std::lock_guard<std::mutex> g(impl_->mutex);
    auto it = impl_->live.find(key);
    if (it != impl_->live.end() && it->second.identity == identity && it->second.sampleRate == sampleRate) return it->second.processor;
  }
  // Real time, replacing an instance of the same plugin (another sample rate): it carries on with what
  // that one holds now (changes made in its editor and not saved yet), not the saved state.
  std::shared_ptr<JuceProcessor> previous;
  if (!offline) {
    std::lock_guard<std::mutex> g(impl_->mutex);
    auto it = impl_->live.find(key);
    if (it != impl_->live.end() && it->second.identity == identity) previous = it->second.processor;
  }
  std::shared_ptr<JuceProcessor> made;
  const std::string name = plugin.value("name", std::string("plugin"));
  runSync([&] {
    if (!offline) impl_->closeWindow(key);  // its window belongs to the instance being replaced
    trace("loading '" + name + "' for " + key + (offline ? " (export)" : ""));
    juce::MemoryBlock carried;
    if (previous && !guarded([&] { previous->instance().getStateInformation(carried); })) {
      previous->markFailed(PluginProcessor::Failure::Crash);
      carried.reset();  // from the document's state instead
    }
    if (!guarded([&] { made = impl_->create(plugin, sampleRate, why, carried.getSize() > 0 ? &carried : nullptr, offline ? exportPluginBlock() : kPluginBlock); })) {
      made = nullptr;
      if (why) *why = "it crashed while it was loaded";
    }
    if (made && offline && !guarded([&] { made->instance().setNonRealtime(true); })) made = nullptr;
    trace(made ? "loaded '" + name + "' for " + key : "could not load '" + name + "' for " + key + (why && !why->empty() ? ": " + *why : ""));
  });
  if (made && !offline) {
    std::lock_guard<std::mutex> g(impl_->mutex);
    impl_->live[key] = Impl::Slot{identity, sampleRate, plugin.value("name", ""), made};
  }
  return made;
}

void JuceHost::keepOnly(const std::vector<std::string>& keys) {
  std::vector<std::string> gone;
  {
    std::lock_guard<std::mutex> g(impl_->mutex);
    for (auto it = impl_->live.begin(); it != impl_->live.end();) {
      if (std::find(keys.begin(), keys.end(), it->first) == keys.end()) { gone.push_back(it->first); it = impl_->live.erase(it); }
      else ++it;
    }
  }
  if (gone.empty()) return;
  for (const auto& k : gone) trace("releasing the plugin of " + k);
  // Their windows close first (the editor belongs to the plugin, which is deleted after it).
  juce::MessageManager::callAsync([impl = impl_.get(), gone] { for (const auto& k : gone) impl->closeWindow(k); });
}

void JuceHost::releaseAll() {
  runSync([this] { impl_->closeAllWindows(); });
  std::lock_guard<std::mutex> g(impl_->mutex);
  impl_->live.clear();  // instances still in the playing graph go when that graph does
}

void JuceHost::resetAll() {
  std::map<std::string, std::shared_ptr<JuceProcessor>> slots;
  {
    std::lock_guard<std::mutex> g(impl_->mutex);
    for (auto& [key, slot] : impl_->live) slots[key] = slot.processor;
  }
  // On the message thread, as VST3 wants for setActive (JUCE's reset: off and on again, under the
  // instance's process lock, so the audio thread waits for it instead of playing a plugin half reset).
  runSync([&] {
    for (auto& [key, p] : slots) {
      if (p->failed()) continue;
      if (!guarded([&] { p->instance().reset(); })) {
        trace("the plugin of " + key + " crashed while it was reset: it is silenced (Retry loads it again)");
        p->markFailed(PluginProcessor::Failure::Crash);
      }
    }
  });
}

json JuceHost::captureStates() {
  json out = json::object();
  std::map<std::string, std::shared_ptr<JuceProcessor>> slots;
  {
    std::lock_guard<std::mutex> g(impl_->mutex);
    for (auto& [key, slot] : impl_->live) slots[key] = slot.processor;
  }
  runSync([&] {
    for (auto& [key, p] : slots) {
      if (p->failed()) continue;  // a plugin that crashed: the document keeps the last state it had
      juce::MemoryBlock data;
      if (!guarded([&] { p->instance().getStateInformation(data); })) {
        trace("the plugin of " + key + " crashed while it gave its state: it is silenced (Retry loads it again)");
        p->markFailed(PluginProcessor::Failure::Crash);
        continue;
      }
      out[key] = utf8(juce::Base64::toBase64(data.getData(), data.getSize()));
    }
  });
  return out;
}

bool JuceHost::showEditor(const std::string& key, bool open, std::string* why) {
  std::shared_ptr<JuceProcessor> p;
  std::string name;
  {
    std::lock_guard<std::mutex> g(impl_->mutex);
    auto it = impl_->live.find(key);
    if (it != impl_->live.end()) { p = it->second.processor; name = it->second.name; }
  }
  if (!p) { if (why) *why = "no plugin loaded for " + key; return false; }
  bool ok = true;
  runSync([&] {
    auto& window = impl_->editors[key];
    if (!open) { if (window) window->setVisible(false); return; }
    if (!window) {
      trace("opening the window of '" + name + "' (" + key + ")");
      juce::AudioProcessorEditor* editor = nullptr;
      if (!guarded([&] { editor = p->instance().hasEditor() ? p->instance().createEditorIfNeeded() : nullptr; })) {
        p->markFailed(PluginProcessor::Failure::Crash);
        impl_->editors.erase(key);
        if (why) *why = "the plugin crashed while it opened its window";
        ok = false;
        return;
      }
      if (!editor) editor = new juce::GenericAudioProcessorEditor(p->instance());
      window = std::make_unique<EditorWindow>(juce::String::fromUTF8(name.empty() ? key.c_str() : name.c_str()), editor);
    }
    window->setVisible(true);
    window->toFront(true);
  });
  return ok;
}

bool JuceHost::takeChanged() { return impl_->changed.exchange(false); }

bool JuceHost::setParameter(const std::string& key, int index, float normalized) {
  std::shared_ptr<JuceProcessor> p;
  {
    std::lock_guard<std::mutex> g(impl_->mutex);
    auto it = impl_->live.find(key);
    if (it != impl_->live.end()) p = it->second.processor;
  }
  if (!p) return false;
  bool ok = false;
  runSync([&] {
    auto params = p->instance().getParameters();
    if (index < 0 || index >= params.size()) return;
    params[index]->setValueNotifyingHost(normalized);
    ok = true;
  });
  return ok;
}

json JuceHost::parameters(const std::string& key) {
  std::shared_ptr<JuceProcessor> p;
  {
    std::lock_guard<std::mutex> g(impl_->mutex);
    auto it = impl_->live.find(key);
    if (it != impl_->live.end()) p = it->second.processor;
  }
  json out = json::array();
  if (!p) return out;
  runSync([&] {
    for (auto* param : p->instance().getParameters()) {
      const juce::String name = param->getName(64).trim();
      if (name.isEmpty() || !param->isAutomatable()) continue;  // hidden ones, and those the plugin says not to automate
      out.push_back({{"id", JuceProcessor::parameterId(*param)}, {"name", utf8(name)}, {"value", param->getValue()},
                     {"default", param->getDefaultValue()}, {"steps", param->isDiscrete() ? param->getNumSteps() : 0},
                     {"text", utf8(param->getCurrentValueAsText())}});
    }
  });
  return out;
}

std::string JuceHost::vstPreset(const std::string& key) {
  std::shared_ptr<JuceProcessor> p;
  {
    std::lock_guard<std::mutex> g(impl_->mutex);
    auto it = impl_->live.find(key);
    if (it != impl_->live.end()) p = it->second.processor;
  }
  std::string out;
  if (!p || p->failed()) return out;
  runSync([&] {
    juce::MemoryBlock preset;
    // JUCE 9: getVST3Client() (null when the plugin is not VST3); the ExtensionsVisitor is gone.
    if (!guarded([&] { if (auto* c = p->instance().getVST3Client()) preset = c->getPreset(); })) {
      p->markFailed(PluginProcessor::Failure::Crash);
      return;
    }
    if (preset.getSize() > 0) out = utf8(juce::Base64::toBase64(preset.getData(), preset.getSize()));
  });
  return out;
}

size_t JuceHost::saveVstPreset(const std::string& key, const std::string& fileUtf8) {
  const std::string preset = vstPreset(key);
  juce::MemoryOutputStream bytes;
  if (preset.empty() || !juce::Base64::convertFromBase64(bytes, juce::String::fromUTF8(preset.c_str()))) return 0;
  if (!juce::File(juce::String::fromUTF8(fileUtf8.c_str())).replaceWithData(bytes.getData(), bytes.getDataSize())) return 0;
  return bytes.getDataSize();
}

void setAfterPluginCode(std::function<void()> f) { afterPluginCode() = std::move(f); }

namespace {
juce::ScopedJuceInitialiser_GUI* runtime = nullptr;
}

Runtime::Runtime() { if (!runtime) runtime = new juce::ScopedJuceInitialiser_GUI(); }
Runtime::~Runtime() { delete runtime; runtime = nullptr; }

int runWithMessageLoop(const std::function<int()>& body) {
  int result = 0;
  std::thread worker([&] {
    result = body();
    juce::MessageManager::callAsync([] { juce::MessageManager::getInstance()->stopDispatchLoop(); });
  });
  juce::MessageManager::getInstance()->runDispatchLoop();
  worker.join();
  return result;
}

json scanFile(const std::string& path) {
  juce::AudioPluginFormatManager formats;
  juce::addDefaultFormatsToManager(formats);
  juce::OwnedArray<juce::PluginDescription> types;
  for (auto* format : formats.getFormats())
    if (format->getName() == "VST3") format->findAllTypesForFile(types, juce::String::fromUTF8(path.c_str()));
  json out = json::array();
  for (auto* t : types)
    out.push_back({{"format", "vst3"}, {"id", utf8(t->createIdentifierString())}, {"name", utf8(t->name)}, {"vendor", utf8(t->manufacturerName)},
                   {"version", utf8(t->version)}, {"file", utf8(t->fileOrIdentifier)}, {"category", utf8(t->category)},
                   {"instrument", t->isInstrument}, {"inputs", t->numInputChannels}, {"outputs", t->numOutputChannels}});
  return out;
}

} // namespace core::plugins
