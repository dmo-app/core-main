// The sheet of a template (Diogo 2026-11-09: "essa ficha deveria ser informação em qualquer template, e não separado"):
// inside every template, in its "ficha" field (saveTemplate and core-template-upgrade write it), a readable text with
// everything the template uses: tempo, sections, every track's instrument with each
// parameter by name, value and unit (kick tail, Hz, ms...), the samples (file, sha256, keys, loop), the
// W-Core modulation, each effect's parameters, the mix, sends, LFOs and automation, and the notes in short.
// It is what an AI (or a person) reads to know how the passage was made; the template stays the source.
#pragma once
#include <string>
#include "templates.hpp"

namespace core::doc {

// Markdown, Portuguese headings. The same template always gives the same text.
std::string templateSheet(const SongTemplate& t);

// <file without ".core-template.json"> + ending, e.g. templateCompanionPath(".../x.core-template.json", ".wav").
std::string templateCompanionPath(const std::string& templateFile, const std::string& ending);

} // namespace core::doc
