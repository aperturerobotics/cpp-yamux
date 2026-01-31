#include "test_framework.hpp"

int main() {
  int passed = 0;
  int failed = 0;

  for (const auto& test : GetTests()) {
    printf("Running %s... ", test.name.c_str());
    fflush(stdout);
    try {
      test.fn();
      printf("PASS\n");
      passed++;
    } catch (const std::exception& e) {
      printf("FAIL: %s\n", e.what());
      failed++;
    } catch (...) {
      printf("FAIL: unknown exception\n");
      failed++;
    }
  }

  printf("\n%d passed, %d failed\n", passed, failed);
  return failed > 0 ? 1 : 0;
}
