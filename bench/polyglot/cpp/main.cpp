#include <cstdint>
#include <iostream>
#include <cstdlib>

namespace {

constexpr std::int64_t kIterations = 1000000;
constexpr std::int64_t kLimit = 2147483647;

inline std::int64_t add(std::int64_t a, std::int64_t b) {
#ifdef SPUTNIK_BENCH_CHECKED_INTS
  std::int64_t result;
  if (__builtin_add_overflow(a, b, &result)) std::abort();
  return result;
#else
  return a + b;
#endif
}
inline std::int64_t sub(std::int64_t a, std::int64_t b) {
#ifdef SPUTNIK_BENCH_CHECKED_INTS
  std::int64_t result;
  if (__builtin_sub_overflow(a, b, &result)) std::abort();
  return result;
#else
  return a - b;
#endif
}

std::int64_t main_workload() {
  std::int64_t i = 0;
  std::int64_t a = 1;
  std::int64_t b = 2;
  std::int64_t checksum = 0;
  while (i < kIterations) {
    a = add(add(a, b), 3);
    if (a > kLimit) {
      a = sub(a, kLimit);
    }
    b = add(add(b, a), i);
    if (b > kLimit) {
      b = sub(b, kLimit);
    }
    if (a > b) {
      checksum = sub(add(checksum, a), b);
    } else {
      checksum = sub(add(checksum, b), a);
    }
    i = add(i, 1);
  }
  return add(add(checksum, a), b);
}

} // namespace

int main() {
  std::cout << main_workload() << '\n';
  return 0;
}
