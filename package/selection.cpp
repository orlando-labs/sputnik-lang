#include "package/selection.h"
#include "runtime/sputnik_ext.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <dlfcn.h>
#if defined(__GLIBC__)
#include <gnu/libc-version.h>
#endif
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace sputnik::pkg {
namespace {
bool numeric(const std::string &s) {
  if (s.empty() || s.size() > 64 || s.front() == '.' || s.back() == '.' || s.find("..") != std::string::npos) return false;
  return std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || c == '.'; });
}
std::vector<unsigned> numbers(const std::string &s) {
  std::vector<unsigned> result; std::istringstream input(s); std::string part;
  while (std::getline(input, part, '.')) {
    if (part.size() > 6) throw std::runtime_error("invalid target version");
    if (result.size() == 6) throw std::runtime_error("invalid target version");
    result.push_back(static_cast<unsigned>(std::stoul(part)));
  }
  result.resize(6); return result;
}
bool minimum(const std::string &actual, const std::string &required) {
  return required.empty() || (numeric(actual) && numbers(actual) >= numbers(required));
}
bool includes(const std::vector<std::string> &a, const std::vector<std::string> &b) {
  return std::all_of(b.begin(), b.end(), [&](const auto &x) { return std::find(a.begin(), a.end(), x) != a.end(); });
}
bool backend(const PackageTarget &t, const std::string &name) {
  return t.compute_backend == name || std::find(t.compute_backends.begin(), t.compute_backends.end(), name) != t.compute_backends.end();
}
bool source_native(const PackageManifest &m) {
  return std::any_of(m.native_extensions.begin(), m.native_extensions.end(), [](const auto &n) { return n.kind == "source"; });
}
// Use the host's NVIDIA utility, with a bounded read/timeout and no shell. This
// reads driver/GPU facts without loading an archive or initializing its runtime.
std::string nvidia_facts() {
  const char *program = nullptr;
  for (const char *p : {"/usr/bin/nvidia-smi", "/usr/local/nvidia/bin/nvidia-smi"})
    if (::access(p, X_OK) == 0) { program = p; break; }
  if (!program) return {};
  int pipes[2]; if (::pipe(pipes)) return {};
  const auto child = ::fork();
  if (child == 0) {
    ::dup2(pipes[1], STDOUT_FILENO); const int null = ::open("/dev/null", O_WRONLY);
    if (null >= 0) ::dup2(null, STDERR_FILENO);
    ::close(pipes[0]); ::close(pipes[1]);
    ::execl(program, program, "--query-gpu=index,uuid,driver_version,compute_cap", "--format=csv,noheader,nounits", static_cast<char *>(nullptr));
    ::_exit(127);
  }
  ::close(pipes[1]);
  if (child < 0) { ::close(pipes[0]); return {}; }
  ::fcntl(pipes[0], F_SETFL, O_NONBLOCK);
  std::string output; int status = 0; bool done = false;
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < until) {
    char buffer[4096]; const auto count = ::read(pipes[0], buffer, sizeof(buffer));
    if (count > 0) output.append(buffer, count);
    if (output.size() > 65536) break;
    if (::waitpid(child, &status, WNOHANG) == child) { done = true; break; }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!done) { ::kill(child, SIGKILL); ::waitpid(child, &status, 0); }
  else { char buffer[4096]; ssize_t count; while ((count = ::read(pipes[0], buffer, sizeof(buffer))) > 0) output.append(buffer, count); }
  ::close(pipes[0]);
  return done && WIFEXITED(status) && WEXITSTATUS(status) == 0 && output.size() <= 65536 ? output : std::string{};
}
void cuda_devices(PackageHost &h) {
#if defined(__linux__)
  void *driver = nullptr;
  for (const char *path : {"/usr/lib64/libcuda.so.1", "/usr/lib/x86_64-linux-gnu/libcuda.so.1", "/usr/lib/wsl/lib/libcuda.so.1", "/usr/local/nvidia/lib64/libcuda.so.1"}) {
    driver = ::dlopen(path,RTLD_LAZY|RTLD_LOCAL);
    if (driver) break;
  }
  if (!driver) return;
  struct Close { void *handle; ~Close() { ::dlclose(handle); } } close{driver};
  const auto init = reinterpret_cast<int (*)(unsigned)>(::dlsym(driver,"cuInit"));
  const auto count = reinterpret_cast<int (*)(int *)>(::dlsym(driver,"cuDeviceGetCount"));
  const auto get_device = reinterpret_cast<int (*)(int *,int)>(::dlsym(driver,"cuDeviceGet"));
  const auto attribute = reinterpret_cast<int (*)(int *,int,int)>(::dlsym(driver,"cuDeviceGetAttribute"));
  const auto version = reinterpret_cast<int (*)(int *)>(::dlsym(driver,"cuDriverGetVersion"));
  int devices = 0, api = 0;
  if (!init || !count || !get_device || !attribute || !version || init(0) || count(&devices) || version(&api) || devices<0 || devices>64) return;
  h.cuda_api_version = std::to_string(api/1000)+"."+std::to_string((api%1000)/10);
  for (int i=0; i<devices; ++i) {
    // Stable CUDA driver ABI attribute IDs from cuda.h; no toolkit dependency.
    int device=0,major=0,minor=0;
    if (get_device(&device,i) || attribute(&major,75,device) || attribute(&minor,76,device)) { h.cuda_architectures.clear(); return; }
    h.cuda_architectures.insert(std::to_string(major)+"."+std::to_string(minor));
  }
#else
  (void)h;
#endif
}
// Query the installed HIP userspace through its stable C API. No SDK headers,
// Python process or library from a package archive is involved.
void rocm_devices(PackageHost &host) {
#if defined(__linux__)
  void *runtime = nullptr;
  for (const char *path : {"/opt/rocm/lib/libamdhip64.so.6", "/usr/lib64/libamdhip64.so.6",
                          "/usr/lib/x86_64-linux-gnu/libamdhip64.so.6"}) {
    runtime = ::dlopen(path, RTLD_LAZY | RTLD_LOCAL);
    if (runtime) break;
  }
  if (!runtime) return;
  struct Close { void *handle; ~Close() { ::dlclose(handle); } } close{runtime};
  const auto init = reinterpret_cast<int (*)(unsigned)>(::dlsym(runtime, "hipInit"));
  const auto count = reinterpret_cast<int (*)(int *)>(::dlsym(runtime, "hipGetDeviceCount"));
  int devices = 0;
  host.rocm = init && count && init(0) == 0 && count(&devices) == 0 && devices > 0 && devices <= 64;
#else
  (void)host;
#endif
}
bool narrower(const PackageManifest &a, const PackageManifest &b) {
  const auto &x = a.target; const auto &y = b.target;
  if (x.compute_backend != y.compute_backend && y.compute_backend != "none" &&
      y.compute_backend != "cpu") return false;
  if (y.os != "any" && x.os != y.os) return false;
  if (y.arch != "any" && x.arch != y.arch) return false;
  if (!y.libc.empty() && x.libc != y.libc) return false;
  if (!minimum(x.min_os, y.min_os) || !minimum(x.min_libc, y.min_libc) ||
      !minimum(x.min_cuda_driver, y.min_cuda_driver) || !minimum(x.cuda_version, y.cuda_version) ||
      !includes(x.cpu_features, y.cpu_features)) return false;
  if (!y.cuda_architectures.empty() && (x.cuda_architectures.empty() || !includes(y.cuda_architectures, x.cuda_architectures))) return false;
  return true;
}
}

bool package_gpu_backend(const std::string &name) { return name == "cuda" || name == "rocm"; }

void validate_package_target(const PackageTarget &t) {
  const auto one_of = [](const std::string &v, std::initializer_list<const char *> allowed) {
    return std::any_of(allowed.begin(), allowed.end(), [&](const char *s) { return v == s; });
  };
  if (!one_of(t.os, {"any", "linux", "macos", "windows"}) || !one_of(t.arch, {"any", "x86_64", "aarch64"}) ||
      !one_of(t.libc, {"", "gnu", "musl"}) || !one_of(t.compute_backend, {"none", "cpu", "mps", "cuda", "rocm"}))
    throw std::runtime_error("invalid package target");
  for (const auto *v : {&t.min_os, &t.min_libc, &t.cuda_version, &t.min_cuda_driver})
    if (!v->empty()) { if (!numeric(*v)) throw std::runtime_error("invalid target version"); (void)numbers(*v); }
  for (const auto &b : t.compute_backends)
    if (!one_of(b, {"cpu", "mps", "cuda", "rocm"})) throw std::runtime_error("invalid compute backend");
  for (const auto &a : t.cuda_architectures)
    if (!numeric(a)) throw std::runtime_error("invalid CUDA compute capability");
  if (backend(t, "rocm") && t.os != "linux")
    throw std::runtime_error("ROCm target requires Linux");
  const bool cuda = backend(t, "cuda");
  if (cuda && (t.os != "linux" || t.cuda_version.empty() || t.min_cuda_driver.empty() || t.cuda_architectures.empty()))
    throw std::runtime_error("CUDA target requires Linux, cuda_version, min_cuda_driver and cuda_architectures");
  if (!cuda && (!t.cuda_version.empty() || !t.min_cuda_driver.empty() || !t.cuda_architectures.empty()))
    throw std::runtime_error("CUDA requirements on a non-CUDA target");
}

PackageHost probe_package_host(bool include_accelerators) {
  PackageHost h;
#if defined(__APPLE__)
  h.os = "macos"; char version[128]{}; size_t size = sizeof(version);
  if (!::sysctlbyname("kern.osproductversion", version, &size, nullptr, 0)) h.os_version = version;
  h.mps = true;
#elif defined(__linux__)
  h.os = "linux";
#if defined(__GLIBC__)
  h.libc = "gnu"; h.libc_version = ::gnu_get_libc_version();
#else
  h.libc = "musl";
#endif
#else
  h.os = "unsupported";
#endif
#if defined(__aarch64__)
  h.arch = "aarch64"; h.cpu_features.insert("neon");
#elif defined(__x86_64__)
  h.arch = "x86_64";
#define SPUTNIK_CPU_FEATURE(name) if (__builtin_cpu_supports(name)) h.cpu_features.insert(name)
  SPUTNIK_CPU_FEATURE("sse2"); SPUTNIK_CPU_FEATURE("sse4.2"); SPUTNIK_CPU_FEATURE("avx");
  SPUTNIK_CPU_FEATURE("avx2"); SPUTNIK_CPU_FEATURE("fma"); SPUTNIK_CPU_FEATURE("avx512f");
#undef SPUTNIK_CPU_FEATURE
#endif
  if (!include_accelerators) return h;
  std::istringstream rows(nvidia_facts()); std::string row;
  while (std::getline(rows, row)) {
    std::istringstream fields(row); std::array<std::string,4> values;
    for (auto &v : values) { std::getline(fields,v,','); v.erase(std::remove_if(v.begin(),v.end(),[](unsigned char c) { return std::isspace(c); }),v.end()); }
    if (!numeric(values[2]) || !numeric(values[3])) continue;
    if (h.cuda_driver.empty() || numbers(values[2]) < numbers(h.cuda_driver)) h.cuda_driver = values[2];
  }
  // Driver ordinals follow the process's CUDA_VISIBLE_DEVICES/DEVICE_ORDER.
  // NVML/nvidia-smi indices alone cannot identify those visible devices.
  cuda_devices(h);
  rocm_devices(h);
  return h;
}
bool package_has_native(const PackageManifest &m) {
  return m.has_native_payload || !m.native_extensions.empty() || !m.native_providers.empty() || m.kind != "library" || m.bundle != "none";
}
std::string package_incompatibility(const PackageManifest &m, const PackageHost &h, const VariantPolicy &p, bool execution) {
  if (!m.compatibility_error.empty()) return m.compatibility_error;
  if (!language::satisfies(h.sputnik_version, m.sputnik_requirement))
    return "LANGUAGE_VERSION_MISMATCH: requires Sputnik " + m.sputnik_requirement + ", host is " + h.sputnik_version;
  validate_package_target(m.target);
  const auto &t = m.target;
  if (!p.variant.empty() && p.variant != m.variant) return "different variant";
  if (t.os != "any" && t.os != h.os) return "OS mismatch";
  if (t.arch != "any" && t.arch != h.arch) return "process architecture mismatch";
  if (!minimum(h.os_version, t.min_os)) return "OS is too old";
  if (!t.libc.empty() && t.libc != h.libc) return "libc mismatch";
  if (!minimum(h.libc_version, t.min_libc)) return "libc is too old";
  for (const auto &f : t.cpu_features) if (!h.cpu_features.count(f)) return "missing CPU feature: " + f;
  if (!execution) {
    if (p.bundle != "auto" && m.bundle != p.bundle) return "different bundle policy";
    if (package_has_native(m) && !p.allow_native) return "native package requires --allow-native";
    if (source_native(m) && !p.build_from_source) return "native compilation requires --build-from-source";
    if (p.build_from_source && package_has_native(m) && !source_native(m)) return "source build requested";
  }
  if (p.backend != "auto" && p.variant.empty() && t.compute_backend != "none" && !backend(t,p.backend)) return "different compute backend";
  const auto b = execution ? t.compute_backend : p.backend;
  if ((b == "mps" || t.compute_backend == "mps") && !h.mps) return "MPS is unavailable";
  if (((b == "rocm" && backend(t, "rocm")) || t.compute_backend == "rocm") && !h.rocm)
    return "ROCm GPU/runtime is unavailable";
  if ((b == "cuda" && backend(t,"cuda")) || t.compute_backend == "cuda") {
    if (h.cuda_architectures.empty() || !minimum(h.cuda_driver, t.min_cuda_driver)) return "CUDA GPU/driver is unavailable or too old";
    if (!minimum(h.cuda_api_version,t.cuda_version)) return "CUDA driver API is too old for the bundled runtime";
    for (const auto &a : h.cuda_architectures)
      if (std::find(t.cuda_architectures.begin(), t.cuda_architectures.end(), a) == t.cuda_architectures.end()) return "CUDA compute capability is unsupported: " + a;
  }
  for (const auto &n : m.native_extensions)
    if (!language::compatible_producer(n.sputnik_version, h.sputnik_version)) return "Sputnik extension language version mismatch";
  for (const auto &r : p.native_requirements) {
    const auto provider = std::find_if(m.native_providers.begin(),m.native_providers.end(),[&](const auto &n) { return n.id == r.id && n.kind == r.kind; });
    if (provider == m.native_providers.end() || provider->version != r.version ||
        (!r.abi.empty() && provider->abi != r.abi) || (!r.build_id.empty() && provider->build_id != r.build_id))
      return "NATIVE_ABI_CONFLICT: " + r.id + " requires " + r.version + " ABI " + r.abi;
  }
  return {};
}
int select_package_variant(const std::vector<PackageManifest> &c, const PackageHost &h, const VariantPolicy &p, bool execution) {
  std::vector<int> candidates;
  for (size_t i = 0; i < c.size(); ++i) if (package_incompatibility(c[i],h,p,execution).empty()) candidates.push_back(static_cast<int>(i));
  if (candidates.empty()) return -1;
  // Automatic selection uses compatible accelerators first. Explicit CPU and
  // backend policies still constrain eligibility, and equally suitable GPU
  // families remain ambiguous rather than depending on discovery order.
  const auto accelerated = [&](const PackageManifest &m) {
    return p.backend == "auto" &&
        ((backend(m.target, "cuda") && !h.cuda_architectures.empty() &&
          minimum(h.cuda_driver, m.target.min_cuda_driver) &&
          minimum(h.cuda_api_version, m.target.cuda_version) &&
          std::all_of(h.cuda_architectures.begin(), h.cuda_architectures.end(),
              [&](const auto &arch) { return std::find(m.target.cuda_architectures.begin(),
                  m.target.cuda_architectures.end(), arch) != m.target.cuda_architectures.end(); })) ||
         (backend(m.target, "rocm") && h.rocm) ||
         (backend(m.target, "mps") && h.mps));
  };
  const bool has_accelerated = std::any_of(candidates.begin(), candidates.end(),
      [&](int i) { return accelerated(c[i]); });
  if (has_accelerated)
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
        [&](int i) { return !accelerated(c[i]); }), candidates.end());
  const auto delivery_rank = [&](const PackageManifest &m) {
    // No compilation is implicit: source adapters already require consent at
    // install time. Prepared execution and auto bundle selection prefer a
    // prebuilt runtime over rebuilding the same target from thin sources.
    return (source_native(m) ? 0 : 100) + (p.bundle != "auto" ? 0 :
        m.bundle == "runtime" ? 20 : m.bundle == "none" ? 10 : 0);
  };
  // With equal target/native selectors, prefer all-source module delivery.
  // Bytecode/mixed alternatives remain available through an explicit variant.
  // Distinct native builds with equivalent selectors are still ambiguous.
  const auto binary = [](const PackageManifest &m) {
    return m.has_bytecode_delivery || std::any_of(m.modules.begin(),m.modules.end(),
        [](const auto &mod) { return mod.representation == "bytecode"; });
  };
  const auto same_native = [](const PackageManifest &a, const PackageManifest &b) {
    if (a.bundle != b.bundle || a.kind != b.kind || a.native_extensions.size() != b.native_extensions.size()) return false;
    for (size_t i=0; i<a.native_extensions.size(); ++i)
      if (a.native_extensions[i].kind != b.native_extensions[i].kind) return false;
    return true;
  };
  std::vector<int> best;
  for (int i : candidates) {
    bool dominated = false;
    for (int j : candidates) if (i != j &&
        ((narrower(c[j],c[i]) && !narrower(c[i],c[j])) ||
         (narrower(c[j],c[i]) && narrower(c[i],c[j]) && delivery_rank(c[j]) > delivery_rank(c[i])) ||
         (narrower(c[j],c[i]) && narrower(c[i],c[j]) && same_native(c[j],c[i]) && !binary(c[j]) && binary(c[i])))) { dominated = true; break; }
    if (!dominated) best.push_back(i);
  }
  if (best.size() != 1) throw std::runtime_error("ARTIFACT_SELECTION_AMBIGUOUS: use --variant");
  return best.front();
}
} // namespace sputnik::pkg
