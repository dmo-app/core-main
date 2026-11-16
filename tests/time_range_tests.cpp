// Testes da seleção de tempo do arranjo (Diogo 2026-11-16: "seleção, andar no timeline, copiar colar, quanto mais
// próximo do Bitwig melhor"): copiar, limpar, colar e duplicar um pedaço de tempo em várias pistas.
#include "support/check.hpp"
#include "edit.hpp"
#include "time_range.hpp"

using namespace core::doc;

namespace {
constexpr int64_t kBar = 4 * kPpq;

// Uma pista com um padrão de 1 compasso e um clip de 4 compassos a partir de 0.
std::string patternTrack(Document& d, const std::string& name) {
  const std::string id = addTrack(d, "", name);
  Track& t = trackById(d, id);
  t.clips.clear();
  addClip(t, t.patterns.front().id, 0, 4 * kBar);
  return id;
}

std::string audioTrackWithClip(Document& d) {
  Asset a;
  a.id = "a1";
  a.name = "vox.wav";
  a.path = "Audio/vox.wav";
  a.sha256 = std::string(64, '0');
  a.sampleRate = 48000;
  a.frames = 48000 * 8;  // 8 s
  d.assets.push_back(a);
  const std::string id = addAudioTrack(d, "vox");
  addAudioClip(d, id, "a1", 0);
  return id;
}

std::string ids(const std::vector<std::string>& v) {
  std::string out;
  for (const auto& x : v) out += (out.empty() ? "" : ",") + x;
  return out;
}

double pointAt(const Automation& a, int64_t tick) {
  for (const auto& p : a.points) if (p.tick == tick) return p.value;
  return -1;
}
}  // namespace

TEST("copiar um pedaço corta os clips nas bordas e não mexe na música") {
  Document d = newEmptyDocument();
  const std::string kick = patternTrack(d, "Kick");
  const Document before = d;
  const RangeCopy c = copyRange(d, {kBar, 3 * kBar, {kick}});
  CHECK_EQ(c.lengthTicks, 2 * kBar);
  CHECK_EQ(c.parts.size(), (size_t)1);
  CHECK_EQ(c.parts[0].clips.size(), (size_t)1);
  CHECK_EQ(c.parts[0].clips[0].startTick, (int64_t)0);
  CHECK_EQ(c.parts[0].clips[0].lengthTicks, 2 * kBar);
  CHECK_EQ(c.parts[0].patterns.size(), (size_t)1);
  CHECK_EQ(d.tracks[0].clips.size(), before.tracks[0].clips.size());  // a música fica igual
  CHECK_EQ(d.tracks[0].clips[0].lengthTicks, 4 * kBar);
}

TEST("limpar um pedaço deixa o tempo e corta o clip em dois") {
  Document d = newEmptyDocument();
  const std::string kick = patternTrack(d, "Kick");
  const int64_t length = d.lengthTicks;
  const int gone = clearRange(d, {kBar, 3 * kBar, {kick}});
  CHECK_EQ(gone, 1);
  const Track& t = trackById(d, kick);
  CHECK_EQ(t.clips.size(), (size_t)2);
  CHECK_EQ(t.clips[0].startTick, (int64_t)0);
  CHECK_EQ(t.clips[0].lengthTicks, kBar);
  CHECK_EQ(t.clips[1].startTick, 3 * kBar);
  CHECK_EQ(t.clips[1].lengthTicks, kBar);
  CHECK_EQ(d.lengthTicks, length);
}

TEST("colar na mesma pista usa o mesmo padrão e substitui o que lá estava") {
  Document d = newEmptyDocument();
  const std::string kick = patternTrack(d, "Kick");
  addClip(trackById(d, kick), trackById(d, kick).patterns.front().id, 8 * kBar, 4 * kBar);
  const RangeCopy c = copyRange(d, {0, 2 * kBar, {kick}});
  const RangePasteResult r = pasteRange(d, c, 9 * kBar, {kick});
  CHECK_EQ(r.clips, 1);
  CHECK_EQ(r.skippedTracks, 0);
  const Track& t = trackById(d, kick);
  CHECK_EQ(t.patterns.size(), (size_t)1);
  int at9 = 0;
  for (const auto& clip : t.clips) {
    if (clip.startTick == 9 * kBar) { ++at9; CHECK_EQ(clip.lengthTicks, 2 * kBar); }
    CHECK(!(clip.startTick > 9 * kBar && clip.startTick < 11 * kBar));  // nada por baixo do que foi colado
  }
  CHECK_EQ(at9, 1);
}

TEST("colar noutra pista copia o padrão com outro id e as mesmas notas") {
  Document d = newEmptyDocument();
  const std::string kick = patternTrack(d, "Kick");
  const std::string other = patternTrack(d, "Other");
  Pattern& p = trackById(d, kick).patterns.front();
  p.notes.push_back(Note{"n1", 0, kPpq / 4, 36, 0.9});
  const RangeCopy c = copyRange(d, {0, kBar, {kick}});
  pasteRange(d, c, 0, {other});
  const Track& t = trackById(d, other);
  CHECK_EQ(t.patterns.size(), (size_t)2);
  const Pattern& copied = t.patterns.back();
  CHECK(copied.id != t.patterns.front().id);
  CHECK_EQ(copied.notes.size(), (size_t)1);
  bool uses = false;
  for (const auto& clip : t.clips) uses |= clip.patternId == copied.id && clip.startTick == 0;
  CHECK(uses);
}

TEST("várias pistas: cola de cima para baixo e salta a pista do tipo errado") {
  Document d = newEmptyDocument();
  const std::string kick = patternTrack(d, "Kick");
  const std::string vox = audioTrackWithClip(d);
  const std::string bass = patternTrack(d, "Bass");
  CHECK_EQ(ids(tracksFrom(d, kick, 2)), ids({kick, vox}));
  CHECK_EQ(ids(tracksFrom(d, vox, 5)), ids({vox, bass}));
  const RangeCopy c = copyRange(d, {0, kBar, {kick, bass}});
  CHECK_EQ(c.parts.size(), (size_t)2);
  // As duas partes de padrões a partir do kick: a segunda cai na pista de áudio e fica de fora.
  const RangePasteResult r = pasteRange(d, c, 6 * kBar, tracksFrom(d, kick, c.parts.size()));
  CHECK_EQ(r.skippedTracks, 1);
  CHECK_EQ(ids(r.range.trackIds), ids({kick}));
}

TEST("áudio: o clip é cortado nas bordas e colado na mesma pista") {
  Document d = newEmptyDocument();
  const std::string vox = audioTrackWithClip(d);
  const RangeCopy c = copyRange(d, {kBar, 2 * kBar, {vox}});
  CHECK_EQ(c.parts.size(), (size_t)1);
  CHECK(c.parts[0].audio);
  CHECK_EQ(c.parts[0].audioClips.size(), (size_t)1);
  const double secondsPerBar = secondsBetween(d, 0, (double)kBar);
  CHECK_NEAR(c.parts[0].audioClips[0].offsetSeconds, secondsPerBar, 1e-6);
  CHECK_NEAR(c.parts[0].audioClips[0].durationSeconds, secondsPerBar, 1e-6);
  const RangePasteResult r = pasteRange(d, c, 12 * kBar, {vox});
  CHECK_EQ(r.clips, 1);
  CHECK_EQ(trackById(d, vox).audioClips.size(), (size_t)2);
}

TEST("duplicar põe a música logo a seguir e a música cresce se preciso") {
  Document d = newEmptyDocument();
  const std::string kick = patternTrack(d, "Kick");
  const TimeRange next = duplicateRange(d, {0, 2 * kBar, {kick}});
  CHECK_EQ(next.fromTick, 2 * kBar);
  CHECK_EQ(next.toTick, 4 * kBar);
  CHECK_EQ(ids(next.trackIds), ids({kick}));
  // No fim da música: cresce.
  const TimeRange end = duplicateRange(d, {d.lengthTicks - kBar, d.lengthTicks, {kick}});
  CHECK_EQ(d.lengthTicks, end.toTick);
}

TEST("automação: copia a curva com os valores das bordas e cola só na própria pista") {
  Document d = newEmptyDocument();
  const std::string kick = patternTrack(d, "Kick");
  const std::string bass = patternTrack(d, "Bass");
  for (const auto& id : {kick, bass}) {
    Automation a;
    a.param = "gainDb";
    a.points = {{0, 0, json::object()}, {4 * kBar, -12, json::object()}};
    trackById(d, id).automation.push_back(a);
  }
  const RangeCopy c = copyRange(d, {kBar, 2 * kBar, {kick}});
  CHECK_EQ(c.parts[0].automation.size(), (size_t)1);
  CHECK_NEAR(c.parts[0].automation[0].points.front().value, -3, 1e-9);
  pasteRange(d, c, 8 * kBar, {kick});
  const Automation& lane = trackById(d, kick).automation.front();
  CHECK_NEAR(pointAt(lane, 8 * kBar), -3, 1e-9);
  pasteRange(d, c, 8 * kBar, {bass});
  CHECK_EQ(trackById(d, bass).automation.front().points.size(), (size_t)2);  // outra pista: a curva não vai
}

TEST("limpar mantém a curva fora do pedaço") {
  Document d = newEmptyDocument();
  const std::string kick = patternTrack(d, "Kick");
  Automation a;
  a.param = "gainDb";
  a.points = {{0, 0, json::object()}, {kBar, -6, json::object()}, {2 * kBar, 0, json::object()}, {4 * kBar, -12, json::object()}};
  trackById(d, kick).automation.push_back(a);
  clearRange(d, {kBar / 2, 3 * kBar, {kick}});
  const Automation& lane = trackById(d, kick).automation.front();
  CHECK_NEAR(pointAt(lane, 0), 0, 1e-9);
  CHECK_NEAR(pointAt(lane, kBar / 2 - 1), -3 * double(kBar / 2 - 1) / double(kBar / 2), 1e-9);
  CHECK_NEAR(pointAt(lane, 3 * kBar), -6, 1e-9);
  CHECK_NEAR(pointAt(lane, 4 * kBar), -12, 1e-9);
  for (const auto& p : lane.points) CHECK(!(p.tick >= kBar / 2 && p.tick < 3 * kBar));
}

TEST("um pedaço vazio não faz nada") {
  Document d = newEmptyDocument();
  const std::string kick = patternTrack(d, "Kick");
  CHECK(copyRange(d, {kBar, kBar, {kick}}).empty());
  CHECK(copyRange(d, {0, kBar, {}}).empty());
  CHECK_EQ(clearRange(d, {kBar, kBar, {kick}}), 0);
  CHECK_EQ(pasteRange(d, RangeCopy{}, 0, {kick}).clips, 0);
}

CORE_TEST_MAIN
