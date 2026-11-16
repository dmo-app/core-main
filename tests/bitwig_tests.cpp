// Testes do export para o Bitwig (bitwig.hpp): que presets vao na pasta, com que nomes, e em que pista do .mid.
#include "support/check.hpp"
#include "bitwig.hpp"
#include "edit.hpp"

using namespace core::doc;

namespace {
json plugin(const std::string& name) {
  return {{"format", "vst3"}, {"id", name + "-id"}, {"name", name}, {"vendor", "Xfer"}, {"file", name + ".vst3"}};
}
}  // namespace

TEST("Bitwig: cada pista com notas e o seu MIDI track, o instrumento e os efeitos plugin dao um preset cada") {
  Document d = newEmptyDocument();
  d.name = "Drop: final?";
  d.bpm = 145;
  const std::string lead = addTrack(d, "", "Lead");
  replaceInstrumentWithPlugin(d, lead, plugin("Serum 2"));
  addPluginEffect(d, lead, plugin("ValhallaRoom"));
  const std::string muted = addTrack(d, "", "Muda");
  trackById(d, muted).mix.mute = true;
  replaceInstrumentWithPlugin(d, muted, plugin("Pad"));
  const std::string bass = addTrack(d, "", "Bass/Sub");
  replaceInstrumentWithPlugin(d, bass, plugin("Serum 2"));
  addAudioTrack(d, "vox");
  addPluginEffect(d, "", plugin("Pro-L 2"));

  const BitwigPlan plan = bitwigPlan(d);
  CHECK_EQ(plan.midiFile, std::string("Drop_ final_.mid"));
  CHECK_EQ(plan.tracks.size(), (size_t)3);  // Lead, Muda (o plugin, sem notas no .mid), Bass; a pista audio sem plugins fica de fora
  CHECK_EQ(plan.tracks[0].midiTrack, 1);
  CHECK_EQ(plan.tracks[1].midiTrack, 0);  // muda: nao vai no .mid (songToMidi deixa-a de fora)
  CHECK_EQ(plan.tracks[2].midiTrack, 2);
  CHECK_EQ(songToMidi(d).tracks.size(), (size_t)2);
  CHECK_EQ(plan.tracks[0].presets.size(), (size_t)2);
  CHECK_EQ(plan.tracks[0].presets[0].key, pluginKey(lead));
  CHECK_EQ(plan.tracks[0].presets[0].file, std::string("01 Lead - Serum 2.vstpreset"));
  CHECK_EQ(plan.tracks[0].presets[1].role, std::string("effect"));
  CHECK_EQ(plan.tracks[0].presets[1].file, std::string("01 Lead - fx1 ValhallaRoom.vstpreset"));
  CHECK_EQ(plan.tracks[2].presets[0].file, std::string("03 Bass_Sub - Serum 2.vstpreset"));
  CHECK_EQ(plan.master.size(), (size_t)1);
  CHECK_EQ(plan.master[0].key, pluginKey("", d.masterEffects[0]["id"]));

  const std::string text = bitwigReadme(d, plan, {pluginKey(bass)});
  CHECK(text.find("145 BPM") != std::string::npos);
  CHECK(text.find("MIDI track 1: Lead") != std::string::npos);
  CHECK(text.find("-> 01 Lead - Serum 2.vstpreset") != std::string::npos);
  CHECK(text.find("No notes: Muda") != std::string::npos);
  CHECK(text.find("no preset") != std::string::npos);  // o Bass ficou sem preset: escolhe-se a mao
  CHECK(text.find("Pro-L 2") != std::string::npos);
}

TEST("Bitwig: nomes de ficheiro sem caracteres que o Windows recusa e sem cortar um caracter a meio") {
  CHECK_EQ(safeFilePart("a<b>c:d\"e/f\\g|h?i*j"), std::string("a_b_c_d_e_f_g_h_i_j"));
  CHECK_EQ(safeFilePart("fim. "), std::string("fim"));
  CHECK_EQ(safeFilePart(""), std::string("_"));
  const std::string longName = std::string(59, 'a') + "\xC3\xA7\xC3\xA7";
  const std::string cut = safeFilePart(longName);
  CHECK(cut.size() <= 60);
  CHECK_EQ(cut, std::string(59, 'a'));
}

CORE_TEST_MAIN
