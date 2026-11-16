// #78 MMCSS (Diogo 2026-11-16, "Tornar o Core leve e fiável", fase 1): os workers do tempo real (RtWorkerPool, M5)
// registam-se como o thread do dispositivo (Windows: MMCSS "Pro Audio") e devolvem o registo no teardown, no mesmo
// thread, uma vez cada. Um registo que falha não impede nada (fallback: prioridade normal) e é contado.
// Aqui com um init/exit falsos que contam e guardam o thread; o registo real só existe no Windows (A/B no PC).
// Correr: rt_tests [parte-do-nome]
#include <atomic>
#include <map>
#include <mutex>
#include <thread>
#include "support/check.hpp"
#include "core/rt_workers.hpp"
#include "core/server.hpp"
#include "io/device.hpp"
#include <cstdlib>

using namespace core;

namespace {
void setEnv(const char* name, const char* value) {  // "" = unset
#ifdef _WIN32
  _putenv_s(name, value);
#else
  if (*value) setenv(name, value, 1); else unsetenv(name);
#endif
}
std::mutex gMutex;
std::map<void*, std::thread::id> gLive;  // token -> thread que o registou (ainda não devolvido)
int gInits = 0, gExits = 0, gWrongThread = 0, gUnknownToken = 0;
bool gFail = false;
std::atomic<uintptr_t> gNext{1};

void* fakeInit() {
  std::lock_guard<std::mutex> lock(gMutex);
  ++gInits;
  if (gFail) return nullptr;
  void* token = reinterpret_cast<void*>(gNext.fetch_add(1) * 16);
  gLive[token] = std::this_thread::get_id();
  return token;
}
void fakeExit(void* token) {
  std::lock_guard<std::mutex> lock(gMutex);
  ++gExits;
  auto it = gLive.find(token);
  if (it == gLive.end()) { ++gUnknownToken; return; }
  if (it->second != std::this_thread::get_id()) ++gWrongThread;
  gLive.erase(it);
}
void reset(bool fail) {
  std::lock_guard<std::mutex> lock(gMutex);
  gLive.clear();
  gInits = gExits = gWrongThread = gUnknownToken = 0;
  gFail = fail;
  rtThreadExit().store(&fakeExit);
  rtThreadInit().store(&fakeInit);
}
void off() { rtThreadInit().store(nullptr); rtThreadExit().store(nullptr); }

struct Sum { std::atomic<int> total{0}; };
void addJob(void* c, int i) { static_cast<Sum*>(c)->total.fetch_add(i + 1); }

// Corre trabalhos até todos os workers terem arrancado pelo menos uma vez (teto: 5 s, falha alto se não chegar).
bool wakeAll(RtWorkerPool& pool) {
  Sum sum;
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (pool.registered() < pool.threads() && std::chrono::steady_clock::now() < until) {
    while (!pool.idle()) std::this_thread::yield();
    pool.run(&addJob, &sum, 64, RtWorkerPool::now() + 1000000000ull);
  }
  return pool.registered() == pool.threads();
}
}  // namespace

TEST("mmcss: cada worker regista-se uma vez e devolve o registo no teardown, no próprio thread") {
  reset(false);
  {
    RtWorkerPool pool(3);
    CHECK(wakeAll(pool));
    CHECK_EQ(pool.registered(), 3);
    CHECK_EQ(pool.registrationFailures(), 0);
    std::lock_guard<std::mutex> lock(gMutex);
    CHECK_EQ(gInits, 3);
    CHECK_EQ(gExits, 0);  // enquanto o pool vive, os registos ficam
  }
  std::lock_guard<std::mutex> lock(gMutex);
  CHECK_EQ(gExits, 3);
  CHECK_EQ(gLive.size(), (size_t)0);
  CHECK_EQ(gWrongThread, 0);
  CHECK_EQ(gUnknownToken, 0);
  off();
}

TEST("mmcss: registo falhado = fallback, os trabalhos correm na mesma, contado, nada a devolver") {
  reset(true);
  {
    RtWorkerPool pool(4);
    CHECK(wakeAll(pool));
    CHECK_EQ(pool.registrationFailures(), 4);
    Sum sum;
    while (!pool.idle()) std::this_thread::yield();
    CHECK(pool.run(&addJob, &sum, 100, RtWorkerPool::now() + 2000000000ull));
    CHECK_EQ(sum.total.load(), 5050);  // 1 + 2 + ... + 100: cada trabalho uma vez
  }
  std::lock_guard<std::mutex> lock(gMutex);
  CHECK_EQ(gExits, 0);  // um token nulo nunca vai ao exit
  off();
}

TEST("mmcss: 50 reinícios do motor (pool novo) não deixam registos pendurados") {
  reset(false);
  for (int i = 0; i < 50; ++i) {
    RtWorkerPool pool(2);
    CHECK(wakeAll(pool));
  }
  std::lock_guard<std::mutex> lock(gMutex);
  CHECK_EQ(gInits, 100);
  CHECK_EQ(gExits, 100);
  CHECK_EQ(gLive.size(), (size_t)0);
  CHECK_EQ(gWrongThread, 0);
  off();
}

TEST("mmcss: um worker que nunca teve trabalho não se regista nem devolve nada") {
  reset(false);
  { RtWorkerPool pool(2); }
  std::lock_guard<std::mutex> lock(gMutex);
  CHECK_EQ(gInits, 0);
  CHECK_EQ(gExits, 0);
  off();
}

// Decisão do Diogo (2026-11-16, cartão "Desligar até passar o soak"): o M5 fica desligado por defeito até ao soak de
// 8 h; CORE_LIVE_THREADS=4 volta a ligá-lo num comando.
TEST("m5: sem CORE_LIVE_THREADS o motor arranca serial (0 workers); CORE_LIVE_THREADS=4 liga-o") {
  setEnv("CORE_LIVE_THREADS", "");
  CHECK_EQ(defaultLiveThreads(), 0);
  {
    Server server([] { return makeMiniaudioOutput(); }, 48000);
    json r = server.handle({{"cmd", "engine.start"}, {"args", {{"backend", "null"}}}});
    CHECK(r.value("ok", false));
    CHECK_EQ(server.handle({{"cmd", "engine.stats"}})["result"]["liveThreads"].get<int>(), 0);
    server.handle({{"cmd", "engine.stop"}});
  }
  setEnv("CORE_LIVE_THREADS", "4");
  CHECK_EQ(defaultLiveThreads(), 4);
  {
    Server server([] { return makeMiniaudioOutput(); }, 48000);
    json r = server.handle({{"cmd", "engine.start"}, {"args", {{"backend", "null"}}}});
    CHECK(r.value("ok", false));
    CHECK_EQ(server.handle({{"cmd", "engine.stats"}})["result"]["liveThreads"].get<int>(), 4);
    server.handle({{"cmd", "engine.stop"}});
  }
  setEnv("CORE_LIVE_THREADS", "0");
  CHECK_EQ(defaultLiveThreads(), 0);
  setEnv("CORE_LIVE_THREADS", "");
}

CORE_TEST_MAIN
