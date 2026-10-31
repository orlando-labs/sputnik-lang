#include "runtime/semver.h"
#include <iostream>
#include <stdexcept>
using namespace sputnik::language;
void check(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
int main() {
  Version a, b;
  check(parse(SPUTNIK_VERSION, a), "current release is SemVer");
  check(a.major == SPUTNIK_VERSION_MAJOR && a.minor == SPUTNIK_VERSION_MINOR &&
        a.patch == SPUTNIK_VERSION_PATCH, "one release identity");
  for (const auto *invalid : {"1", "1.0", "01.0.0", "1.00.0", "1.0.00", "1.0.0-01",
                              "1.0.0-", "1.0.0+", "1.0.0+x..y", "1.0.0.1", "v1.0.0"})
    check(!parse(invalid, a), "invalid SemVer accepted");
  const char *ordered[] = {"1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
                           "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0", "1.0.1", "1.1.0", "2.0.0"};
  for (unsigned i = 1; i < sizeof(ordered) / sizeof(*ordered); ++i) {
    check(parse(ordered[i-1], a) && parse(ordered[i], b) && compare(a,b) < 0, "SemVer precedence");
  }
  check(parse("1.0.0+a", a) && parse("1.0.0+b", b) && compare(a,b) == 0, "build metadata affects precedence");
  check(satisfies("1.0.0", baseline_requirement), "baseline release");
  check(satisfies("1.99.99", baseline_requirement), "compatible minor/patch update");
  check(!satisfies("2.0.0", baseline_requirement), "breaking major accepted");
  check(!satisfies("1.0.1", ">=1.0.2 <2.0.0"), "minimum patch ignored");
  check(satisfies("1.4.0", "^1.2.3") && !satisfies("2.0.0", "^1.2.3"), "caret range");
  check(satisfies("0.2.8", "^0.2.3") && !satisfies("0.3.0", "^0.2.3"), "zero major caret range");
  check(satisfies("1.2.9", "~1.2.3") && !satisfies("1.3.0", "~1.2.3"), "tilde range");
  check(satisfies("2.0.0", "1.0.0 || >=2.0.0 <3.0.0"), "range union");
  check(!satisfies("1.1.0-rc.1", baseline_requirement), "implicit prerelease accepted");
  check(satisfies("1.1.0-rc.2", ">=1.1.0-rc.1 <1.1.0"), "explicit prerelease rejected");
  for (const auto *invalid : {"", "*", ">=1.0.0", ">=1.0.0 || *", "^1.0", "1.0.0 ||", ">==1.0.0", "1.0.0 junk"})
    check(!valid_requirement(invalid), "invalid or unbounded requirement accepted");
  check(compatible_producer("1.0.0", "1.2.1"), "old extension rejected by compatible VM");
  check(!compatible_producer("1.2.1", "1.2.0"), "new extension accepted by old VM");
  check(!compatible_producer("1.0.0", "2.0.0"), "extension crossed breaking major");
  std::cout << "language_version_tests: PASS\n";
}
