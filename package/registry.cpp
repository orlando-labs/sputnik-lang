#include "package/registry.h"
#include "package/registry_json.h"
#include "package/distribution.h"
#include "package/store.h"
#include "runtime/net_http_transport.h"
#include "frontend/lexer/token.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <set>
#include <sstream>
#include <thread>
#include <fcntl.h>
#include <openssl/rand.h>
#include <sys/stat.h>
#include <unistd.h>

namespace sputnik::pkg {
namespace {
namespace http = runtime::http;
namespace fs = std::filesystem;
using registry_json::Value;
[[noreturn]] void fail(const std::string &message) { throw std::runtime_error("registry: " + message); }
std::string quote(const std::string &s) {
  std::ostringstream out; out << '"';
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') out << '\\' << char(c);
    else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c);
    else out << char(c);
  }
  out << '"'; return out.str();
}
std::string random_id() {
  unsigned char bytes[16];
  if (RAND_bytes(bytes, sizeof(bytes)) != 1) fail("secure randomness unavailable");
  std::ostringstream out;
  for (auto c : bytes) out << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
  return out.str();
}
std::string read_private(const fs::path &path) {
  int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) fail("cannot open credentials/session file: " + path.string());
  struct stat info{};
  if (::fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_uid != ::getuid() ||
      (info.st_mode & 077) || info.st_size > 65536) {
    ::close(fd); fail("credentials/session must be an owned private regular file (mode 0600)");
  }
  std::string text; char buffer[8192]; ssize_t count;
  while ((count = ::read(fd, buffer, sizeof(buffer))) > 0) {
    text.append(buffer, count);
    if (text.size() > 65536) { ::close(fd); fail("credentials/session file exceeds 64 KiB"); }
  }
  ::close(fd); if (count < 0) fail("credentials/session read failed");
  return text;
}
void save_private(const fs::path &path, const std::string &text) {
  const auto parent = fs::absolute(path).parent_path();
  if (fs::create_directories(parent)) fs::permissions(parent, fs::perms::owner_all);
  const auto temporary = parent / (".sputnik-" + random_id());
  int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  if (fd < 0) fail("cannot create private credentials/session file");
  std::size_t offset = 0;
  while (offset < text.size()) {
    auto count = ::write(fd, text.data() + offset, text.size() - offset);
    if (count <= 0) { ::close(fd); fs::remove(temporary); fail("private file write failed"); }
    offset += count;
  }
  if (::fsync(fd)) { ::close(fd); fs::remove(temporary); fail("private file sync failed"); }
  ::close(fd);
  try { fs::rename(temporary, path); }
  catch (...) { fs::remove(temporary); throw; }
  fd = ::open(parent.c_str(), O_RDONLY); if (fd >= 0) { ::fsync(fd); ::close(fd); }
}
bool part(const std::string &s, bool scope = false) {
  if (s.empty() || s.size() > (scope ? 63U : 64U) || s[0] < 'a' || s[0] > 'z') return false;
  return std::all_of(s.begin(), s.end(), [scope](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || (!scope && (c == '.' || c == '-'));
  }) && s.find("..") == std::string::npos && s.back() != '.' && s.back() != '-';
}
bool release_version(const std::string &s) {
  unsigned dots = 0; bool digit = false; bool suffix = false;
  for (char c : s) {
    if (!suffix && c == '.' && digit && dots < 2) { ++dots; digit = false; }
    else if (!suffix && c == '-' && dots == 2 && digit) { suffix = true; digit = false; }
    else if (c >= '0' && c <= '9') digit = true;
    else if (suffix && ((c >= 'a' && c <= 'z') || c == '.' || c == '-')) digit = true;
    else return false;
  }
  return digit && dots == 2 && s.size() <= 128;
}
std::string base(const std::string &ref) {
  if (!registry_reference(ref)) fail("invalid package reference: " + ref);
  auto slash = ref.find('/');
  return slash == std::string::npos ? "/api/v1/packages/" + ref :
      "/api/v1/namespaces/" + ref.substr(0, slash) + "/packages/" + ref.substr(slash + 1);
}
std::uint64_t size_field(const Value &v) {
  if (v.kind != Value::Number || v.text.empty() || v.text.size() > 10 ||
      v.text.find_first_not_of("0123456789") != std::string::npos) fail("invalid artifact size");
  return std::stoull(v.text);
}
class APIError : public std::runtime_error {
public:
  int status;
  std::string code;
  APIError(int status, std::string code, const std::string &message)
      : std::runtime_error("registry: " + message), status(status), code(std::move(code)) {}
};
class API {
  std::string origin_, token_;
public:
  API(std::string origin, const fs::path &credentials) : origin_(std::move(origin)) {
    http::HttpUrl parsed; http::HttpErrorKind kind; std::string error;
    if (!http::http_parse_url(origin_, &parsed, &kind, &error) || parsed.target != "/" ||
        origin_.find_first_of("?#@") != std::string::npos || origin_.back() == '/' ||
        (parsed.scheme != "https" && !(parsed.scheme == "http" &&
           (parsed.host == "127.0.0.1" || parsed.host == "localhost" || parsed.host == "::1"))))
      fail("expected HTTPS origin (or loopback HTTP)");
    if (!credentials.empty()) {
      const auto text = read_private(credentials);
      auto doc = registry_json::Parser(text).parse();
      if (doc.at("origin").string() != origin_) fail("credential origin differs from registry");
      token_ = doc.at("token").string();
      if (token_.rfind("spreg_", 0) || token_.size() > 256 ||
          token_.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos)
        fail("invalid credential token");
    }
  }
  void token(std::string value) { token_ = std::move(value); }
  http::HttpResponseStartResult request(const std::string &method, const std::string &path,
      const std::string &body = {}, const std::string &key = {}) {
    http::HttpRequest request; http::HttpErrorKind kind; std::string error;
    http::HttpHeaders headers;
    headers.add("connection", "close", &error);
    headers.add("accept-encoding", "identity", &error);
    headers.add("content-type", "application/json", &error);
    if (!key.empty()) headers.add("idempotency-key", key, &error);
    if (!token_.empty()) headers.add("authorization", "Bearer " + token_, &error);
    if (!http::http_build_request(method, origin_ + path, headers, body, method != "GET", true, &request, &kind, &error)) fail(error);
    auto transport = http::http_connect(request, std::chrono::seconds(30), {}, &kind, &error);
    if (!transport || !http::http_write_request_head(*transport, request, &kind, &error)) fail(error);
    if (!body.empty() && !transport->write_all(body, &error)) fail(error);
    auto response = http::http_read_response_start(std::move(transport));
    if (!response.ok) fail(response.error_message);
    if (response.status < 200 || response.status >= 300) {
      std::string text, code;
      if (response.body->read_all(65536, &text, &kind, &error)) {
        try {
          auto value = registry_json::Parser(text).parse().at("error").at("code").string();
          if (value.size() <= 80 && value.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") == std::string::npos) code = " " + value;
        } catch (...) {}
      }
      throw APIError(response.status, code.empty() ? "" : code.substr(1),
          "HTTP " + std::to_string(response.status) + code + " for " + path +
          (response.status == 401 ? "; use pkg login or --credentials" : ""));
    }
    if (response.headers.contains("content-encoding")) fail("encoded registry response");
    return response;
  }
  http::HttpResponseStartResult get(const std::string &path) {
    auto response = request("GET", path);
    if (response.status != 200) fail("expected HTTP 200 for " + path);
    return response;
  }
  std::string text(const std::string &method, const std::string &path, const std::string &body = {}, const std::string &key = {}) {
    auto response = request(method, path, body, key); std::string text, error; http::HttpErrorKind kind;
    if (!response.body->read_all(1048576, &text, &kind, &error)) fail(error);
    return text;
  }
  Value post(const std::string &path, const std::string &body, const std::string &key = {}) {
    const auto result = text("POST", path, body, key);
    return registry_json::Parser(result).parse();
  }
  void upload(const std::string &path, const fs::path &file, std::uint64_t size) {
    http::HttpRequest request; http::HttpErrorKind kind; std::string error;
    http::HttpHeaders headers;
    headers.add("authorization", "Bearer " + token_, &error);
    headers.add("content-type", "application/vnd.sputnik.package.v1", &error);
    headers.add("connection", "close", &error);
    if (!http::http_build_streaming_request("PUT", origin_ + path, headers, size, true, &request, &kind, &error)) fail(error);
    auto transport = http::http_connect(request, std::chrono::seconds(30), {}, &kind, &error);
    if (!transport || !http::http_write_request_head(*transport, request, &kind, &error)) fail(error);
    std::ifstream input(file, std::ios::binary); char buffer[65536]; std::uint64_t written = 0;
    while (input.read(buffer, sizeof(buffer)) || input.gcount()) {
      if (!http::http_write_request_body_chunk(*transport, request, std::string(buffer, input.gcount()), &written, &kind, &error)) fail(error);
    }
    if (!input.eof() || !http::http_finish_request_body(*transport, request, written, &kind, &error)) fail("upload read/framing failed");
    auto response = http::http_read_response_start(std::move(transport));
    if (!response.ok || response.status != 200) fail("artifact upload failed; retry the same publish command");
    std::string result;
    if (!response.body->read_all(1048576, &result, &kind, &error)) fail(error);
  }
  Value document(const std::string &path, std::size_t *bytes = nullptr) {
    auto response = get(path); std::string text, error; http::HttpErrorKind kind;
    if (!response.body->read_all(1048576, &text, &kind, &error)) fail(error);
    if (bytes) *bytes = text.size();
    return registry_json::Parser(text).parse();
  }
};

std::string canonical_reference(const std::string &name, const Value &document) {
  const auto canonical = document.at("package_ref").string();
  if (!registry_reference(canonical) ||
      (canonical != name && (name.find('/') != std::string::npos ||
       canonical.find('/') == std::string::npos ||
       canonical.substr(canonical.find('/') + 1) != name)))
    fail("invalid root alias identity");
  return canonical;
}

std::string canonical_package(API &api, const std::string &name) {
  return canonical_reference(name, api.document(base(name)));
}

void write_json(std::ostream &out, const Value &value) {
  switch (value.kind) {
  case Value::Null: out << "null"; break;
  case Value::Boolean:
  case Value::Number: out << value.text; break;
  case Value::String: out << quote(value.text); break;
  case Value::Array: {
    out << '[';
    bool comma = false;
    for (const auto &item : value.array) {
      if (comma) out << ',';
      comma = true;
      write_json(out, item);
    }
    out << ']';
    break;
  }
  case Value::Object: {
    out << '{';
    bool comma = false;
    for (const auto &item : value.object) {
      if (comma) out << ',';
      comma = true;
      out << quote(item.first) << ':';
      write_json(out, item.second);
    }
    out << '}';
    break;
  }
  }
}

bool boolean_field(const Value &value) {
  if (value.kind != Value::Boolean) fail("expected registry boolean");
  return value.truth();
}

void validate_release(const Value &release, const std::string &canonical,
                      const std::string &version) {
  if (release.at("package_ref").string() != canonical ||
      release.at("version").string() != version || !release_version(version))
    fail("invalid release identity");
  (void)boolean_field(release.at("yanked"));
}

Value complete_release_index(API &api, const std::string &reference) {
  const auto location = base(reference) + "/index";
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    try {
      Value result;
      std::set<std::string> cursors, versions;
      std::string path = location, revision, canonical;
      std::size_t bytes = 0;
      for (;;) {
        std::size_t page_bytes = 0;
        auto page = api.document(path, &page_bytes);
        bytes += page_bytes;
        if (bytes > 16U * 1024U * 1024U) fail("release index exceeds 16 MiB");
        const auto identity = canonical_reference(reference, page);
        if (page.at("versions").kind != Value::Array ||
            page.at("revision").kind != Value::Number)
          fail("invalid release index");
        if (result.kind == Value::Null) {
          canonical = identity;
          revision = page.at("revision").text;
          result = page;
          result.object.at("versions").array.clear();
        } else if (identity != canonical) {
          fail("release index package identity changed");
        } else if (page.at("revision").text != revision) {
          fail("release index revision changed without INDEX_CHANGED");
        }
        for (auto &release : page.object.at("versions").array) {
          const auto version = release.at("version").string();
          validate_release(release, canonical, version);
          if (!versions.insert(version).second) fail("duplicate release in index");
          if (versions.size() > 10000) fail("release index exceeds 10000 versions");
          result.object.at("versions").array.push_back(std::move(release));
        }
        const auto &cursor = page.at("next_cursor");
        if (cursor.kind == Value::Null) break;
        const auto next = cursor.string();
        if (next.empty() || next.size() > 4096 ||
            !cursors.insert(next).second || cursors.size() > 1000 ||
            next.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=._-") != std::string::npos)
          fail("invalid pagination cursor");
        std::string escaped;
        for (char c : next) {
          if (c == '+') escaped += "%2B";
          else if (c == '/') escaped += "%2F";
          else if (c == '=') escaped += "%3D";
          else escaped += c;
        }
        path = location + "?cursor=" + escaped;
      }
      auto &releases = result.object.at("versions").array;
      std::sort(releases.begin(), releases.end(), [](const Value &a, const Value &b) {
        return version_less(b.at("version").text, a.at("version").text);
      });
      result.object.at("next_cursor") = Value{};
      return result;
    } catch (const APIError &error) {
      if (error.status != 409 || error.code != "INDEX_CHANGED") throw;
    }
  }
  fail("release index kept changing; retry the command");
}

PackageManifest registry_artifact_manifest(const Value &a) {
    PackageManifest m; m.variant = a.at("variant").string();
    if (!a.object.count("compatibility") || !a.at("compatibility").object.count("sputnik_requirement")) {
      m.compatibility_error = "LANGUAGE_REQUIREMENT_MISSING: rebuild the package with requires.sputnik";
    } else {
      m.sputnik_requirement = a.at("compatibility").at("sputnik_requirement").string();
      if (!language::valid_requirement(m.sputnik_requirement)) fail("invalid registry Sputnik language requirement");
    }
    const auto string = [](const Value &object, const std::string &key, std::string &out) { if (object.object.count(key)) out = object.at(key).string(); };
    const auto strings = [](const Value &object, const std::string &key, std::vector<std::string> &out) {
      if (!object.object.count(key)) return;
      if (object.at(key).kind != Value::Array) fail("invalid target requirements");
      for (const auto &x : object.at(key).array) out.push_back(x.string());
    };
    string(a,"kind",m.kind); string(a,"bundle",m.bundle);
    if (a.object.count("target")) {
      const auto &t = a.at("target");
      string(t,"os",m.target.os); string(t,"arch",m.target.arch); string(t,"min_os",m.target.min_os);
      string(t,"libc",m.target.libc); string(t,"min_libc",m.target.min_libc); string(t,"compute_backend",m.target.compute_backend);
      string(t,"cuda_version",m.target.cuda_version); string(t,"min_cuda_driver",m.target.min_cuda_driver);
      strings(t,"cpu_features",m.target.cpu_features); strings(t,"compute_backends",m.target.compute_backends); strings(t,"cuda_architectures",m.target.cuda_architectures);
    } else if (m.variant != "source" && m.variant != "bytecode") fail("native variant lacks target metadata");
    std::string mode = "none"; string(a,"native_mode",mode);
    m.has_native_payload = a.object.count("native_payload") && a.at("native_payload").truth();
    if (mode != "none") {
      if (!a.object.count("target") || !a.object.count("compatibility")) fail("native variant lacks compatibility metadata");
      PackageNativeExtension n; n.kind = mode == "source" || mode == "mixed" ? "source" : "prebuilt";
      const auto &compatibility = a.at("compatibility");
      // Third-party runtime/SDK providers do not export Sputnik entry points.
      const bool provider_only = compatibility.object.count("sputnik_versions") &&
          compatibility.at("sputnik_versions").array.empty() &&
          compatibility.object.count("native_providers") &&
          !compatibility.at("native_providers").array.empty();
      if (!provider_only && (!compatibility.object.count("sputnik_versions") || compatibility.at("sputnik_versions").array.empty())) m.compatibility_error = "LANGUAGE_REQUIREMENT_MISSING: native producer version is absent";
      else for (const auto &version : compatibility.at("sputnik_versions").array) {
        if (!language::compatible_producer(version.string())) m.compatibility_error = "LANGUAGE_VERSION_MISMATCH: native producer is Sputnik " + version.string();
        n.sputnik_version = version.string();
      }
      if (!provider_only) m.native_extensions.push_back(n);
    }
    if (a.object.count("compatibility")) {
      const auto &c = a.at("compatibility");
      m.has_bytecode_delivery = c.object.count("bytecode_versions") && !c.at("bytecode_versions").array.empty();
      if (m.has_bytecode_delivery) {
        if (!c.object.count("bytecode_languages") || c.at("bytecode_languages").array.empty()) m.compatibility_error = "LANGUAGE_REQUIREMENT_MISSING: bytecode producer version is absent";
        else for (const auto &v : c.at("bytecode_languages").array)
          if (!language::compatible_producer(v.string())) m.compatibility_error = "LANGUAGE_VERSION_MISMATCH: bytecode producer is Sputnik " + v.string();
        for (const auto &v : c.at("bytecode_versions").array)
          if (v.array.size() != 2 || size_field(v.array[0]) != 1 || size_field(v.array[1]) > 2) m.compatibility_error = "BYTECODE_VERSION_MISMATCH: unsupported bytecode format";
      }
      if (c.object.count("native_providers")) for (const auto &item : c.at("native_providers").array) {
        PackageNativeProvider p;
        string(item,"id",p.id); string(item,"kind",p.kind); string(item,"version",p.version);
        string(item,"abi",p.abi); string(item,"build_id",p.build_id); string(item,"runtime_build_id",p.runtime_build_id);
        m.native_providers.push_back(p);
      }
    }
    return m;
}

int registry_query_command(int argc, char **argv) {
  const std::string command = argv[2];
  std::string origin = default_registry, reference, version;
  fs::path credentials;
  bool json = false, check_host = false;
  if (const auto *value = std::getenv("SPUTNIK_PKG_REGISTRY")) if (*value) origin = value;
  if (const auto *value = std::getenv("SPUTNIK_PKG_CREDENTIALS")) if (*value) credentials = value;
  std::set<std::string> options;
  for (int i = 3; i < argc; ++i) {
    const std::string option = argv[i];
    if (option == "--json" || option == "--check-host" || option == "--registry" || option == "--credentials") {
      if (!options.insert(option).second) fail("duplicate option: " + option);
      if (option == "--json") { json = true; continue; }
      if (option == "--check-host") { check_host = true; continue; }
      if (++i >= argc || !*argv[i] || std::string(argv[i]).rfind("--", 0) == 0)
        fail("missing value for " + option);
      if (option == "--registry") origin = argv[i];
      else credentials = argv[i];
    } else if (option.rfind('-', 0) != 0 && reference.empty()) reference = option;
    else fail("unknown pkg " + command + " argument: " + option);
  }
  if (check_host && (command != "variants" || !json)) fail("--check-host requires pkg variants --json");
  if (command == "variants") {
    const auto at = reference.find('@');
    if (at == std::string::npos) fail("usage: sputnik pkg variants <package>@<version>");
    version = reference.substr(at + 1);
    reference.resize(at);
    if (!release_version(version)) fail("variants requires an exact version: <package>@<version>");
  }
  if (!registry_reference(reference))
    fail(command == "versions" ? "usage: sputnik pkg versions <package>" : "invalid package reference");
  if (credentials.empty()) {
    const auto saved = registry_credentials(origin);
    if (fs::exists(saved)) credentials = saved;
  }
  API api(origin, credentials);
  // Nested read endpoints resolve root aliases and return canonical identity.
  // Avoid a separate package GET (and TLS handshake) just to resolve the name.
  auto result = command == "versions" ? complete_release_index(api, reference) :
      api.document(base(reference) + "/versions/" + version);
  const auto canonical = canonical_reference(reference, result);
  if (command == "variants") {
    validate_release(result, canonical, version);
    auto &artifacts = result.object.at("artifacts");
    if (artifacts.kind != Value::Array) fail("invalid artifact list");
    std::set<std::string> variants;
    for (const auto &artifact : artifacts.array) {
      const auto variant = artifact.at("variant").string();
      if (variant.empty() || variant.size() > 128 || variant.find("..") != std::string::npos ||
          variant.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_.-") != std::string::npos ||
          std::string("abcdefghijklmnopqrstuvwxyz0123456789").find(variant.front()) == std::string::npos ||
          !variants.insert(variant).second)
        fail("invalid or duplicate artifact variant");
      (void)boolean_field(artifact.at("quarantined"));
    }
    artifacts.array.erase(std::remove_if(artifacts.array.begin(), artifacts.array.end(),
        [](const Value &artifact) { return artifact.at("quarantined").truth(); }), artifacts.array.end());
    std::sort(artifacts.array.begin(), artifacts.array.end(), [](const Value &a, const Value &b) {
      return a.at("variant").text < b.at("variant").text;
    });
  }
  if (check_host) {
    // Use the installer's target/ABI/hardware gates. Consent to native code,
    // source compilation and size is collected separately by the caller.
    const auto host = probe_package_host(true);
    const auto string_value = [](const std::string &s) { Value v; v.kind = Value::String; v.text = s; return v; };
    const auto bool_value = [](bool b) { Value v; v.kind = Value::Boolean; v.text = b ? "true" : "false"; return v; };
    Value host_value; host_value.kind = Value::Object;
    host_value.object = {{"os", string_value(host.os)}, {"arch", string_value(host.arch)}};
    result.object["host"] = host_value;
    std::vector<PackageManifest> best;
    int best_rank = -10000;
    for (auto &artifact : result.object.at("artifacts").array) {
      auto manifest = registry_artifact_manifest(artifact);
      VariantPolicy policy;
      policy.variant = manifest.variant; policy.bundle = manifest.bundle;
      policy.allow_native = true;
      policy.build_from_source = std::any_of(manifest.native_extensions.begin(), manifest.native_extensions.end(),
          [](const auto &n) { return n.kind == "source"; });
      const auto supports = [&](const std::string &b) {
        return manifest.target.compute_backend == b ||
            std::find(manifest.target.compute_backends.begin(), manifest.target.compute_backends.end(), b) != manifest.target.compute_backends.end();
      };
      policy.backend = manifest.target.compute_backend == "none" ? "cpu" : manifest.target.compute_backend;
      auto reason = package_incompatibility(manifest, host, policy);
      for (const auto *backend : {"cuda", "rocm", "mps", "cpu"}) {
        if (!supports(backend)) continue;
        auto candidate = policy; candidate.backend = backend;
        const auto problem = package_incompatibility(manifest, host, candidate);
        if (problem.empty()) { policy = candidate; reason.clear(); break; }
      }
      Value selection; selection.kind = Value::Object;
      selection.object = {{"compatible", bool_value(reason.empty())}, {"reason", string_value(reason)},
          {"backend", string_value(policy.backend)}, {"bundle", string_value(policy.bundle)},
          {"allow_native", bool_value(package_has_native(manifest))},
          {"build_from_source", bool_value(policy.build_from_source)}};
      artifact.object["host_selection"] = selection;
      if (!reason.empty() || result.at("yanked").truth()) continue;
      const int rank = (policy.build_from_source ? -1000 : 0) +
          (package_gpu_backend(policy.backend) || policy.backend == "mps" ? 100 : 0) +
          (policy.bundle == "none" ? 20 : policy.bundle == "runtime" ? 10 : 0);
      if (rank > best_rank) { best.clear(); best_rank = rank; }
      if (rank == best_rank) best.push_back(manifest);
    }
    Value recommended;
    if (!best.empty()) {
      VariantPolicy policy; policy.bundle = "auto"; policy.allow_native = true;
      const auto &first = result.at("artifacts").array;
      const auto selected = std::find_if(first.begin(), first.end(), [&](const auto &a) { return a.at("variant").text == best.front().variant; });
      policy.backend = selected->at("host_selection").at("backend").text;
      policy.build_from_source = std::any_of(best.front().native_extensions.begin(), best.front().native_extensions.end(),
          [](const auto &n) { return n.kind == "source"; });
      // Equivalent builds have no unique SDK selector. The UI pins an explicit
      // variant, so a stable lexical choice is safe and remains user-editable.
      int chosen = 0;
      try { const int selected = select_package_variant(best, host, policy); if (selected >= 0) chosen = selected; }
      catch (const std::runtime_error &error) {
        if (std::string(error.what()).find("ARTIFACT_SELECTION_AMBIGUOUS") == std::string::npos) throw;
      }
      recommended = string_value(best[chosen].variant);
    }
    result.object["recommended_variant"] = recommended;
  }
  if (json) { write_json(std::cout, result); std::cout << '\n'; }
  else if (command == "versions") {
    for (const auto &release : result.at("versions").array)
      std::cout << release.at("version").text << (release.at("yanked").truth() ? " (yanked)" : "") << '\n';
  } else {
    if (result.at("yanked").truth()) std::cerr << canonical << '@' << version << " is yanked\n";
    for (const auto &artifact : result.at("artifacts").array)
      std::cout << artifact.at("variant").text << '\n';
  }
  return 0;
}
} // namespace

bool registry_reference(const std::string &name) {
  auto slash = name.find('/');
  return slash == std::string::npos ? part(name) :
      name.find('/', slash + 1) == std::string::npos && part(name.substr(0, slash), true) && part(name.substr(slash + 1));
}
bool remote_registry(const std::string &registry) { return registry.find("://") != std::string::npos; }

fs::path registry_credentials(const std::string &origin) {
  if (const auto *value = std::getenv("SPUTNIK_PKG_CREDENTIALS"))
    if (*value) return value;
  const auto *home = std::getenv("HOME");
  if (!home || !*home) fail("HOME is unavailable; pass --credentials");
  return fs::path(home) / ".sputnik" / "credentials" / (lexer::sha256_hex(origin) + ".json");
}

int registry_command(int argc, char **argv) {
  const std::string command = argv[2];
  if (command == "versions" || command == "variants") return registry_query_command(argc, argv);
  std::string origin = default_registry, name_space, artifact;
  fs::path credentials, session;
  bool large = false;
  if (const auto *value = std::getenv("SPUTNIK_PKG_REGISTRY")) if (*value) origin = value;
  for (int i = 3; i < argc; ++i) {
    const std::string option = argv[i];
    if (option == "--allow-native-large") { large = true; continue; }
    if (option == "--registry" || option == "--credentials" || option == "--namespace" || option == "--session") {
      if (++i >= argc) fail("missing value for " + option);
      if (option == "--registry") origin = argv[i];
      else if (option == "--credentials") credentials = argv[i];
      else if (option == "--namespace") name_space = argv[i];
      else session = argv[i];
    } else if (command == "publish" && artifact.empty() && option.rfind("--", 0)) artifact = option;
    else fail("unknown pkg " + command + " option: " + option);
  }
  if (credentials.empty()) credentials = registry_credentials(origin);
  if (command == "login") {
    API api(origin, {});
    const auto caps = api.document("/api/v1/capabilities");
    bool github = false;
    for (const auto &provider : caps.at("login_providers").array) if (provider.string() == "github") github = true;
    if (!github) fail("GitHub login is not enabled on " + origin);
    std::string expected;
    if (fs::exists(credentials)) {
      API existing(origin, credentials);
      auto saved = registry_json::Parser(read_private(credentials)).parse();
      if (saved.object.count("account_id")) expected = saved.at("account_id").string();
      else expected = existing.document("/api/v1/me").at("account").at("id").string();
    }
    auto attempt = api.post("/api/v1/auth/device", "{\"provider\":\"github\"" +
        (expected.empty() ? "" : ",\"expected_account_id\":" + quote(expected)) + "}");
    auto id = attempt.at("attempt_id").string();
    auto secret = attempt.at("login_secret").string();
    if (id.size() != 32 || id.find_first_not_of("0123456789abcdef") != std::string::npos || secret.rfind("splog_", 0)) fail("invalid login attempt");
    api.token(secret);
    auto path = "/api/v1/auth/device/" + id;
    bool saved = false;
    try {
      const auto uri = attempt.at("verification_uri").string();
      if (uri != "https://github.com/login/device" && origin.rfind("http://127.0.0.1:", 0)) fail("unexpected GitHub verification URI");
      std::cerr << "Registry: " << origin << "\nOpen " << uri << " and enter code: "
                << attempt.at("user_code").string() << "\nWaiting for GitHub authorization...\n";
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(16);
      Value verified;
      for (;;) {
        if (std::chrono::steady_clock::now() >= deadline) fail("GitHub authorization timed out");
        verified = api.post(path + "/poll", "{}");
        auto state = verified.at("state").string();
        if (state == "verified") break;
        if (state != "pending") {
          std::string reason = state;
          if (verified.object.count("error")) reason = verified.at("error").at("code").string();
          fail("GitHub login failed: " + reason);
        }
        auto wait = size_field(verified.at("retry_after"));
        if (!wait || wait > 300) fail("invalid login polling interval");
        std::this_thread::sleep_for(std::chrono::seconds(wait));
      }
      const auto identity = verified.at("identity");
      std::cerr << "GitHub: " << identity.at("login").string() << " (ID " << identity.at("subject").string() << ")\n";
      bool interactive = name_space.empty() && !verified.object.count("namespace");
      if (verified.object.count("namespace")) {
        auto existing = verified.at("namespace").string();
        if (!name_space.empty() && name_space != existing) fail("existing account keeps namespace " + existing);
        name_space = existing;
      }
      for (;;) {
        if (name_space.empty()) {
          std::cerr << "Choose personal namespace (lowercase letters, digits, underscore): ";
          if (!std::getline(std::cin, name_space) || name_space.empty()) fail("namespace required; retry with --namespace NAME");
        }
        std::cerr << "Token permissions: read, manage own tokens, create/publish packages in " << name_space << "\n";
        Value completed;
        try {
          completed = api.post(path + "/complete", "{\"namespace\":" + quote(name_space) + ",\"token\":{\"label\":\"Sputnik CLI\"}}", random_id());
        } catch (const std::exception &error) {
          if (!interactive || std::string(error.what()).find("NAMESPACE_UNAVAILABLE") == std::string::npos) throw;
          std::cerr << "Namespace unavailable. Choose another.\n"; name_space.clear(); continue;
        }
        auto token = completed.at("token").string();
        if (token.rfind("spreg_", 0)) fail("invalid registry token");
        save_private(credentials, "{\"origin\":" + quote(origin) + ",\"token\":" + quote(token) +
            ",\"account_id\":" + quote(completed.at("account_id").string()) + ",\"namespace\":" + quote(completed.at("namespace").string()) +
            ",\"token_id\":" + quote(completed.at("id").string()) + ",\"expires_at\":" + quote(completed.at("expires_at").string()) + "}\n");
        saved = true;
        std::cout << "Logged in to " << origin << " as " << identity.at("login").string() << "; namespace " << name_space << "\n";
        break;
      }
    } catch (...) {
      if (!saved) {
        try { api.text("DELETE", path, "{}"); }
        catch (...) { std::cerr << "Login cancellation could not be confirmed; any issued token may need revocation.\n"; }
      }
      throw;
    }
    return 0;
  }
  API api(origin, credentials);
  if (command == "whoami") {
    std::cout << api.text("GET", "/api/v1/me") << '\n'; return 0;
  }
  if (command == "logout") {
    auto token = api.document("/api/v1/me").at("token_id").string();
    if (token.size() != 32 || token.find_first_not_of("0123456789abcdef") != std::string::npos) fail("invalid token ID");
    api.text("DELETE", "/api/v1/tokens/" + token, "{}", random_id());
    fs::remove(credentials);
    std::cout << "Logged out of " << origin << '\n'; return 0;
  }
  if (command != "publish") fail("unknown registry command");
  if (artifact.empty()) fail("usage: sputnik pkg publish FILE.sputnikpkg");
  const auto info = inspect_distribution(artifact);
  if (!registry_reference(info.manifest.name) || !release_version(info.manifest.version)) fail("invalid artifact identity");
  if (info.compressed_size > (large ? 2147483648ULL : 67108864ULL)) fail("artifact exceeds enabled upload profile");
  const auto profile = info.manifest.limit_profile;
  if (profile == "native-large" && !large) fail("native-large publication requires --allow-native-large");
  auto identity = origin + "\n" + info.manifest.name + "\n" + info.manifest.version + "\n" + info.manifest.variant + "\n" + info.sha256;
  if (session.empty()) session = fs::path(artifact + ".publish-" + lexer::sha256_hex(origin).substr(0, 12) + ".json");
  std::string sid, create_key, commit_key;
  if (fs::exists(session)) {
    auto doc = registry_json::Parser(read_private(session)).parse();
    if (doc.at("identity").string() != identity) fail("publish session belongs to another artifact or origin");
    create_key = doc.at("create_key").string(); commit_key = doc.at("commit_key").string();
    if (doc.object.count("id")) sid = doc.at("id").string();
  } else { create_key = random_id(); commit_key = random_id(); }
  const auto save = [&]() {
    save_private(session, "{\"identity\":" + quote(identity) + ",\"create_key\":" + quote(create_key) + ",\"commit_key\":" + quote(commit_key) +
        (sid.empty() ? "" : ",\"id\":" + quote(sid)) + "}\n");
  };
  save();
  if (sid.empty()) {
    auto result = api.post("/api/v1/publish-sessions", "{\"package_ref\":" + quote(info.manifest.name) + ",\"version\":" + quote(info.manifest.version) +
        ",\"variant\":" + quote(info.manifest.variant) + ",\"expected_size\":" + std::to_string(info.compressed_size) + ",\"expected_sha256\":" + quote("sha256:" + info.sha256) + ",\"limit_profile\":" + quote(profile) + "}", create_key);
    sid = result.at("id").string(); save();
  }
  if (sid.size() != 32 || sid.find_first_not_of("0123456789abcdef") != std::string::npos) fail("invalid publish session ID");
  const auto path = "/api/v1/publish-sessions/" + sid;
  auto state = api.document(path).at("state").string();
  if (state == "created") { api.upload(path + "/artifact", artifact, info.compressed_size); state = "uploaded"; }
  if (state == "uploaded") state = api.post(path + "/commit", "{}", commit_key).at("state").string();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(20);
  while (state == "validating" || state == "promoting") {
    if (std::chrono::steady_clock::now() >= deadline) fail("publication still running; retry the same publish command");
    std::this_thread::sleep_for(std::chrono::seconds(1));
    state = api.document(path).at("state").string();
  }
  if (state != "published") fail("publication ended in state " + state + "; inspect session " + sid);
  std::cout << "Published " << info.manifest.name << '@' << info.manifest.version << " (" << info.manifest.variant << ") to " << origin << '\n';
  return 0;
}

RegistryDownload download_registry_package(const std::string &origin,
    const std::string &name, const std::string &requested,
    const fs::path &destination, const fs::path &credentials,
    const VariantPolicy &policy, const std::string &pinned_digest,
    bool allow_native_large) {
  API api(origin, credentials);
  const auto canonical = canonical_package(api, name);
  auto location = base(canonical);
  const auto host = probe_package_host(policy.backend == "auto" || package_gpu_backend(policy.backend) || !policy.variant.empty());
  const auto select = [&](const Value &release) {
    if (release.at("artifacts").kind != Value::Array) fail("invalid artifact list");
    std::vector<PackageManifest> variants;
    for (const auto &a : release.at("artifacts").array) if (!a.at("quarantined").truth()) variants.push_back(registry_artifact_manifest(a));
    const int i = select_package_variant(variants,host,policy);
    return i < 0 ? std::string{} : variants[i].variant;
  };
  std::string chosen = requested, variant;
  if (!chosen.empty() && !release_version(chosen)) fail("an exact release version is required");
  if (chosen.empty()) {
    std::set<std::string> cursors;
    std::string path = location + "/index";
    std::vector<std::string> versions;
    do {
      auto index = api.document(path);
      if (index.at("package_ref").string() != canonical || index.at("versions").kind != Value::Array) fail("invalid release index");
      for (const auto &release : index.at("versions").array) {
        auto version = release.at("version").string();
        if (!release_version(version)) fail("invalid release version");
        if (release.at("yanked").truth()) continue;
        versions.push_back(version);
      }
      auto cursor = index.at("next_cursor");
      if (cursor.kind == Value::Null) break;
      auto next = cursor.string();
      if (!cursors.insert(next).second || cursors.size() > 1000 ||
          next.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=._-") != std::string::npos)
        fail("invalid pagination cursor");
      // Cursor is base64, so escape its reserved query characters.
      std::string escaped;
      for (char c : next) { if (c == '+') escaped += "%2B"; else if (c == '/') escaped += "%2F"; else if (c == '=') escaped += "%3D"; else escaped += c; }
      path = location + "/index?cursor=" + escaped;
    } while (true);
    std::sort(versions.begin(),versions.end(),version_less);
    for (bool stable : {true,false}) {
      for (auto it = versions.rbegin(); it != versions.rend(); ++it) {
        if ((it->find('-') == std::string::npos) != stable) continue;
        auto release = api.document(location + "/versions/" + *it);
        if (release.at("package_ref").string() != canonical || release.at("version").string() != *it) fail("invalid release identity");
        if (release.at("yanked").truth()) continue;
        variant = select(release);
        if (!variant.empty()) { chosen = *it; break; }
      }
      if (!chosen.empty()) break;
    }
    if (chosen.empty()) fail("NO_COMPATIBLE_ARTIFACT: " + name);
  }
  auto release = api.document(location + "/versions/" + chosen);
  if (release.at("package_ref").string() != canonical || release.at("version").string() != chosen || release.at("yanked").truth())
    fail("release is yanked or has a different identity");
  variant = select(release);
  if (variant.empty()) fail("NO_COMPATIBLE_ARTIFACT: " + canonical + "@" + chosen + "; check --backend, --bundle, --allow-native and --variant");
  auto path = location + "/versions/" + chosen + "/artifacts/" + variant;
  auto meta = api.document(path);
  if (meta.at("package_ref").string() != canonical || meta.at("version").string() != chosen ||
      meta.at("variant").string() != variant || meta.at("url").string() != origin + path + "/download")
    fail("artifact metadata identity or download origin differs");
  auto size = size_field(meta.at("size")); auto digest = meta.at("sha256").string();
  if (!pinned_digest.empty() && digest != pinned_digest) fail("artifact differs from sputnik.lock");
  if (!size || size > (allow_native_large ? 2147483648ULL : 67108864ULL) || digest.size() != 71 || digest.rfind("sha256:", 0) ||
      digest.substr(7).find_first_not_of("0123456789abcdef") != std::string::npos) fail("invalid artifact size/digest");
  auto response = api.get(path + "/download");
  if (response.headers.first("content-length") != std::optional<std::string>(std::to_string(size)) ||
      response.headers.first("x-sputnik-sha256") != std::optional<std::string>(digest)) fail("download framing/digest differs");
  fs::create_directories(destination);
  auto output = destination / (digest.substr(7) + ".sputnikpkg");
  try {
    std::ofstream file(output, std::ios::binary | std::ios::trunc);
    if (!file) fail("cannot create package download");
    std::uint64_t total = 0; std::string bytes, error; http::HttpErrorKind kind;
    while (!response.body->consumed()) {
      if (!response.body->read(65536, &bytes, &kind, &error)) fail(error);
      total += bytes.size(); if (total > size) fail("download exceeds declared size");
      file.write(bytes.data(), bytes.size());
    }
    file.close();
    if (!file || total != size || "sha256:" + file_sha256(output) != digest) fail("download checksum/size differs");
    auto archive = inspect_distribution(output);
    if (archive.manifest.name != canonical || archive.manifest.version != chosen || archive.manifest.variant != variant)
      fail("archive identity differs from registry metadata");
    if (registry_artifact_manifest(meta).sputnik_requirement != archive.manifest.sputnik_requirement) fail("archive language requirement differs from registry metadata");
    const auto problem = package_incompatibility(archive.manifest,host,policy);
    if (!problem.empty()) fail("NO_COMPATIBLE_ARTIFACT: " + problem);
    PackageManifest expected_target, actual_target;
    expected_target.target = registry_artifact_manifest(meta).target; actual_target.target = archive.manifest.target;
    if (render_distribution_manifest(expected_target) != render_distribution_manifest(actual_target)) fail("archive target differs from registry metadata");
  } catch (...) { fs::remove(output); throw; }
  return {output, canonical, chosen};
}
} // namespace sputnik::pkg
