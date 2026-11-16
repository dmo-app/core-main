#include "knowledge.hpp"
#include <cctype>
#include <fstream>
#include "json.hpp"
#include <set>
#include "sqlite3.h"

namespace core::ai {

using json = nlohmann::json;

struct Knowledge::Db {
  sqlite3* h = nullptr;
  ~Db() { if (h) sqlite3_close(h); }
};

namespace {

std::string pathText(const std::filesystem::path& p) {
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}

bool exec(sqlite3* h, const char* sql, std::string* error) {
  char* msg = nullptr;
  if (sqlite3_exec(h, sql, nullptr, nullptr, &msg) == SQLITE_OK) return true;
  if (error) *error = msg ? msg : "sqlite error";
  sqlite3_free(msg);
  return false;
}

}  // namespace

// The words of a question as an FTS5 query: each word quoted (no operators get through), any of them.
std::string ftsQuery(const std::string& q) {
  static const std::set<std::string> stop = {"a", "o", "e", "de", "do", "da", "dos", "das", "em", "no", "na", "um", "uma", "para", "com",
                                             "que", "como", "the", "of", "and", "to", "in", "on", "for", "is", "how", "what", "os", "as"};
  std::string out, word;
  auto flush = [&] {
    std::string lower;
    for (unsigned char c : word) lower += (char)std::tolower(c);
    if (lower.size() >= 2 && !stop.count(lower)) out += (out.empty() ? "" : " OR ") + std::string("\"") + word + "\"";
    word.clear();
  };
  for (unsigned char c : q) {
    if (std::isalnum(c) || c >= 0x80 || c == '-') word += (char)c;
    else flush();
  }
  flush();
  return out;
}

int buildKnowledge(const std::filesystem::path& jsonl, const std::filesystem::path& dbPath, std::string* error) {
  std::ifstream in(jsonl, std::ios::binary);
  if (!in) {
    if (error) *error = "cannot read " + pathText(jsonl);
    return -1;
  }
  std::string line;
  if (!std::getline(in, line)) {
    if (error) *error = "empty file";
    return -1;
  }
  const json head = json::parse(line, nullptr, false);
  if (!head.is_object() || head.value("format", std::string()) != "core-ai-knowledge") {
    if (error) *error = "not a core-ai-knowledge file";
    return -1;
  }
  std::error_code ec;
  std::filesystem::create_directories(dbPath.parent_path(), ec);
  const std::filesystem::path tmp = dbPath.string() + ".tmp";
  std::filesystem::remove(tmp, ec);
  sqlite3* h = nullptr;
  if (sqlite3_open(pathText(tmp).c_str(), &h) != SQLITE_OK) {
    if (error) *error = "cannot create " + pathText(tmp);
    sqlite3_close(h);
    return -1;
  }
  int n = 0;
  bool ok = exec(h, "PRAGMA journal_mode=OFF; BEGIN;"
                    "CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT);"
                    "CREATE VIRTUAL TABLE chunks USING fts5(title, text, style UNINDEXED, source UNINDEXED, tokenize='unicode61 remove_diacritics 2');",
                error);
  sqlite3_stmt* ins = nullptr;
  ok = ok && sqlite3_prepare_v2(h, "INSERT INTO chunks(title, text, style, source) VALUES(?,?,?,?)", -1, &ins, nullptr) == SQLITE_OK;
  while (ok && std::getline(in, line)) {
    const json r = json::parse(line, nullptr, false);
    if (!r.is_object()) continue;
    const std::string fields[4] = {r.value("title", std::string()), r.value("text", std::string()), r.value("style", std::string()),
                                   r.value("source", std::string())};
    for (int i = 0; i < 4; ++i) sqlite3_bind_text(ins, i + 1, fields[i].c_str(), (int)fields[i].size(), SQLITE_TRANSIENT);
    ok = sqlite3_step(ins) == SQLITE_DONE;
    sqlite3_reset(ins);
    ++n;
  }
  sqlite3_finalize(ins);
  if (ok) {
    const std::string meta = "INSERT INTO meta VALUES('format','core-ai-knowledge'),('from'," +
                             std::string("'") + [&] { std::string s = head.value("from", std::string()); std::string o; for (char c : s) if (c != '\'') o += c; return o; }() +
                             "'); COMMIT; INSERT INTO chunks(chunks) VALUES('optimize');";
    ok = exec(h, meta.c_str(), error);
  } else if (error && error->empty()) {
    *error = sqlite3_errmsg(h);
  }
  sqlite3_close(h);
  if (!ok) {
    std::filesystem::remove(tmp, ec);
    return -1;
  }
  std::filesystem::rename(tmp, dbPath, ec);
  if (ec) {
    std::filesystem::remove(dbPath, ec);
    std::filesystem::rename(tmp, dbPath, ec);
  }
  if (ec) {
    if (error) *error = "cannot write " + pathText(dbPath);
    return -1;
  }
  return n;
}

std::unique_ptr<Knowledge> Knowledge::open(const std::filesystem::path& path, std::string* error) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    if (error) *error = "no knowledge base at " + pathText(path);
    return nullptr;
  }
  std::unique_ptr<Knowledge> k(new Knowledge());
  k->db_ = std::make_unique<Db>();
  if (sqlite3_open_v2(pathText(path).c_str(), &k->db_->h, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK || k->size() < 0) {
    if (error) *error = "not a knowledge base: " + pathText(path);
    return nullptr;
  }
  return k;
}

Knowledge::~Knowledge() = default;

int Knowledge::size() const {
  sqlite3_stmt* st = nullptr;
  int n = -1;
  if (sqlite3_prepare_v2(db_->h, "SELECT count(*) FROM chunks", -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
  sqlite3_finalize(st);
  return n;
}

std::string Knowledge::from() const {
  sqlite3_stmt* st = nullptr;
  std::string out;
  if (sqlite3_prepare_v2(db_->h, "SELECT value FROM meta WHERE key='from'", -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
    out = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
  sqlite3_finalize(st);
  return out;
}

std::vector<KnowledgeHit> Knowledge::search(const std::string& query, const std::string& style, int limit) const {
  std::vector<KnowledgeHit> out;
  const std::string q = ftsQuery(query);
  if (q.empty() || !db_ || !db_->h) return out;
  // Title words weigh more; another style's chunks go after this style's and the general ones.
  const char* sql =
      "SELECT source, title, style, text, bm25(chunks, 4.0, 1.0) AS r FROM chunks WHERE chunks MATCH ?1 "
      "ORDER BY (CASE WHEN ?2 = '' OR style = '' OR style = ?2 THEN 0 ELSE 1 END), r LIMIT ?3";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_->h, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, q.c_str(), (int)q.size(), SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, style.c_str(), (int)style.size(), SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 3, limit < 1 ? 1 : limit > 20 ? 20 : limit);
  auto col = [&](int i) { const unsigned char* t = sqlite3_column_text(st, i); return t ? std::string(reinterpret_cast<const char*>(t)) : std::string(); };
  while (sqlite3_step(st) == SQLITE_ROW) out.push_back({col(0), col(1), col(2), col(3), sqlite3_column_double(st, 4)});
  sqlite3_finalize(st);
  return out;
}

}  // namespace core::ai
