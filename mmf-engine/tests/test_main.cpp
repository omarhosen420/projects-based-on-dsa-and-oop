// Test runner: every test file self-registers a suite via MMF_TEST_SUITE.
#include <string>
#include <vector>

#include "test_support.hpp"

int main(int argc, char** argv) {
  std::vector<std::string> filters;
  bool verbose = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--verbose" || arg == "-v") {
      verbose = true;
    } else if (arg == "--list") {
      std::cout << mmftest::Registry::instance().count() << " test suites registered\n";
      return 0;
    } else {
      filters.push_back(arg);
    }
  }
  return mmftest::Registry::instance().runAll(filters, verbose);
}
