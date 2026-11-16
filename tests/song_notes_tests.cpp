// Notas no tempo da música (Diogo 2026-11-16: o painel de baixo mostra várias pistas juntas, como um piano roll).
#include "support/check.hpp"
#include "edit.hpp"
#include "song_notes.hpp"

using namespace core::doc;

namespace {
constexpr int64_t kBar = 4 * kPpq, kStep = kPpq / 4;

// Uma pista com um padrão de 1 compasso (kick em cada tempo) num clip de 4 compassos a partir do compasso 2.
Document song() {
  Document d = newTemplateDocument();
  Track t;
  t.id = "k1";
  t.name = "Kick";
  t.instrument.kind = "plugin";
  Pattern p;
  p.id = "p1";
  p.lengthTicks = kBar;
  for (int i = 0; i < 4; ++i) p.notes.push_back(Note{"n" + std::to_string(i + 1), i * kPpq, kStep, 36, 0.9});
  t.patterns.push_back(p);
  t.clips.push_back(Clip{"c1", "p1", kBar, 4 * kBar, 0});
  d.tracks.push_back(t);
  return d;
}
}  // namespace

TEST("as notas de um padrão em loop aparecem em cada repetição do clip") {
  const Document d = song();
  const auto all = songNotesIn(d, "k1", 0, 10 * kBar);
  CHECK_EQ(all.size(), (size_t)16);
  CHECK_EQ(all.front().start, kBar);
  CHECK_EQ(all.back().start, 4 * kBar + 3 * kPpq);
  const auto some = songNotesIn(d, "k1", 2 * kBar, 3 * kBar);  // só o 2.º compasso do clip
  CHECK_EQ(some.size(), (size_t)4);
  CHECK_EQ(some.front().start, 2 * kBar);
  CHECK(songNotesIn(d, "k1", 0, kBar).empty());  // antes do clip
  CHECK_EQ(songPitchesIn(d, "k1", 0, 10 * kBar).size(), (size_t)1);
}

TEST("um clip cortado a meio do padrão mantém a fase (offset)") {
  Document d = song();
  d.tracks.back().clips[0].offsetTicks = 2 * kPpq;  // começa no 3.º tempo do padrão
  const auto n = songNotesIn(d, "k1", kBar, 2 * kBar);
  CHECK_EQ(n.size(), (size_t)4);
  CHECK_EQ(n[0].noteId, std::string("n3"));
  CHECK_EQ(n[0].start, kBar);
}

TEST("acrescentar no tempo da música escreve no padrão do clip; sem clip cria um compasso") {
  Document d = song();
  auto [pid, nid] = addSongNote(d, "k1", 3 * kBar + 2 * kStep, kStep, 38, 0.5);
  CHECK_EQ(pid, std::string("p1"));
  const Pattern& p = trackById(d, "k1").patterns[0];
  CHECK_EQ(p.notes.back().tick, 2 * kStep);
  CHECK_EQ(p.notes.back().pitch, 38);
  CHECK_EQ(songNotesIn(d, "k1", 0, 10 * kBar).size(), (size_t)20);  // aparece nas 4 repetições
  auto [pid2, nid2] = addSongNote(d, "k1", 8 * kBar + kPpq, kStep, 36, 0.8);  // depois do clip
  CHECK(pid2 != "p1");
  Track& t = trackById(d, "k1");
  CHECK_EQ(t.clips.size(), (size_t)2);
  CHECK_EQ(t.clips.back().startTick, 8 * kBar);
  CHECK_EQ(patternById(t, pid2).notes[0].tick, kPpq);
  CHECK_THROWS(addSongNote(d, "k1", 0, kStep, 200, 0.5));
  CHECK_THROWS(addSongNote(d, "k1", 0, 0, 36, 0.5));
}

TEST("mover, encurtar, velocity e apagar mexem na nota do padrão") {
  Document d = song();
  moveSongNote(d, "k1", "p1", "n4", kPpq, 2);  // passa o fim: dá a volta ao início do padrão
  Pattern& p = trackById(d, "k1").patterns[0];
  CHECK_EQ(p.notes[3].tick, 0);
  CHECK_EQ(p.notes[3].pitch, 38);
  resizeSongNote(d, "k1", "p1", "n1", 10 * kBar);
  CHECK_EQ(p.notes[0].length, kBar);  // cortada no fim do padrão
  setSongNoteVelocity(d, "k1", "p1", "n2", 3.0);
  CHECK_NEAR(p.notes[1].velocity, 1.0, 1e-9);
  removeSongNote(d, "k1", "p1", "n3");
  CHECK_EQ(p.notes.size(), (size_t)3);
  CHECK_THROWS(removeSongNote(d, "k1", "p1", "n3"));
}

CORE_TEST_MAIN
