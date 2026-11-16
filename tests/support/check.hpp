// A small test harness for Core's suites (no dependencies). Rules it enforces, from the 2026-11-15 review of the old
// suites: a failed check prints what was measured and what was expected; a check that cannot run is a failure, never a
// silent skip; nothing here compares against a bit-exact golden tied to one compiler.
#pragma once
#include <cmath>
#include <cstdio>
#include <exception>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace coretest {

struct Case { const char* name; std::function<void()> fn; };
inline std::vector<Case>& cases() { static std::vector<Case> c; return c; }
inline int& failures() { static int f = 0; return f; }
inline int& checks() { static int c = 0; return c; }
struct Register { Register(const char* n, std::function<void()> f) { cases().push_back({n, std::move(f)}); } };

inline void fail(const char* file, int line, const std::string& what) {
  ++failures();
  std::fprintf(stderr, "  FALHOU %s:%d: %s\n", file, line, what.c_str());
}

template <class A, class B> std::string pair(const A& a, const B& b) {
  std::ostringstream s; s.precision(17); s << "medido " << a << ", esperado " << b; return s.str();
}

// Runs every case (or only those whose name contains argv[1]); exit code 0 = all green.
inline int runAll(int argc, char** argv) {
  const std::string only = argc > 1 ? argv[1] : "";
  int ran = 0;
  for (auto& c : cases()) {
    if (!only.empty() && std::string(c.name).find(only) == std::string::npos) continue;
    ++ran;
    const int before = failures();
    try { c.fn(); }
    catch (const std::exception& e) { fail(c.name, 0, std::string("excepcao: ") + e.what()); }
    catch (...) { fail(c.name, 0, "excepcao desconhecida"); }
    std::printf("%s %s\n", failures() == before ? "ok  " : "FAIL", c.name);
  }
  if (ran == 0) { std::fprintf(stderr, "nenhum teste corresponde a '%s'\n", only.c_str()); return 1; }
  std::printf("%d testes, %d verificacoes, %d falhas\n", ran, checks(), failures());
  return failures() == 0 ? 0 : 1;
}

}  // namespace coretest

#define CT_CAT2(a, b) a##b
#define CT_CAT(a, b) CT_CAT2(a, b)
#define TEST(name) \
  static void CT_CAT(test_, __LINE__)(); \
  static coretest::Register CT_CAT(reg_, __LINE__)(name, CT_CAT(test_, __LINE__)); \
  static void CT_CAT(test_, __LINE__)()

#define CHECK(cond) do { ++coretest::checks(); if (!(cond)) coretest::fail(__FILE__, __LINE__, #cond); } while (0)
#define CHECK_EQ(a, b) do { ++coretest::checks(); auto _a = (a); auto _b = (b); \
  if (!(_a == _b)) coretest::fail(__FILE__, __LINE__, std::string(#a " == " #b ": ") + coretest::pair(_a, _b)); } while (0)
#define CHECK_NEAR(a, b, tol) do { ++coretest::checks(); double _a = (a), _b = (b), _t = (tol); \
  if (!(std::fabs(_a - _b) <= _t)) coretest::fail(__FILE__, __LINE__, std::string(#a " ~ " #b ": ") + coretest::pair(_a, _b) + " (tol " + std::to_string(_t) + ")"); } while (0)
#define CHECK_THROWS(expr) do { ++coretest::checks(); bool _t = false; try { (void)(expr); } catch (...) { _t = true; } \
  if (!_t) coretest::fail(__FILE__, __LINE__, "esperava excepcao: " #expr); } while (0)
#define CORE_TEST_MAIN int main(int argc, char** argv) { return coretest::runAll(argc, argv); }
