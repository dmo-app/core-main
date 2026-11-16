// As pistas mostram a forma de onda do som dos plugins (Diogo 2026-11-16: "ainda vejo as tracks com blocos em vez de waves").
#include "support/check.hpp"
#include "edit.hpp"
#include "track_waves.hpp"
#include <cmath>

using namespace core::doc;

namespace {
constexpr int64_t kBar = 4 * kPpq;

Track patternTrack(const std::string& id, bool withNotes) {
  Track t;
  t.id = id;
  t.name = id;
  t.instrument.kind = "plugin";
  Pattern p;
  p.id = id + "p";
  p.lengthTicks = kBar;
  if (withNotes) p.notes.push_back(Note{"n1", 0, kPpq / 4, 36, 0.9});
  t.patterns.push_back(p);
  t.clips.push_back(Clip{id + "c", id + "p", 0, 4 * kBar, 0});
  return t;
}
}  // namespace

TEST("só as pistas com notas em clips recebem um stem para a forma de onda") {
  Document d = newTemplateDocument();
  d.tracks.clear();
  d.tracks.push_back(patternTrack("kick", true));
  d.tracks.push_back(patternTrack("vazia", false));  // padrão sem notas: nada a desenhar
  Track audio = patternTrack("audio", true);
  audio.instrument.kind = "audio";  // as pistas de áudio já mostram o ficheiro
  d.tracks.push_back(audio);
  Track semClips = patternTrack("semclips", true);
  semClips.clips.clear();
  d.tracks.push_back(semClips);
  d.tracks.push_back(patternTrack("bass", true));
  const auto ids = tracksForWaves(d);
  CHECK_EQ(ids.size(), (size_t)2);
  CHECK(ids.size() == 2 && ids[0] == "kick" && ids[1] == "bass");
}

TEST("o pedaço do stem debaixo de um clip segue o tempo da música") {
  Document d = newTemplateDocument();
  d.bpm = 190;
  const auto [a, b] = stemSeconds(d, (double)kBar, (double)(2 * kBar));
  CHECK(std::abs(a - 4 * 60.0 / 190) < 1e-9);
  CHECK(std::abs(b - 8 * 60.0 / 190) < 1e-9);
}

TEST("o nome do ficheiro do stem é seguro e muda com a revisão") {
  CHECK_EQ(waveFileName("t1", 42), std::string("t1-42.wav"));
  CHECK_EQ(waveFileName("a/b:c", 3), std::string("a_b_c-3.wav"));
  CHECK_EQ(waveFileName("", 1), std::string("track-1.wav"));
}

CORE_TEST_MAIN
