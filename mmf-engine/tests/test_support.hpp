// Minimal dependency-free test harness (no GTest/Catch required).
#pragma once

#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace mmftest {

struct TestContext {
  int checks{0};
  int failures{0};
  std::vector<std::string> messages;

  void check(bool condition, const char* expression, const char* file, int line,
             const std::string& detail = "") {
    ++checks;
    if (condition) return;
    ++failures;
    std::ostringstream out;
    out << file << ":" << line << ": CHECK(" << expression << ") failed";
    if (!detail.empty()) out << " — " << detail;
    messages.push_back(out.str());
  }
};

using TestFunction = void (*)(TestContext&);

class Registry {
 public:
  static Registry& instance() {
    static Registry registry;
    return registry;
  }

  void add(std::string name, TestFunction fn) { tests_.emplace_back(std::move(name), fn); }

  int runAll(const std::vector<std::string>& filters, bool verbose) {
    int executed = 0;
    int failedSuites = 0;
    for (const auto& [name, fn] : tests_) {
      if (!filters.empty()) {
        bool selected = false;
        for (const auto& filter : filters) {
          if (name.find(filter) != std::string::npos) selected = true;
        }
        if (!selected) continue;
      }
      ++executed;
      TestContext ctx;
      std::cout << "[ RUN      ] " << name << std::endl;
      try {
        fn(ctx);
      } catch (const std::exception& ex) {
        ctx.check(false, "no exception", __FILE__, __LINE__, std::string("uncaught: ") + ex.what());
      } catch (...) {
        ctx.check(false, "no exception", __FILE__, __LINE__, "uncaught non-std exception");
      }
      if (ctx.failures == 0) {
        std::cout << "[       OK ] " << name << " (" << ctx.checks << " checks)" << std::endl;
      } else {
        ++failedSuites;
        std::cout << "[   FAILED ] " << name << " (" << ctx.failures << "/" << ctx.checks
                  << " checks failed)" << std::endl;
        for (const auto& message : ctx.messages) std::cout << "             " << message << std::endl;
      }
      if (verbose) std::cout << "             " << ctx.checks << " checks" << std::endl;
    }
    std::cout << "\n" << (failedSuites == 0 ? "PASSED" : "FAILED") << ": " << (executed - failedSuites)
              << "/" << executed << " test suites" << std::endl;
    return failedSuites == 0 ? 0 : 1;
  }

  [[nodiscard]] std::size_t count() const { return tests_.size(); }

 private:
  std::vector<std::pair<std::string, TestFunction>> tests_;
};

struct Registrar {
  Registrar(const char* name, TestFunction fn) { Registry::instance().add(name, fn); }
};

}  // namespace mmftest

#define MMF_TEST_SUITE(name)                                                       \
  static void name(mmftest::TestContext&);                                         \
  static mmftest::Registrar mmf_registrar_##name(#name, &name);                    \
  static void name(mmftest::TestContext& ctx)

#define CHECK(condition) ctx.check((condition), #condition, __FILE__, __LINE__)

#define CHECK_MSG(condition, detail) \
  ctx.check((condition), #condition, __FILE__, __LINE__, (detail))

template <class A, class B>
inline void checkEqualImpl(mmftest::TestContext& ctx, const A& actual, const B& expected,
                           const char* expression, const char* file, int line) {
  if (actual == expected) {
    ++ctx.checks;
    return;
  }
  std::ostringstream out;
  out << "expected " << expected << ", got " << actual;
  ctx.check(false, expression, file, line, out.str());
}

#define CHECK_EQ(actual, expected) \
  checkEqualImpl(ctx, (actual), (expected), #actual " == " #expected, __FILE__, __LINE__)

#define CHECK_THROWS(expression, exceptionType)                                        \
  do {                                                                                 \
    bool threw = false;                                                                \
    try {                                                                              \
      (void)(expression);                                                              \
    } catch (const exceptionType&) {                                                   \
      threw = true;                                                                    \
    } catch (...) {                                                                    \
    }                                                                                  \
    ctx.check(threw, "throws " #exceptionType ": " #expression, __FILE__, __LINE__);    \
  } while (false)
