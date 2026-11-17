// Testes do dominio (core_domain): o documento da app, guardar/reabrir, undo/redo, templates e a projeccao para o motor.
#include <filesystem>
#include "support/check.hpp"
#include "core/project.hpp"
#include "core/server.hpp"
#include "edit.hpp"
#include "history.hpp"
#include "plugin_refs.hpp"
#include "storage.hpp"
#include "templates.hpp"

using namespace core::doc;
using coretest::cases;
namespace fs = std::filesystem;

namespace {
fs::path tempDir() {
  fs::path d = fs::temp_directory_path() / "core-domain-tests";
  fs::create_directories(d);
  return d;
}
std::string canon(const Document& d) { return toJson(d).dump(); }
}  // namespace

TEST("documento: novo documento vazio e valido e estavel") {
  Document d = newEmptyDocument();
  CHECK_EQ(d.tracks.size(), (size_t)0);
  CHECK_EQ(canon(parseDocument(toJson(d))), canon(d));
}

TEST("documento: o template basico tem kick, bass e hats a espera de plugin") {
  Document d = newTemplateDocument();
  CHECK_EQ(d.tracks.size(), (size_t)3);
  for (const auto& t : d.tracks) CHECK(!t.patterns.empty() && !t.patterns[0].notes.empty());
}

TEST("guardar/reabrir: o ficheiro reaberto e igual ao guardado") {
  Document d = newTemplateDocument();
  d.name = "Teste guardar";
  d.bpm = 188;
  const fs::path file = tempDir() / ("reabrir" + std::string(kFileExtension));
  fs::remove(file);
  saveFile(d, file);
  CHECK(fs::exists(file));
  OpenResult r = openFile(file);
  CHECK(!r.imported);
  CHECK_EQ(canon(r.document), canon(d));
  // Guardar por cima deixa o anterior em .bak.
  d.bpm = 200;
  saveFile(d, file);
  CHECK(fs::exists(backupPath(file)));
  CHECK_NEAR(openFile(file).document.bpm, 200, 0);
}

TEST("guardar/reabrir: ficheiro estragado da erro e nao apaga nada") {
  const fs::path file = tempDir() / ("estragado" + std::string(kFileExtension));
  { std::FILE* f = std::fopen(file.string().c_str(), "wb"); std::fputs("{ isto nao e json", f); std::fclose(f); }
  CHECK_THROWS(openFile(file));
  CHECK(fs::exists(file));
}

TEST("undo/redo: adicionar e remover pistas volta exactamente atras") {
  Document start = newEmptyDocument();
  History h(start);
  CHECK(!h.canUndo());
  Document a = h.current();
  const std::string id = addTrack(a, "kick", "Kick");
  h.commit(a, "adicionar pista");
  Document b = h.current();
  addTrack(b, "bass", "Bass");
  h.commit(b, "adicionar pista");
  CHECK_EQ(h.current().tracks.size(), (size_t)2);
  CHECK(h.undo());
  CHECK_EQ(canon(h.current()), canon(a));
  CHECK(h.undo());
  CHECK_EQ(canon(h.current()), canon(start));
  CHECK(!h.canUndo());
  CHECK(h.redo());
  CHECK(h.redo());
  CHECK_EQ(canon(h.current()), canon(b));
  CHECK(!h.canRedo());
  // Uma alteracao nova depois de undo apaga o redo.
  CHECK(h.undo());
  Document c = h.current();
  removeTrack(c, id);
  h.commit(c, "remover pista");
  CHECK(!h.canRedo());
  CHECK_EQ(h.current().tracks.size(), (size_t)0);
}

TEST("undo/redo: um gesto (arrastar) e um so passo") {
  History h(newEmptyDocument());
  h.beginGesture("bpm");
  for (double bpm : {150.0, 160.0, 170.0}) { Document d = h.current(); d.bpm = bpm; h.updateGesture(d); }
  h.endGesture();
  CHECK_NEAR(h.current().bpm, 170, 0);
  CHECK(h.undo());
  CHECK_NEAR(h.current().bpm, 145, 0);
  CHECK(!h.canUndo());
}

TEST("modificado: guardar limpa, editar suja, undo ate ao guardado limpa") {
  History h(newEmptyDocument());
  h.markSaved();
  CHECK(!h.modified());
  Document d = h.current(); d.bpm = 190; h.commit(d, "bpm");
  CHECK(h.modified());
  h.undo();
  CHECK(!h.modified());
}

// Todos os templates que vao com a app abrem, viram projecto do motor, e o motor aceita-os e renderiza sem NaN.
TEST("templates: todos abrem e o motor toca-os sem erro") {
  TemplateLibrary lib = loadTemplates(CORE_TEMPLATES_DIR, tempDir() / "user-templates-vazia", true);
  for (const auto& p : lib.problems) coretest::fail(p.file.string().c_str(), 0, p.message);
  CHECK(lib.templates.size() >= 1);
  for (const auto& e : lib.templates) {
    Document d = documentFromTemplate(e.tmpl);
    core::Project p;
    try {
      p = core::fromJson(toProjection(d));
    } catch (const std::exception& ex) {
      coretest::fail(e.file.string().c_str(), 0, std::string("o motor recusou: ") + ex.what());
      continue;
    }
    p.lengthTicks = std::min<int64_t>(p.lengthTicks, 4 * 4 * core::kPpq);  // 4 compassos chegam
    core::RenderResult r = core::renderProject(p, 48000, 512, false, 0.1, nullptr, nullptr, nullptr, 0);
    bool finite = true;
    for (float s : r.L) finite = finite && std::isfinite(s);
    CHECK(finite);
  }
}

TEST("plugins por fabricante: todos entram num grupo, sem fabricante vai para Other, ordem alfabetica") {
  const json plugins = json::array({
      {{"format", "vst3"}, {"id", "VST3-Snap Heap-aaaa-1111"}, {"name", "Snap Heap"}, {"vendor", "KiloHearts"}, {"instrument", false}},
      {{"format", "vst3"}, {"id", "VST3-Multipass-bbbb-2222"}, {"name", "Multipass"}, {"vendor", "kiloHearts"}, {"instrument", false}},
      {{"format", "vst3"}, {"id", "VST3-Fracture-cccc-3333"}, {"name", "Fracture"}, {"vendor", "GlitchMachines"}, {"instrument", false}},
      {{"format", "vst3"}, {"id", "VST3-Kick3-dddd-4444"}, {"name", "Kick3"}, {"vendor", ""}, {"instrument", true}},
      {{"format", "vst3"}, {"id", "VST3-SemFabricante-eeee-5555"}, {"name", "Sem Fabricante"}, {"instrument", true}},
  });
  const auto groups = pluginsByVendor(plugins);
  size_t total = 0;
  for (const auto& g : groups) total += g.plugins.size();
  CHECK_EQ(total, plugins.size());  // nenhum plugin instalado se perde, mesmo sem fabricante
  CHECK_EQ(groups.size(), (size_t)3);
  if (groups.size() == 3) {
    CHECK_EQ(groups[0].vendor, std::string("GlitchMachines"));  // alfabetico, maiusculas ignoradas
    CHECK_EQ(groups[1].vendor, std::string("KiloHearts"));      // KiloHearts e kiloHearts: o mesmo fabricante
    CHECK_EQ(groups[1].plugins.size(), (size_t)2);
    CHECK_EQ(groups[2].vendor, std::string("Other"));           // sem fabricante: "Other", nao desaparece
    CHECK_EQ(groups[2].plugins.size(), (size_t)2);
  }
  CHECK(pluginsByVendor(json::object()).empty());  // entrada que nao e lista: vazio, sem erro
}

CORE_TEST_MAIN
