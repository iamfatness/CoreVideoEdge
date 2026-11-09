#include "TestHarness.h"

#include <cstring>

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int run = 0;
  for (const auto& c : edge_test::registry()) {
    const std::string full = std::string(c.suite) + "." + c.name;
    if (filter && full.find(filter) == std::string::npos) continue;
    const int before = edge_test::failures();
    c.body();
    ++run;
    std::fprintf(stderr, "%s %s\n", edge_test::failures() == before ? "[ OK ]" : "[FAIL]", full.c_str());
  }
  std::fprintf(stderr, "%d test(s), %d failure(s)\n", run, edge_test::failures());
  return edge_test::failures() == 0 && run > 0 ? 0 : 1;
}
