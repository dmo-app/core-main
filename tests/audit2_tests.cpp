// Testes da segunda auditoria externa de 2026-11-16 (sweep por analise de codigo: IPC cliente-motor, sync do
// documento, espaco em disco do export, autosave estragado). Cada teste repete o caso do relatorio.
#include <filesystem>
#include <fstream>
#include "support/check.hpp"
#include "autosave.hpp"
#include "engine_protocol.hpp"
#include "sync_retry.hpp"
#include "core/project.hpp"
#include "core/render_job.hpp"
#include "core/server.hpp"

namespace fs = std::filesystem;
using core::json;
using core::client::EngineLine;
using core::client::readEngineLine;
using core::client::SyncRetry;

TEST("auditoria 2, bug 1: uma linha do motor com tipos errados nunca lanca no cliente") {
  // O que o cliente fazia antes com estas linhas: json::value com o tipo errado lanca (e lancava na thread de leitura,
  // o que fecha a app com std::terminate).
  CHECK_THROWS(json::parse(R"({"id":1,"ok":false,"error":42})").value("error", std::string("Engine error.")));
  CHECK_THROWS(json::parse(R"({"id":1,"ok":"yes"})").value("ok", false));

  EngineLine r = readEngineLine(R"({"id":7,"ok":false,"error":42})");
  CHECK(r.kind == EngineLine::Kind::Reply);
  CHECK_EQ(r.id, (int64_t)7);
  CHECK(!r.ok);
  CHECK_EQ(r.error, std::string("42"));

  r = readEngineLine(R"({"id":8,"ok":"yes","result":{"a":1}})");
  CHECK(r.kind == EngineLine::Kind::Reply);
  CHECK(!r.ok);  // so "ok": true e sucesso
  CHECK_EQ(r.error, std::string("Engine error."));

  r = readEngineLine(R"({"id":9,"ok":true,"result":{"a":1}})");
  CHECK(r.ok);
  CHECK_EQ(r.result["a"].get<int>(), 1);
  r = readEngineLine(R"({"id":10,"ok":true})");
  CHECK(r.ok);
  CHECK(r.result.is_null());

  r = readEngineLine(R"({"id":"x","error":{"why":"bad"}})");
  CHECK(r.kind == EngineLine::Kind::NoId);
  CHECK(r.error.find("bad") != std::string::npos);
  r = readEngineLine(R"({"id":1.5,"ok":true})");
  CHECK(r.kind == EngineLine::Kind::NoId);
  r = readEngineLine(R"({"error":5})");
  CHECK(r.kind == EngineLine::Kind::NoId);
  CHECK_EQ(r.error, std::string("5"));

  CHECK(readEngineLine("[1,2]").kind == EngineLine::Kind::NotJson);
  CHECK(readEngineLine("not json {").kind == EngineLine::Kind::NotJson);
  CHECK(readEngineLine(std::string("\xff\xfe", 2)).kind == EngineLine::Kind::NotJson);
  CHECK(readEngineLine(R"({"event":"ready","protocol":1})").kind == EngineLine::Kind::Ready);
  CHECK(readEngineLine(R"({"event":7})").kind == EngineLine::Kind::Event);
  // Um erro que nao e texto aparece como JSON.
  r = readEngineLine(R"({"id":3,"ok":false,"error":["a"]})");
  CHECK_EQ(r.error, std::string("[\"a\"]"));
}

TEST("auditoria 2, ponto 4: uma resposta sem id valido termina a sessao logo, com o motivo") {
  // Antes: so ficava no registo, e o comando a espera so falhava no timeout (10 s), sem dizer porque.
  using core::client::endsSession;
  using core::client::endReason;
  for (const char* line : {R"({"id":"1","ok":false,"error":{"msg":"x"}})", R"({"ok":false,"error":"Bad JSON: x"})",
                           R"({"id":null,"ok":true})", R"({"id":2.5,"ok":true,"result":1})"}) {
    const EngineLine r = readEngineLine(line);
    CHECK(endsSession(r));
    CHECK(endReason(r).find("protocol") != std::string::npos);
  }
  CHECK(endReason(readEngineLine(R"({"ok":false,"error":"Bad JSON: x"})")).find("Bad JSON: x") != std::string::npos);
  // O que o protocolo permite continua a nao terminar nada.
  for (const char* line : {R"({"id":1,"ok":false,"error":42})", R"({"id":1,"ok":true})", R"({"event":"state"})",
                           R"({"event":"ready"})", "isto nao e JSON", "[1]"})
    CHECK(!endsSession(readEngineLine(line)));
}

TEST("auditoria 2, bug 2: um project.load sem resposta volta a ser enviado sozinho, com uma revisao nova") {
  SyncRetry retry;
  int delay = -1;
  // Sem resposta a tempo: tenta outra vez, cada vez mais tarde, e para ao fim de 5.
  for (int expected : {500, 1000, 2000, 4000, 8000}) {
    CHECK(retry.failed(true, delay) == SyncRetry::After::Retry);
    CHECK_EQ(delay, expected);
  }
  CHECK(retry.failed(true, delay) == SyncRetry::After::WaitForEdit);
  // Uma edicao (ou uma sessao nova do motor) recomeca a contagem.
  retry.reset();
  CHECK(retry.failed(true, delay) == SyncRetry::After::Retry);
  CHECK_EQ(delay, 500);
  retry.succeeded();
  CHECK_EQ(retry.failures(), 0);
  // O que nao melhora sozinho (recusado, mensagem grande demais) espera pela proxima edicao, como antes.
  CHECK(retry.failed(false, delay) == SyncRetry::After::WaitForEdit);

  // Porque a revisao tem de ser nova: o motor pode ter aceitado a antiga tarde, e recusa-a outra vez como "stale".
  core::Server server(nullptr, 48000);
  const json project = {{"format", core::kProjectFormat}, {"version", 2}, {"bpm", 120}, {"lengthTicks", 960},
                        {"masterGainDb", 0}, {"tracks", json::array()}};
  CHECK(server.handle({{"cmd", "project.load"}, {"args", {{"project", project}, {"revision", 5}}}})["ok"].get<bool>());
  CHECK(!server.handle({{"cmd", "project.load"}, {"args", {{"project", project}, {"revision", 5}}}})["ok"].get<bool>());
  CHECK(server.handle({{"cmd", "project.load"}, {"args", {{"project", project}, {"revision", 6}}}})["ok"].get<bool>());
}

namespace {
fs::space_info spaceOf(uint64_t capacity, uint64_t available) {
  fs::space_info s{};
  s.capacity = capacity;
  s.free = available;
  s.available = available;
  return s;
}
}  // namespace

TEST("auditoria 2, bug 3: o espaco do export conta os mono como mono e cada volume por si") {
  const int64_t frames = 48000 * 60 * 5;  // 5 minutos
  const uint64_t mono = core::exportFileBytes(frames, 24, true), stereo = core::exportFileBytes(frames, 24, false);
  CHECK_EQ(mono, (uint64_t)44 + (uint64_t)frames * 3);
  CHECK_EQ(stereo, (uint64_t)44 + (uint64_t)frames * 6);
  const uint64_t margin = 16u << 20;

  // Dois mono na mesma pasta, com espaco para eles mas nao para dois estereo (o calculo antigo recusava).
  std::vector<core::RenderOutput> outs = {{"/disco/a.wav", -1, false, true}, {"/disco/b.wav", 0, false, true}};
  const uint64_t room = 2 * mono + margin + 1024;
  CHECK(room < 2 * stereo + margin);  // antes: "Not enough disk space"
  auto oneDisk = [&](const fs::path&, fs::space_info& s) { s = spaceOf(1ull << 40, room); return true; };
  core::checkExportSpace(outs, frames, 24, oneDisk);  // nao lanca
  // Um estereo a mais ja nao cabe.
  outs.push_back({"/disco/c.wav", -1, false, false});
  CHECK_THROWS(core::checkExportSpace(outs, frames, 24, oneDisk));

  // Dois volumes: o primeiro tem espaco de sobra, o segundo nao. Antes so se via o do primeiro ficheiro.
  std::vector<core::RenderOutput> two = {{"/grande/mix.wav", -1}, {"/pequeno/stem.wav", 0}};
  auto twoDisks = [&](const fs::path& folder, fs::space_info& s) {
    s = folder == fs::path("/grande") ? spaceOf(1ull << 40, 1ull << 39) : spaceOf(1ull << 30, stereo / 2);
    return true;
  };
  bool named = false;
  try { core::checkExportSpace(two, frames, 24, twoDisks); }
  catch (const std::runtime_error& e) { named = std::string(e.what()).find("pequeno") != std::string::npos; }
  CHECK(named);

  // Duas pastas no mesmo volume somam-se: cada ficheiro cabe sozinho, os dois juntos nao.
  std::vector<core::RenderOutput> shared = {{"/d/um/a.wav", -1}, {"/d/dois/b.wav", 0}};
  auto sameDisk = [&](const fs::path&, fs::space_info& s) { s = spaceOf(1ull << 40, stereo + margin + 1024); return true; };
  CHECK_THROWS(core::checkExportSpace(shared, frames, 24, sameDisk));
  shared.pop_back();
  core::checkExportSpace(shared, frames, 24, sameDisk);

  // Um volume que nao responde nao bloqueia o export (a escrita e que diz).
  core::checkExportSpace(two, frames, 24, [](const fs::path&, fs::space_info&) { return false; });
}

TEST("auditoria 2, observacao: um autosave estragado diz que esta estragado, sem excecao do JSON") {
  const fs::path dir = fs::temp_directory_path() / "core-audit2-tests";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const fs::path file = dir / "broken.core-autosave";
  { std::ofstream(file, std::ios::binary) << "{\"format\":\"core-autosave\",\"document\":{"; }
  std::string message;
  try { (void)core::doc::readAutosave(file); } catch (const std::runtime_error& e) { message = e.what(); }
  CHECK(message.find("damaged") != std::string::npos);
  { std::ofstream(file, std::ios::binary | std::ios::trunc) << "[1,2,3]"; }
  message.clear();
  try { (void)core::doc::readAutosave(file); } catch (const std::runtime_error& e) { message = e.what(); }
  CHECK(message.find("not an autosave") != std::string::npos);
  // E a lista de autosaves deixa-o de fora (ja era assim).
  CHECK(core::doc::findAutosaves(dir, "").empty());
}

CORE_TEST_MAIN
