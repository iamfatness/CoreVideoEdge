#pragma once

// Minimal self-registering test harness: no third-party download, no SDK.

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace edge_test {

struct Case {
  const char* suite;
  const char* name;
  std::function<void()> body;
};

inline std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

inline int& failures() {
  static int count = 0;
  return count;
}

struct Registrar {
  Registrar(const char* suite, const char* name, std::function<void()> body) {
    registry().push_back({suite, name, std::move(body)});
  }
};

inline void fail(const char* file, int line, const std::string& message) {
  ++failures();
  std::fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, message.c_str());
}

template <typename T>
void print(std::ostringstream& out, const T& value) {
  if constexpr (requires { out << value; }) out << value;
  else if constexpr (std::is_enum_v<T>) out << static_cast<long long>(value);
  else out << "(unprintable)";
}

template <typename A, typename B>
std::string describe(const A& a, const B& b) {
  std::ostringstream out;
  out << "expected [";
  print(out, b);
  out << "] got [";
  print(out, a);
  out << "]";
  return out.str();
}

}  // namespace edge_test

#define EDGE_CAT2(a, b) a##b
#define EDGE_CAT(a, b) EDGE_CAT2(a, b)
#define TEST(suite, name)                                                                     \
  static void EDGE_CAT(test_, EDGE_CAT(suite, name))();                                       \
  static edge_test::Registrar EDGE_CAT(reg_, EDGE_CAT(suite, name))(#suite, #name,            \
                                                                    EDGE_CAT(test_, EDGE_CAT(suite, name))); \
  static void EDGE_CAT(test_, EDGE_CAT(suite, name))()

#define EXPECT_TRUE(cond) \
  do { if (!(cond)) edge_test::fail(__FILE__, __LINE__, "expected true: " #cond); } while (0)
#define EXPECT_FALSE(cond) \
  do { if (cond) edge_test::fail(__FILE__, __LINE__, "expected false: " #cond); } while (0)
#define EXPECT_EQ(actual, expected)                                                                   \
  do {                                                                                                \
    const auto& edge_a = (actual);                                                                    \
    const auto& edge_b = (expected);                                                                  \
    if (!(edge_a == edge_b)) edge_test::fail(__FILE__, __LINE__, #actual ": " + edge_test::describe(edge_a, edge_b)); \
  } while (0)
#define ASSERT_TRUE(cond)                                                          \
  do {                                                                             \
    if (!(cond)) { edge_test::fail(__FILE__, __LINE__, "required: " #cond); return; } \
  } while (0)
