#ifndef YAMUX_TEST_FRAMEWORK_HPP
#define YAMUX_TEST_FRAMEWORK_HPP

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

struct TestCase {
  std::string name;
  std::function<void()> fn;
};

inline std::vector<TestCase>& GetTests() {
  static std::vector<TestCase> tests;
  return tests;
}

struct TestRegistrar {
  TestRegistrar(const char* name, std::function<void()> fn) {
    GetTests().push_back({name, std::move(fn)});
  }
};

#define TEST(name)                                           \
  static void test_##name();                                 \
  static TestRegistrar registrar_##name(#name, test_##name); \
  static void test_##name()

#define ASSERT_TRUE(expr)                                              \
  do {                                                                 \
    if (!(expr)) {                                                     \
      fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
      exit(1);                                                         \
    }                                                                  \
  } while (0)

#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))
#define ASSERT_NE(a, b) ASSERT_TRUE((a) != (b))

#endif  // YAMUX_TEST_FRAMEWORK_HPP
