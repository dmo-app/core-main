// Testes do Channel Rack (Diogo 2026-11-16: "diz que estou sair do padrao?"): um step clicado depois do fim de um
// padrão mais curto (hats de 1 tempo num rack de 4 compassos) entra, e o padrão cresce a repetir o que tocava.
#include "support/check.hpp"
#include "edit.hpp"
#include "pattern_rack.hpp"

using namespace core::doc;

namespace {
constexpr int64_t kBar = 4 * kPpq, kStep = kPpq / 4;

Pattern oneBeatHat() {
  Pattern p;
  p.id = "p1";
  p.lengthTicks = kPpq;  // 1 tempo
  p.notes.push_back(Note{"n1", kStep * 2, kStep / 2, 42, 0.8});
  return p;
}

int notesAt(const Pattern& p, int64_t tick) {
  int n = 0;
  for (const auto& x : p.notes) n += x.tick == tick;
  return n;
}
}  // namespace

TEST("um step dentro do padrão liga e desliga como antes") {
  Pattern p = oneBeatHat();
  CHECK(toggleRackStep(p, 0, kStep, 42, 0.8, 0));
  CHECK_EQ(p.lengthTicks, kPpq);
  CHECK(!toggleRackStep(p, 0, kStep, 42, 0.8, 0));
  CHECK_EQ(p.notes.size(), (size_t)1);
}

TEST("um step depois do fim: o padrão cresce ao compasso e repete o que tocava") {
  Pattern p = oneBeatHat();
  CHECK(toggleRackStep(p, kBar - kStep, kStep, 42, 0.8, 0));  // o último 1/16 do compasso 1
  CHECK_EQ(p.lengthTicks, kBar);
  for (int beat = 0; beat < 4; ++beat) CHECK_EQ(notesAt(p, beat * kPpq + kStep * 2), 1);  // o hat em cada tempo
  CHECK_EQ(notesAt(p, kBar - kStep), 1);
}

TEST("um step depois do fim onde a repetição já toca fica ligado (não se apaga)") {
  Pattern p = oneBeatHat();
  CHECK(toggleRackStep(p, kPpq + kStep * 2, kStep, 42, 0.8, 0));  // o hat do 2.º tempo, que a repetição já tocava
  CHECK_EQ(p.lengthTicks, kBar);
  CHECK_EQ(notesAt(p, kPpq + kStep * 2), 1);
}

TEST("um step no compasso 3 leva o padrão a 4 compassos; depois de 8 recusa") {
  Pattern p = oneBeatHat();
  CHECK(toggleRackStep(p, 2 * kBar, kStep, 42, 0.8, 0));
  CHECK_EQ(p.lengthTicks, 4 * kBar);
  CHECK_EQ(notesAt(p, 3 * kBar + kStep * 2), 1);
  bool refused = false;
  try { toggleRackStep(p, kRackMaxBars * kBar, kStep, 42, 0.8, 0); } catch (const DocumentError&) { refused = true; }
  CHECK(refused);
  CHECK_EQ(p.lengthTicks, 4 * kBar);
}

CORE_TEST_MAIN
