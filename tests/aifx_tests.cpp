// Testes do Core AI Gate (app/aifx/src/gate.hpp, o primeiro plugin da familia "AI FX"): o som (padroes, rate, duck,
// suavizacao, nada de NaN), o papel "gate", a tool toolGate e a chamada "gate" da AI. Sem JUCE: o plugin so embrulha
// este codigo.
#include <cmath>
#include <vector>
#include "support/check.hpp"
#include "calls.hpp"
#include "edit.hpp"
#include "gate.hpp"
#include "role_tools.hpp"
#include "roles.hpp"
#include "synth_roles.hpp"

using namespace core;
using namespace core::aifx;
using doc::kPpq;
using json = nlohmann::json;

namespace {
// The gate's role as withAiRoles writes it (ParamIDs = the parameters' indexes).
doc::RoleMap gateMap() {
  const json scanned = json::array({{{"format", "vst3"}, {"id", "VST3-CoreAIGate"}, {"name", "Core AI Gate"}, {"vendor", "Core"},
                                     {"file", "C:/VST3/Core AI Gate.vst3"}}});
  return doc::roleMapFromJson(ai::withAiRoles(json::object(), scanned));
}
std::string trackNamed(const doc::Document& d, const std::string& name) {
  for (const auto& t : d.tracks) if (t.name.find(name) != std::string::npos) return t.id;
  return "";
}
// One second of a constant 1.0 through the gate at 120 BPM from beat 0; the gains at each sample.
std::vector<float> run(const Settings& s, bool playing = true, double rate = 48000) {
  Gate g;
  g.prepare(rate);
  std::vector<float> l((size_t)rate, 1.0f), r((size_t)rate, 1.0f);
  float* ch[2] = {l.data(), r.data()};
  for (int at = 0; at < (int)rate; at += 256) {
    const int n = std::min(256, (int)rate - at);
    float* part[2] = {ch[0] + at, ch[1] + at};
    g.process(part, 2, n, s, at * 2.0 / rate, 120, playing);
  }
  return l;
}
}  // namespace

TEST("gate: os parametros tem ordem fixa e ida e volta 0..1") {
  CHECK_EQ(std::string(params()[kStep1].id), std::string("step01"));
  CHECK_EQ(std::string(params()[kStep1 + 15].id), std::string("step16"));
  CHECK_EQ(std::string(params()[kRate].id), std::string("rate"));
  CHECK_EQ(std::string(params()[kMix].id), std::string("mix"));
  CHECK_EQ((int)kParamCount, 26);
  CHECK_NEAR(toNormalized(kStep1, 1), 0.5, 1e-12);
  CHECK_NEAR(fromNormalized(kRate, toNormalized(kRate, 3)), 3.0, 1e-12);
  CHECK_NEAR(fromNormalized(kSmooth, toNormalized(kSmooth, 7.5)), 7.5, 1e-9);
}

TEST("gate: padroes em texto repetem-se ate 16 passos; texto errado recusado") {
  std::array<int, kSteps> s{};
  CHECK(parsePattern("x.xx", s));
  CHECK_EQ(patternText(s), std::string("x.xxx.xxx.xxx.xx"));
  CHECK(parsePattern("x.h. x.h. | x.h. x.h.", s));
  CHECK_EQ(s[2], 1);
  CHECK(!parsePattern("x.x", s));      // 3 nao divide 16
  CHECK(!parsePattern("xyz.", s));
  CHECK(!parsePattern("", s));
  for (const auto& p : presets()) {
    CHECK(parsePattern(p.pattern, s));
    CHECK(p.rate >= 0 && p.rate < 4);
  }
  CHECK(findPreset("PSY-BASS-GAP") != nullptr);
  CHECK_EQ(rateIndex("1/16t"), 3);
  CHECK_EQ(rateIndex("1/3"), -1);
}

TEST("gate: o ganho segue o padrao no tempo da musica") {
  Settings s = fromPreset(*findPreset("psy-bass-gap"));  // .xxx a 1/16
  CHECK_NEAR(gateGain(s, 0.01), 0.0, 1e-9);   // primeira 1/16 de cada tempo: fechado
  CHECK_NEAR(gateGain(s, 0.30), 1.0, 1e-9);
  CHECK_NEAR(gateGain(s, 1.05), 0.0, 1e-9);
  CHECK_NEAR(gateGain(s, -0.70), 1.0, 1e-9);  // antes do 0 tambem (pre-roll)
  CHECK_NEAR(gateGain(s, -0.95), 0.0, 1e-9);
  Settings t;                                  // trance 1/16, metade aberta
  CHECK_NEAR(gateGain(t, 0.1), 1.0, 1e-9);
  CHECK_NEAR(gateGain(t, 0.2), 0.0, 1e-9);
  t.depth = 0.5;
  CHECK_NEAR(gateGain(t, 0.2), 0.5, 1e-9);
  t.steps[0] = 1;                              // meio passo
  t.depth = 1;
  CHECK_NEAR(gateGain(t, 0.1), 0.5, 1e-9);
  Settings d = fromPreset(*findPreset("pump-hard"));
  CHECK(gateGain(d, 0.0) < 0.01);              // no tempo: o duck ao fundo
  CHECK_NEAR(gateGain(d, 0.5), 1.0, 1e-9);     // depois do duck: aberto
}

TEST("gate: sem cliques (suavizado), parado deixa passar, nunca NaN") {
  Settings s;
  const auto out = run(s);
  float biggest = 0;
  for (size_t i = 1; i < out.size(); ++i) biggest = std::max(biggest, std::abs(out[i] - out[i - 1]));
  CHECK(biggest < 0.02f);                     // 3 ms a 48 kHz: nenhum salto
  float lo = 1, hi = 0;
  for (float v : out) { lo = std::min(lo, v); hi = std::max(hi, v); }
  CHECK(lo < 0.05f);
  CHECK(hi > 1.0f);                            // auto gain sobe o som aberto
  CHECK(hi <= 2.0f + 1e-4f);
  const auto still = run(s, false);
  CHECK_NEAR(still.back(), 1.0, 1e-4);         // parado: passa como esta
  Gate g;
  g.prepare(48000);
  float nan[4] = {NAN, INFINITY, 1.0f, -INFINITY};
  float* ch[1] = {nan};
  g.process(ch, 1, 4, s, 0, 120, true);
  for (float v : nan) CHECK(std::isfinite(v));
}

TEST("gate: o auto gain fica entre 0 e +6 dB") {
  Settings s;
  CHECK(autoGainOf(s) > 1.0 && autoGainOf(s) <= 2.0);
  s.depth = 0;
  CHECK_NEAR(autoGainOf(s), 1.0, 1e-9);
  s.depth = 1;
  s.steps.fill(0);
  CHECK_NEAR(autoGainOf(s), 2.0, 1e-9);
}

TEST("gate: o papel gate vem com os ParamIDs do plugin, sem adivinhar") {
  const doc::RoleInfo* r = doc::findRole("gate");
  CHECK(r && !r->instrument);
  if (!r) return;
  CHECK(doc::findRoleMacro(*r, "step01") && doc::findRoleMacro(*r, "depth") && doc::findRoleMacro(*r, "duck"));
  CHECK_NEAR(doc::findRoleMacro(*r, "depth")->rest, 0.0, 1e-12);  // fora das seccoes o gate nao faz nada
  const doc::RoleMap m = gateMap();
  CHECK(m.find("gate") != nullptr);
  CHECK_EQ(m.find("gate")->params.at("step01"), std::string("0"));
  CHECK_EQ(m.find("gate")->params.at("rate"), std::string("16"));
  CHECK_EQ(m.find("gate")->params.at("mix"), std::string("23"));
  // Adivinhado pelos nomes (Settings > Roles com o plugin vivo): o mesmo resultado.
  json live = json::array();
  for (int p = 0; p < kParamCount; ++p) live.push_back({{"id", std::to_string(p)}, {"name", params()[(size_t)p].name}});
  const doc::RoleBinding g = doc::guessRoleBinding("gate", m.find("gate")->plugin, live);
  CHECK_EQ(g.params.at("length"), std::string("18"));
  CHECK_EQ(g.params.at("duck"), std::string("20"));
  CHECK_EQ(g.params.at("duckLength"), std::string("21"));
  CHECK_EQ(g.params.at("step10"), std::string("9"));
}

TEST("gate: toolGate escreve linhas planas so na seccao e volta ao repouso") {
  doc::Document d = doc::newTemplateDocument();
  const std::string bass = trackNamed(d, "ass");
  CHECK(!bass.empty());
  const doc::RoleMap m = gateMap();
  const int64_t start = 4 * 4 * kPpq, end = 8 * 4 * kPpq;
  const auto r = doc::toolGate(d, bass, m, start, end, fromPreset(*findPreset("psy-bass-gap")), "psy-bass-gap");
  CHECK(!r.keys.empty());
  CHECK(r.sentence.find(".xxx") != std::string::npos);
  const int fx = doc::findRoleEffect(d, bass, "gate");
  CHECK(fx >= 0);
  const std::string id = doc::effectChain(d, bass)[(size_t)fx]["id"].get<std::string>();
  const doc::Automation* depth = doc::findAutomation(d, bass, "fx:" + id + ":vst:17");
  const doc::Automation* step1 = doc::findAutomation(d, bass, "fx:" + id + ":vst:0");
  const doc::Automation* step2 = doc::findAutomation(d, bass, "fx:" + id + ":vst:1");
  CHECK(depth && step1);
  CHECK(step2 == nullptr);  // passo ligado = repouso: nada escrito
  if (!depth || !step1) return;
  CHECK_NEAR(doc::automationValueAt(*depth, start + kPpq), 1.0, 1e-9);
  CHECK_NEAR(doc::automationValueAt(*depth, end), 0.0, 1e-9);
  CHECK_NEAR(doc::automationValueAt(*step1, start + 3 * kPpq), 0.0, 1e-9);
  CHECK_NEAR(doc::automationValueAt(*step1, end + kPpq), 1.0, 1e-9);
  CHECK_THROWS(doc::toolGate(d, bass, doc::RoleMap{}, start, end, Settings{}));  // sem papel: recusado
}

TEST("gate: a chamada gate da AI com preset ou padrao, recusada sem nada") {
  doc::Document d = doc::newTemplateDocument();
  d.sections.clear();
  doc::Section drop;
  drop.id = "sec-drop";
  drop.name = "Drop";
  drop.startTick = 0;
  drop.lengthTicks = std::min<int64_t>(8 * 4 * kPpq, d.lengthTicks);
  d.sections.push_back(drop);
  const std::string section = d.sections.front().name;
  const std::string bassName = doc::trackById(d, trackNamed(d, "ass")).name;
  const auto fns = control::buildFunctions();
  const json raw = {{"calls", json::array({
      {{"fn", "gate"}, {"on", bassName}, {"section", section}, {"preset", "psy-bass-gap"}},
      {{"fn", "gate"}, {"on", bassName}, {"section", section}, {"pattern", "x.xx"}, {"rate", "1/16"}, {"args", {{"duck", 0.5}}}},
      {{"fn", "gate"}, {"on", bassName}, {"section", section}},                                   // sem preset nem padrao
      {{"fn", "gate"}, {"on", bassName}, {"section", section}, {"pattern", "x.x"}},              // padrao errado
      {{"fn", "gate"}, {"on", bassName}, {"preset", "trance-16"}},                                // sem seccao
      {{"fn", "gate"}, {"on", bassName}, {"section", section}, {"preset", "trance-16"}, {"args", {{"depth", 0}}}}})}};
  const control::Proposal p = control::validateProposal(raw, fns, d, nullptr);
  CHECK_EQ(p.calls.size(), (size_t)2);
  CHECK_EQ(p.problems.size(), (size_t)4);
  const doc::RoleMap m = gateMap();
  control::ApplyEnv env;
  CHECK_THROWS(control::applyProposal(d, p, "", env));  // sem mapa de papeis
  env.roles = &m;
  const auto lines = control::applyProposal(d, p, "", env);
  CHECK_EQ(lines.size(), (size_t)2);
  CHECK(lines[1].find("duck 50%") != std::string::npos);
}

CORE_TEST_MAIN
