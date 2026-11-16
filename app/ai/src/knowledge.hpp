// The AI's local knowledge base (Diogo, 2026-11-16): how to make music in each style, from CORE-MANUAL, in a SQLite
// file beside Core (ai/core-ai-knowledge.sqlite, shipped and removed with the AI package). The AI consults it with a
// tool (consultar_manual) instead of carrying the manual in every request.
// Built from ai/knowledge/core-manual.jsonl (tools/ai-knowledge/extract_manual.py) by buildKnowledge; searched with
// FTS5 (bm25, accents folded).
#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace core::ai {

struct KnowledgeHit {
  std::string source, title, style, text;
  double rank = 0;  // bm25: lower is better
};

// Builds (replaces) the database from the JSONL; returns the number of chunks, or -1 and why.
int buildKnowledge(const std::filesystem::path& jsonl, const std::filesystem::path& db, std::string* error = nullptr);

class Knowledge {
public:
  // Opens the database read-only; null (and why) when it is missing or not a knowledge base.
  static std::unique_ptr<Knowledge> open(const std::filesystem::path& db, std::string* error = nullptr);
  ~Knowledge();
  Knowledge(const Knowledge&) = delete;
  Knowledge& operator=(const Knowledge&) = delete;

  // The best chunks for the words of `query` (any of them; the more, the better). `style` ("darkpsycore") puts that
  // style's chunks and the general ones first. Never throws: an empty list when nothing matches.
  std::vector<KnowledgeHit> search(const std::string& query, const std::string& style = "", int limit = 4) const;
  int size() const;
  std::string from() const;  // "dmo-app/core-manual@8dc3a33"

private:
  Knowledge() = default;
  struct Db;
  std::unique_ptr<Db> db_;
};

}  // namespace core::ai
