// wl_test.h — a deliberately tiny host-test harness.
//
// No external framework: Stage 1 logic is small and pure, so a header-only
// registry of test functions plus two assertion macros is enough and keeps the
// build trivially portable (just g++/clang, no fetch). Tests self-register at
// static-init time via the WL_TEST macro; main() in test_main.cpp calls
// wltest::run_all().
#pragma once
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace wltest {

struct TestCase {
  const char* name;
  std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}
inline int& failures() {
  static int f = 0;
  return f;
}
inline int& checks() {
  static int c = 0;
  return c;
}

struct Registrar {
  Registrar(const char* n, std::function<void()> fn) { registry().push_back({n, fn}); }
};

inline int run_all() {
  std::printf("Running %zu test(s)...\n\n", registry().size());
  for (auto& t : registry()) {
    int before = failures();
    t.fn();
    bool ok = failures() == before;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", t.name);
  }
  std::printf("\n%d check(s), %d failure(s)\n", checks(), failures());
  return failures() == 0 ? 0 : 1;
}

}  // namespace wltest

// Define a test. Body follows the macro like a function body.
#define WL_TEST(name)                                               \
  static void name();                                               \
  static ::wltest::Registrar wl_reg_##name(#name, name);           \
  static void name()

// Boolean assertion.
#define WL_CHECK(cond)                                              \
  do {                                                             \
    ::wltest::checks()++;                                          \
    if (!(cond)) {                                                 \
      ::wltest::failures()++;                                      \
      std::printf("  FAIL %s:%d  WL_CHECK(%s)\n", __FILE__, __LINE__, #cond); \
    }                                                              \
  } while (0)

// Equality assertion (operands must support == and be printable-free; we only
// report the expressions, not values, to stay type-agnostic).
#define WL_CHECK_EQ(a, b)                                          \
  do {                                                             \
    ::wltest::checks()++;                                          \
    if (!((a) == (b))) {                                           \
      ::wltest::failures()++;                                      \
      std::printf("  FAIL %s:%d  WL_CHECK_EQ(%s, %s)\n", __FILE__, __LINE__, #a, #b); \
    }                                                              \
  } while (0)
