// The AI's memory (Diogo, 2026-11-16: "Gostei" keeps what he approved, to reuse in similar requests; like Meow;
// tied to Core, not to the model). The design follows Meow Memory (github.com/Phant0Meow/dsh-meow-memory, MIT) as
// AICORE's memory review read it: SQLite with BM25 search, a few hits under a budget, importance 1-3 that fades
// with time unless confirmed, append-only events, no model is ever called to organise it. No code was copied.
//
// A local file in %APPDATA%\Core\AI (core-ai-memory.sqlite), apart from the knowledge base (which an update replaces).
//   events   what happened, never rewritten: applied (request, style, the calls, the model), liked, disliked, used
//   recipes  one per applied proposal, derived from the events:
//            candidate (applied, importance 1)  ->  liked (Gostei: confirmed, importance 3)
//                                               ->  disliked (Não gostei: kept, never offered again)
//            a candidate used 3 times in proposals Diogo applied gets importance 2.
// Recipes are Core calls (control/calls.hpp), so any model reads them the same: Claude, DeepSeek, a local one.
// Search: the request's words (FTS5, accents folded); confirmed first, then importance x fading (half-life 90 days
// for importance 1-2; 3 never fades), then bm25. A few hits, cut to a character budget; the request always wins.
#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include "json.hpp"

namespace core::ai {

struct Recipe {
  std::string id;
  std::string request, style, parts, summary, model, status;  // status: candidate | liked | disliked
  std::string note;  // Diogo's words with his Gostei / Não gostei
  nlohmann::json calls = nlohmann::json::array();
  int importance = 1, uses = 0;
  int64_t createdMs = 0, updatedMs = 0;
  nlohmann::json toJson() const;
};

class Memory {
public:
  // Opens (creates) the memory file; null and why when it cannot.
  static std::unique_ptr<Memory> open(const std::filesystem::path& db, std::string* error = nullptr);
  ~Memory();
  Memory(const Memory&) = delete;
  Memory& operator=(const Memory&) = delete;

  // A proposal Diogo applied: a new candidate recipe. `shown` are the recipes the model was given for it (each counts
  // as used). Returns the new recipe's id ("" when the proposal has no calls).
  std::string applied(const std::string& request, const std::string& style, const nlohmann::json& proposal, const std::string& model,
                      const std::vector<std::string>& shown, int64_t nowMs);
  // Gostei / Não gostei, with an optional note in his words. False when there is no such recipe.
  bool feedback(const std::string& recipeId, bool liked, const std::string& note, int64_t nowMs);

  // The recipes for a request, best first, at most `limit`, their JSON together within `budgetChars`.
  std::vector<Recipe> search(const std::string& request, const std::string& style, int64_t nowMs, int limit = 3, size_t budgetChars = 3200) const;
  std::vector<Recipe> all() const;  // newest first
  int size() const;

  // Export: {"format": "core-ai-memory", "version": 1, "recipes": [...], "events": [...]}. Import adds what is not
  // there yet (by id); returns how many recipes were added, -1 and why when the file is not a memory export.
  nlohmann::json exportJson() const;
  int importJson(const nlohmann::json& j, std::string* error = nullptr);

private:
  Memory() = default;
  struct Db;
  std::unique_ptr<Db> db_;
};

// The block the chat adds to a request: the recipes in short form, as data (never instructions). "" with no recipes.
std::string recipesBlock(const std::vector<Recipe>& recipes);

}  // namespace core::ai
