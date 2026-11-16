// core-ai-bench (2026-11-16): what Core's AI costs and how well it does, with the real API and Core's own chat code.
// Core had an AI chat inside it once (core-old, 3-5 Nov 2026) and it was taken out because it cost too much; this
// measures the new one before it ships.
//
// Each run is one message of a scenario (a beat, a song structure, a change to a part) on one model and one effort.
// The chat is the app's (ai::Chat): the same tools, catalog and checks. The bench sends its requests with curl (built
// into Windows 10+), and for each run records the cost, the requests, the tokens, the time, the calls accepted and
// left out, and whether the proposal applies to the song. It stops before a run that could pass the money cap.
//
//   core-ai-bench --out <folder> [--models claude-haiku-5-5,claude-sonnet-5-5] [--efforts low,medium]
//                 [--scenarios beat,structure,change] [--max-usd 2] [--knowledge <core-ai-knowledge.sqlite>]
//                 [--pcore <pcore/styles.json>] [--key-file <file>] [--dry-run]
// The key: ANTHROPIC_API_KEY, else --key-file (default %APPDATA%\Core\AI\anthropic-key.txt). It goes to curl in a
// temporary header file that is deleted after each request; it is never printed or written in the report.
// --dry-run sends nothing: it prints the plan and the size of the first request.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include "ai_chat.hpp"
#include "edit.hpp"
#include "knowledge.hpp"
#include "pcore.hpp"
#include "score.hpp"

using namespace core;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
const char* kCurl = "curl.exe";
#else
const char* kCurl = "curl";
#endif

struct Scenario {
  std::string id, prompt;
  bool fromTemplate;  // start from a template song shaped darkpsycore-64 and arranged (else an empty song)
};

const std::vector<Scenario>& scenarios() {
  static const std::vector<Scenario> s = {
      {"beat", "Faz uma batida dark psycore para o drop: kick e baixo KBBB, 8 compassos, alinhados.", false},
      {"structure", "Cria a estrutura de uma música dark psycore de 64 compassos e arranja as pistas pelas secções.", false},
      {"change", "No drop, tira os hats e faz outra variação do baixo, mais agressiva.", true},
  };
  return s;
}

std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  for (std::string x; std::getline(ss, x, ',');)
    if (!x.empty()) out.push_back(x);
  return out;
}

std::string readFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::string trim(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r')) ++i;
  return s.substr(i);
}

std::string apiKey(const std::string& keyFile) {
  if (const char* e = std::getenv("ANTHROPIC_API_KEY"); e && *e) return trim(e);
  fs::path f = keyFile;
  if (f.empty())
    if (const char* appdata = std::getenv("APPDATA")) f = fs::path(appdata) / "Core" / "AI" / "anthropic-key.txt";
  return !f.empty() && fs::exists(f) ? trim(readFile(f)) : std::string();
}

// One POST through curl. The body and the headers go in temporary files (the key never on the command line).
json post(const ai::HttpRequest& req, const fs::path& tmp, std::string* failure) {
  const fs::path body = tmp / "bench-body.json", headers = tmp / "bench-headers.txt";
  {
    std::ofstream b(body, std::ios::binary);
    b << req.body.dump(-1, ' ', false, json::error_handler_t::replace);
    std::ofstream h(headers, std::ios::binary);
    for (const auto& [k, v] : req.headers) h << k << ": " << v << "\n";
  }
  const std::string cmd = std::string(kCurl) + " -s -S --max-time 300 -X POST \"" + req.url + "\" -H @\"" + headers.string() +
                          "\" --data-binary @\"" + body.string() + "\"";
  std::string out;
  if (FILE* p = popen(cmd.c_str(), "r")) {
    char buf[65536];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, p)) > 0;) out.append(buf, n);
    pclose(p);
  }
  std::error_code ec;
  fs::remove(headers, ec);
  fs::remove(body, ec);
  json r = json::parse(out, nullptr, false);
  if (r.is_discarded()) {
    *failure = out.empty() ? "no answer (curl failed)" : "not JSON: " + out.substr(0, 200);
    return nullptr;
  }
  return r;
}

// A worst-case guess of one run's cost, to stop before passing the cap: maxSteps requests of ~12k input tokens
// uncached and maxTokens output.
double worstCase(const ai::Config& c) {
  const ai::Prices p = ai::pricesOf(c.model);
  return c.maxSteps * (12000 * p.cacheWrite + 2000.0 * p.output) / 1e6;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> models = {"claude-haiku-5-5", "claude-sonnet-5-5"}, efforts = {"low", "medium"}, wanted;
  double maxUsd = 2.0;
  fs::path out, knowledgeDb, pcorePath, keyFile;
  bool dry = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--models") models = split(next());
    else if (a == "--efforts") efforts = split(next());
    else if (a == "--scenarios") wanted = split(next());
    else if (a == "--max-usd") maxUsd = std::atof(next().c_str());
    else if (a == "--out") out = next();
    else if (a == "--knowledge") knowledgeDb = next();
    else if (a == "--pcore") pcorePath = next();
    else if (a == "--key-file") keyFile = next();
    else if (a == "--dry-run") dry = true;
    else {
      std::cerr << "unknown argument " << a << "\n";
      return 2;
    }
  }
  if (out.empty()) {
    std::cerr << "usage: core-ai-bench --out <folder> [--models ...] [--efforts ...] [--scenarios ...] [--max-usd 2] [--knowledge db] [--pcore styles.json] [--dry-run]\n";
    return 2;
  }
  if (pcorePath.empty()) pcorePath = fs::path(argv[0]).parent_path() / "pcore" / "styles.json";
  if (!fs::exists(pcorePath)) {
    std::cerr << "no P-Core catalog at " << pcorePath.string() << " (--pcore)\n";
    return 2;
  }
  const doc::PCoreCatalog cat = doc::loadPCoreCatalog(pcorePath.string());
  std::unique_ptr<ai::Knowledge> knowledge;
  if (!knowledgeDb.empty()) {
    std::string why;
    knowledge = ai::Knowledge::open(knowledgeDb, &why);
    if (!knowledge) {
      std::cerr << "knowledge base: " << why << "\n";
      return 2;
    }
  }
  const std::string key = dry ? std::string() : apiKey(keyFile.string());
  if (!dry && key.empty()) {
    std::cerr << "no key: set ANTHROPIC_API_KEY or --key-file\n";
    return 2;
  }
  fs::create_directories(out);
  const auto functions = control::buildFunctions(&cat);

  json runs = json::array();
  double spent = 0;
  bool capped = false;
  for (const auto& sc : scenarios()) {
    if (!wanted.empty() && std::find(wanted.begin(), wanted.end(), sc.id) == wanted.end()) continue;
    for (const auto& model : models)
      for (const auto& effort : efforts) {
        ai::Config cfg;
        cfg.model = model;
        cfg.effort = effort;
        cfg.provider.apiKey = key;
        if (spent + worstCase(cfg) > maxUsd) {
          std::cerr << "stop: the next run (" << sc.id << ", " << model << ", " << effort << ") could pass the cap of $" << maxUsd << "\n";
          capped = true;
          break;
        }
        doc::Document song = sc.fromTemplate ? doc::newTemplateDocument() : doc::newEmptyDocument();
        if (sc.fromTemplate) {  // a song with its sections, so "the drop" exists
          doc::setScoreShape(song, "darkpsycore-64");
          std::vector<std::string> ids;
          for (const auto& tr : song.tracks) ids.push_back(tr.id);
          doc::scoreArrange(song, ids);
        }
        song.name = "Bench " + sc.id;
        ai::Chat chat(cfg, functions, &cat, knowledge.get());
        json body = chat.ask(sc.prompt, song);
        if (dry) {
          std::cout << sc.id << " " << model << " " << effort << ": first request " << body.dump().size() << " bytes, "
                    << body["tools"].size() << " tools\n";
          continue;
        }
        const auto t0 = std::chrono::steady_clock::now();
        std::string failure;
        while (!body.is_null()) {
          const json r = post(chat.http(body), out, &failure);
          if (!failure.empty()) {
            chat.abandon();
            break;
          }
          body = chat.onResponse(r, song);
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const ai::Usage& u = chat.usage();
        const double cost = chat.cost(u);
        spent += cost;
        // Does the proposal apply? On a copy, as the app does.
        const json proposal = chat.proposal();
        const control::Proposal p = control::validateProposal(proposal, functions, song, nullptr);
        bool applies = false;
        std::string applyError;
        if (!p.calls.empty()) {
          try {
            doc::Document copy = song;
            control::ApplyEnv env;
            env.pcore = &cat;
            control::applyProposal(copy, p, "", env);
            applies = true;
          } catch (const std::exception& e) {
            applyError = e.what();
          }
        }
        int leftOut = 0;
        for (const auto& l : chat.log()) leftOut += l.rfind("left out", 0) == 0 || l.rfind("not allowed", 0) == 0;
        json run = {{"scenario", sc.id}, {"model", model}, {"effort", effort}, {"usd", cost}, {"requests", u.requests},
                    {"input", u.input}, {"output", u.output}, {"cacheRead", u.cacheRead}, {"cacheWrite", u.cacheWrite},
                    {"seconds", seconds}, {"calls", proposal["calls"].size()}, {"leftOut", leftOut}, {"applies", applies},
                    {"problems", p.problems}, {"reply", chat.reply()}, {"error", failure.empty() ? chat.error() : failure},
                    {"log", chat.log()}, {"proposal", proposal}};
        if (!applyError.empty()) run["applyError"] = applyError;
        runs.push_back(run);
        std::printf("%-9s %-18s %-6s $%.4f  %d req  %5.1f s  %2zu calls  %d left out  %s%s\n", sc.id.c_str(), model.c_str(),
                    effort.c_str(), cost, u.requests, seconds, proposal["calls"].size(), leftOut, applies ? "applies" : "does not apply",
                    run["error"].get<std::string>().empty() ? "" : ("  (" + run["error"].get<std::string>() + ")").c_str());
        std::fflush(stdout);
      }
    if (capped) break;
  }
  if (dry) return 0;

  // The report: JSON (everything) and Markdown (the table).
  const json report = {{"format", "core-ai-bench"}, {"version", 1}, {"spentUsd", spent}, {"maxUsd", maxUsd}, {"capped", capped},
                       {"knowledge", knowledge != nullptr}, {"runs", runs}};
  std::ofstream(out / "ai-bench.json", std::ios::binary) << report.dump(1, ' ', false, json::error_handler_t::replace);
  std::ofstream md(out / "ai-bench.md", std::ios::binary);
  md << "# Core AI bench\n\nSpent: $" << spent << " (cap $" << maxUsd << ")" << (capped ? ", stopped at the cap" : "") << "\n\n";
  md << "| scenario | model | effort | USD | requests | seconds | calls | left out | applies | error |\n|---|---|---|---|---|---|---|---|---|---|\n";
  for (const auto& r : runs) {
    char usd[32];
    std::snprintf(usd, sizeof usd, "%.4f", r["usd"].get<double>());
    char sec[32];
    std::snprintf(sec, sizeof sec, "%.1f", r["seconds"].get<double>());
    md << "| " << r["scenario"].get<std::string>() << " | " << r["model"].get<std::string>() << " | " << r["effort"].get<std::string>() << " | "
       << usd << " | " << r["requests"].get<int>() << " | " << sec << " | " << r["calls"].get<size_t>() << " | " << r["leftOut"].get<int>()
       << " | " << (r["applies"].get<bool>() ? "yes" : "no") << " | " << r["error"].get<std::string>() << " |\n";
  }
  md << "\n## Replies\n\n";
  for (const auto& r : runs)
    md << "**" << r["scenario"].get<std::string>() << " / " << r["model"].get<std::string>() << " / " << r["effort"].get<std::string>()
       << "**: " << r["reply"].get<std::string>() << "\n\n";
  std::printf("spent $%.4f; report in %s\n", spent, (out / "ai-bench.md").string().c_str());
  return 0;
}
