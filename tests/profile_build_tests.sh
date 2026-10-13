#!/bin/sh
set -eu
repository=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
test_root=$(mktemp -d "${TMPDIR:-/tmp}/sputnik-profile-build.XXXXXX")
trap 'rm -rf -- "$test_root"' EXIT HUP INT TERM
tool="$repository/tools/profile-build"

profdata() {
  if [ -n "${LLVM_PROFDATA:-}" ]; then
    "$LLVM_PROFDATA" "$@"
  elif command -v llvm-profdata >/dev/null 2>&1; then
    llvm-profdata "$@"
  else
    xcrun llvm-profdata "$@"
  fi
}

training="$test_root/training dir'quoted"
optimized="$test_root/optimized dir'quoted"
cat > "$test_root/check.cpp" <<'EOF'
int main(int argc, char **) {
  volatile int sum = 0;
  for (int i = 0; i < argc * 100; ++i) sum += i;
  return sum == argc * 100 * (argc * 100 - 1) / 2 ? 0 : 1;
}
EOF
"$tool" native-instrument "$training" > "$test_root/training-wrapper.txt"
"$training/cxx" -O2 "$test_root/check.cpp" -o "$test_root/check"
# Exercise both branch counts before merging the profiles.
"$test_root/check"
"$test_root/check" another-input
profdata merge -o "$test_root/merged.profdata" "$training"/raw/*.profraw
"$tool" native-optimize "$optimized" "$test_root/merged.profdata" > "$test_root/optimized-wrapper.txt"
cmp "$test_root/merged.profdata" "$optimized/profile.profdata"
# The wrapper must use its snapshot, not retain the caller's mutable file.
printf 'replaced input\n' > "$test_root/merged.profdata"
"$optimized/cxx" -O2 "$test_root/check.cpp" -o "$test_root/optimized-check"
"$test_root/optimized-check"
"$test_root/optimized-check" another-input

if "$tool" native-instrument "$training" > "$test_root/reuse.log" 2>&1; then
  echo 'existing directory was reused' >&2; exit 1
fi
if "$tool" vm-optimize "$test_root/missing" "$test_root/absent.profdata" > "$test_root/missing.log" 2>&1; then
  echo 'missing profile was accepted' >&2; exit 1
fi
[ ! -e "$test_root/missing" ]
if JOBS=00 "$tool" native-instrument "$test_root/zero-jobs" > "$test_root/jobs.log" 2>&1; then
  echo 'zero JOBS was accepted' >&2; exit 1
fi
[ ! -e "$test_root/zero-jobs" ]
# Exercise the VM workflow on a tiny real build with different CLI/test main
# control flow. Applying the CLI profile to either test main is a CFG mismatch.
fixture="$test_root/vm-fixture"
mkdir -p "$fixture/tools" "$fixture/tests"
cp "$tool" "$fixture/tools/profile-build"
cat > "$fixture/runtime.cpp" <<'EOF'
int answer(int n) { return n > 0 ? n + 1 : 0; }
EOF
cat > "$fixture/cli.cpp" <<'EOF'
int answer(int);
int main(int argc, char **) { return answer(argc) == argc + 1 ? 0 : 1; }
EOF
cat > "$fixture/tests/vm_tests.cpp" <<'EOF'
int answer(int);
int main() {
  for (int i = 0; i < 20; ++i) if (answer(i) != (i > 0 ? i + 1 : 0)) return 1;
  return 0;
}
EOF
cp "$fixture/tests/vm_tests.cpp" "$fixture/tests/sputnik_ext_tests.cpp"
cat > "$fixture/Makefile" <<'EOF'
$(BUILD_DIR)/obj/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(BUILD_DIR)/runtime.a: $(BUILD_DIR)/obj/runtime.o
	$(AR) rcs $@ $^
$(BUILD_DIR)/sputnik: $(BUILD_DIR)/obj/cli.o $(BUILD_DIR)/runtime.a
	$(CXX) $(CXXFLAGS) $^ -o $@
$(BUILD_DIR)/vm_tests: $(BUILD_DIR)/obj/tests/vm_tests.o $(BUILD_DIR)/runtime.a
	$(CXX) $(CXXFLAGS) $^ -o $@
$(BUILD_DIR)/sputnik_ext_tests: $(BUILD_DIR)/obj/tests/sputnik_ext_tests.o $(BUILD_DIR)/runtime.a
	$(CXX) $(CXXFLAGS) $^ -o $@
EOF
"$fixture/tools/profile-build" vm-instrument "$fixture/generate" > "$test_root/vm-generate.log"
"$fixture/generate/sputnik"
profdata merge -o "$fixture/vm.profdata" "$fixture"/generate/raw/*.profraw
"$fixture/tools/profile-build" vm-optimize "$fixture/use" "$fixture/vm.profdata" > "$test_root/vm-use.log"
"$fixture/use/sputnik"

printf 'profile_build_tests: ok\n'
