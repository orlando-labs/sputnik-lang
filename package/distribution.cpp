#include "package/distribution.h"
#include "package/selection.h"
#include "package/store.h"

#include "bytecode/format.h"
#include "frontend/lexer/lexer.h"
#include "frontend/lexer/token.h"
#include "frontend/parser/parser.h"

#include <archive.h>
#include <archive_entry.h>
#include <openssl/evp.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <sys/file.h>
#include <unistd.h>

namespace sputnik::pkg {
namespace {
namespace fs = std::filesystem;
constexpr std::uint64_t MiB = 1024U * 1024U;
constexpr std::uint64_t GiB = 1024U * MiB;
constexpr std::size_t MaxMembers = 10000;
struct Limits {
  std::uint64_t compressed, expanded, member;
};
Limits limits(const std::string &profile) {
  if (profile == "standard")
    return {64 * MiB, 256 * MiB, 64 * MiB};
  if (profile == "native-large")
    return {2 * GiB, 8 * GiB, 4 * GiB};
  throw std::runtime_error("unknown package limit profile: " + profile);
}
[[noreturn]] void fail(const std::string &message) {
  throw std::runtime_error("package: " + message);
}
std::string quote(const std::string &value) {
  std::ostringstream out;
  out << '"';
  for (unsigned char c : value) {
    if (c == '"' || c == '\\')
      out << '\\' << c;
    else if (c == '\n')
      out << "\\n";
    else if (c == '\r')
      out << "\\r";
    else if (c == '\t')
      out << "\\t";
    else if (c < 32)
      fail("control character in metadata");
    else
      out << c;
  }
  out << '"';
  return out.str();
}

void path_check(const std::string &path) {
  if (path.empty() || path.size() > 1024 || path.front() == '/')
    fail("invalid member path: " + path);
  for (unsigned char c : path)
    if (c < 32 || c >= 127 || c == '\\' || c == ':')
      fail("nonportable member path: " + path);
  std::istringstream parts(path);
  std::string part;
  while (std::getline(parts, part, '/'))
    if (part.empty() || part == "." || part == "..")
      fail("unsafe member path: " + path);
  if (path.back() == '/')
    fail("directory member is not a regular file: " + path);
  if (path == "modules.index" || path == "manifest.json" ||
      path == "origin.json" || path == "sputnik.lock" ||
      path == "native-library.meta" || path.rfind(".native", 0) == 0 ||
      path.rfind(".compiled", 0) == 0 || path.rfind(".git", 0) == 0 ||
      fs::path(path).extension() == ".sputnikpkg")
    fail("reserved member path: " + path);
}
std::string fold(std::string text) {
  for (char &c : text)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}
void component(const std::string &value, std::size_t maximum = 128) {
  if (value.empty() || value.size() > maximum ||
      value.find("..") != std::string::npos ||
      !std::isalnum(static_cast<unsigned char>(value.front())))
    fail("invalid identity component: " + value);
  for (unsigned char c : value)
    if (!(std::isalnum(c) || c == '.' || c == '_' || c == '-'))
      fail("invalid identity component: " + value);
}
void identity(const PackageManifest &m) {
  const auto slash = m.name.find('/');
  if (slash == std::string::npos)
    component(m.name, 64);
  else {
    component(m.name.substr(0, slash), 63);
    component(m.name.substr(slash + 1), 64);
    if (m.name.find('/', slash + 1) != std::string::npos)
      fail("invalid scoped package reference");
    for (const auto &module : m.modules)
      if (module.name.rfind(m.name.substr(0, slash) + ".", 0) != 0)
        fail("scoped module lacks publisher prefix: " + module.name);
  }
  if (m.kind != "library" && m.kind != "native")
    fail("unknown package kind");
  if (m.kind == "native" &&
      (!m.modules.empty() || !m.root_module.empty() ||
       !m.native_extensions.empty() || m.native_providers.empty()))
    fail("native provider package must have providers and no "
         "modules/root/extensions");
  if (m.bundle != "none" && m.bundle != "runtime" && m.bundle != "sdk")
    fail("unknown bundle profile");
  if (m.kind == "library" && m.bundle == "sdk" &&
      std::none_of(m.native_extensions.begin(), m.native_extensions.end(),
                   [](const auto &n) { return n.kind == "source"; }))
    fail("SDK bundle requires native source");
  std::set<std::string> requirements, providers;
  for (const auto &p : m.native_providers) {
    component(p.id);
    if (!providers.insert(p.id).second || p.version.empty() ||
        p.build_id.size() != 71 || p.build_id.rfind("sha256:", 0) != 0 ||
        (p.kind != "runtime" && p.kind != "sdk"))
      fail("invalid native provider");
    for (const auto &f : p.files)
      path_check(f);
    for (const auto &l : p.libraries)
      path_check(l.path);
    for (const auto *dirs : {&p.include_dirs, &p.library_dirs})
      for (const auto &d : *dirs)
        path_check(d);
    if (p.kind == "sdk" && p.runtime_build_id.empty())
      fail("SDK needs runtime build identity");
  }
  for (const auto &r : m.native_requirements) {
    component(r.id);
    if (!requirements.insert(r.id).second || r.version.empty() ||
        (r.kind != "runtime" && r.kind != "sdk") ||
        (r.provider != "external" && r.provider != "bundled" &&
         r.provider != "package"))
      fail("invalid native requirement");
    if (r.provider == "bundled" && !providers.count(r.id))
      fail("missing bundled provider: " + r.id);
    if (r.provider == "package" && std::none_of(m.artifact_dependencies.begin(),m.artifact_dependencies.end(),[&](const auto &d) { return d.name == r.package; }) &&
        std::none_of(m.dependencies.begin(),m.dependencies.end(),[&](const auto &d) { return d.name == r.package; }))
      fail("package native provider requires an exact package dependency: " + r.id);
    for (const auto &l : r.libraries) {
      path_check(l);
      if (fs::path(l).filename() != l)
        fail("requirement library must be a basename");
    }
  }
  for (const auto &n : m.native_extensions)
    for (const auto &id : n.requirements)
      if (!requirements.count(id))
        fail("unknown native requirement: " + id);
  for (const auto &r : m.native_requirements)
    if (r.kind == "sdk" && !requirements.count(r.runtime))
      fail("SDK needs runtime requirement");
  component(m.version, 64);

  component(m.variant);
  validate_package_target(m.target);
  limits(m.limit_profile);
  std::set<std::string> module_paths;
  for (const auto &module : m.modules) {
    if (!module_paths.insert(module.path).second)
      fail("multiple modules share one member path");
    bool first = true;
    for (unsigned char c : module.name) {
      if (c == '.') {
        if (first)
          fail("invalid module name");
        first = true;
      } else {
        if (!(std::isalpha(c) || c == '_' || (!first && std::isdigit(c))))
          fail("invalid module name: " + module.name);
        first = false;
      }
    }
    if (first)
      fail("invalid module name");
    const auto suffix = fs::path(module.path).extension();
    if ((module.representation == "source" && suffix != ".s" &&
         suffix != ".spu" && suffix != ".sputnik") ||
        (module.representation == "bytecode" && suffix != ".sputnikbc") ||
        (module.representation != "source" &&
         module.representation != "bytecode"))
      fail("module representation/path mismatch: " + module.name);
  }
  if (m.target.os != "any" && m.target.os != "macos" &&
      m.target.os != "linux" && m.target.os != "windows")
    fail("unsupported target OS");
  if (m.target.arch != "any" && m.target.arch != "aarch64" &&
      m.target.arch != "x86_64")
    fail("unsupported target architecture");
}
struct Hash {
  EVP_MD_CTX *context = EVP_MD_CTX_new();
  Hash() {
    if (!context || EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1)
      fail("cannot initialize SHA-256");
  }
  ~Hash() { EVP_MD_CTX_free(context); }
  void add(const void *bytes, std::size_t size) {
    if (EVP_DigestUpdate(context, bytes, size) != 1)
      fail("SHA-256 update failed");
  }
  std::string finish() {
    unsigned char bytes[EVP_MAX_MD_SIZE];
    unsigned size = 0;
    if (EVP_DigestFinal_ex(context, bytes, &size) != 1)
      fail("SHA-256 finish failed");
    std::ostringstream out;
    for (unsigned i = 0; i < size; ++i)
      out << std::hex << std::setw(2) << std::setfill('0')
          << unsigned(bytes[i]);
    return out.str();
  }
};
std::string read_small(const fs::path &path, std::uint64_t maximum = 64 * MiB) {
  if (fs::file_size(path) > maximum)
    fail("metadata exceeds size limit: " + path.string());
  std::ifstream in(path, std::ios::binary);
  if (!in)
    fail("cannot open: " + path.string());
  return {std::istreambuf_iterator<char>(in), {}};
}
std::string bytecode_facts(const std::string &bytes) {
  const auto decoded = bytecode::deserialize_module(
      std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
  if (!decoded.ok())
    fail("invalid package bytecode");
  const auto &m = decoded.module;
  std::ostringstream abi;
  for (auto b : m.abi_hash)
    abi << std::hex << std::setw(2) << std::setfill('0') << unsigned(b);
  std::ostringstream out;
  out << "{\"format\":[" << m.format_version.major << ','
      << m.format_version.minor << "],\"language\":["
      << m.language_version.major << ',' << m.language_version.minor << ',' << m.language_version.patch
      << "],\"profile_flags\":" << m.profile_flags
      << ",\"abi_hash\":" << quote(abi.str())
      << ",\"sha256\":" << quote(lexer::sha256_hex(bytes)) << '}';
  return out.str();
}
void source_module(const PackageModule &module, const std::string &source) {
  if (source.size() > 64 * MiB)
    fail("source module exceeds size budget");
  auto tokens = lexer::Lexer(source, module.path).lex();
  if (!tokens.ok())
    fail("invalid source encoding/tokens: " + module.path);
  parser::Parser parser(tokens.tokens);
  const auto parsed = parser.parse_module_unit();
  if (!parsed.ok() || (!parsed.module_name.empty() && parsed.module_name != module.name))
    fail("source package declaration/syntax does not match manifest: " +
         module.path + "\n" + lexer::diagnostics_to_json(parsed.diagnostics));
}
using Facts = std::map<std::string, std::string>;
std::string build_metadata(const PackageManifest &m,
                           const std::string &revision, const Facts &bytecode,
                           const Facts &digests) {
  if (revision != "unspecified" &&
      (revision.size() != 64 ||
       !std::all_of(revision.begin(), revision.end(), [](char c) {
         return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
       })))
    fail("invalid toolchain revision");
  std::ostringstream out;
  out << "{\n\"schema\":\"sputnik.package-build.v1\",\n\"toolchain_revision\":"
      << quote(revision) << ",\n\"bytecode\":{";
  bool first = true;
  for (const auto &entry : bytecode) {
    if (!first)
      out << ',';
    first = false;
    out << quote(entry.first) << ':' << entry.second;
  }
  out << "},\n\"native\":[";
  first = true;
  for (const auto &n : m.native_extensions) {
    if (!first)
      out << ',';
    first = false;
    out << "{\"name\":" << quote(n.name) << ",\"kind\":" << quote(n.kind)
        << ",\"sputnik_version\":" << quote(n.sputnik_version)
        << ",\"os\":" << quote(m.target.os)
        << ",\"arch\":" << quote(m.target.arch) << ",\"files\":{";
    std::set<std::string> files(n.sources.begin(), n.sources.end());
    files.insert(n.headers.begin(), n.headers.end());
    if (!n.library.empty())
      files.insert(n.library);
    for (const auto &lib : n.libraries)
      files.insert(lib.path);
    bool file_first = true;
    for (const auto &file : files) {
      if (!digests.count(file))
        fail("missing native build input: " + file);
      if (!file_first)
        out << ',';
      file_first = false;
      out << quote(file) << ':' << quote(digests.at(file));
    }
    std::string symbols;
    auto sorted_symbols = n.symbols;
    std::sort(sorted_symbols.begin(), sorted_symbols.end(), [](const auto &a, const auto &b) { return a.logical < b.logical; });
    for (const auto &symbol : sorted_symbols)
      symbols += symbol.logical + "=" + symbol.symbol + "\n";
    out << "},\"declared_symbols_sha256\":" << quote(lexer::sha256_hex(symbols))
        << '}';
  }
  out << "]\n}\n";
  return out.str();
}

struct Input {
  fs::path file;
  std::string bytes;
  std::uint64_t size = 0;
  std::string digest;
};
using Inputs = std::map<std::string, Input>;
void unique_paths(const std::vector<DistributionMember> &members) {
  std::set<std::string> folded;
  for (const auto &m : members) {
    path_check(m.path);
    if (!folded.insert(fold(m.path)).second)
      fail("duplicate/case-folded member: " + m.path);
  }
  for (const auto &m : members) {
    auto parent = fs::path(m.path).parent_path();
    while (!parent.empty()) {
      if (folded.count(fold(parent.generic_string())))
        fail("member conflicts with parent file: " + m.path);
      parent = parent.parent_path();
    }
  }
}
fs::path contained(const fs::path &root, const std::string &path) {
  path_check(path);
  const auto resolved = fs::canonical(root / path);
  const auto relative = resolved.lexically_relative(root);
  if (relative.empty() || *relative.begin() == "..")
    fail("member escapes root: " + path);
  return resolved;
}
void add_file(Inputs &inputs, const fs::path &root, const std::string &name,
              const Limits &lim) {
  if (name == "sputnik.build.json" || name == "sputnik.build.yaml" || name == "sputnik.build.yml" || name == "checksums.sha256")
    fail("packlist replaces package metadata");
  auto file = contained(root, name);
  if (fs::is_directory(file)) {
    for (const auto &entry : fs::recursive_directory_iterator(file)) {
      if (entry.is_symlink())
        fail("symlink in packlist directory: " + entry.path().string());
      if (entry.is_regular_file())
        add_file(inputs, root,
                 entry.path().lexically_relative(root).generic_string(), lim);
    }
    return;
  }
  if (!fs::is_regular_file(file))
    fail("packlist member is not a regular file: " + name);
  const auto size = fs::file_size(file);
  if (size > lim.member || inputs.size() >= MaxMembers)
    fail("member budget exceeded: " + name);
  auto found = inputs.find(name);
  if (found != inputs.end()) {
    if (found->second.file != file)
      fail("duplicate destination: " + name);
    return;
  }
  inputs[name] = {file, {}, size, file_sha256(file)};
}
void add_bytes(Inputs &inputs, const std::string &path,
               const std::string &bytes) {
  path_check(path);
  if (inputs.count(path))
    fail("duplicate generated member: " + path);
  inputs[path] = {{}, bytes, bytes.size(), lexer::sha256_hex(bytes)};
}
void archive_ok(int status, struct archive *a) {
  if (status != ARCHIVE_OK)
    fail(archive_error_string(a) ? archive_error_string(a)
                                 : "archive codec failed");
}
struct ArchiveDeleter {
  void operator()(struct archive *a) const {
    if (a)
      archive_free(a);
  }
};
using Archive = std::unique_ptr<struct archive, ArchiveDeleter>;
PackageManifest manifest(const std::string &text) {
  if (text.size() > MiB) fail("manifest exceeds 1 MiB");
  auto parsed = parse_manifest(text, "sputnik.build.json");
  if (!parsed.ok()) {
    std::string message;
    for (const auto &d : parsed.diagnostics)
      message += d.message + "\n";
    fail(message);
  }
  if (parsed.manifest.schema != "sputnik.build.v1")
    fail("unsupported distribution manifest schema");
  if (!parsed.manifest.explicit_sputnik_requirement)
    fail("LANGUAGE_REQUIREMENT_MISSING: rebuild the package with requires.sputnik");
  identity(parsed.manifest);
  return parsed.manifest;
}
std::uint32_t u32(const std::string &data, std::size_t offset) {
  if (offset + 4 > data.size())
    fail("truncated native binary header");
  const auto *p = reinterpret_cast<const unsigned char *>(data.data() + offset);
  return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 |
         std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
}
std::uint32_t macos_version(const std::string &text) {
  std::istringstream parts(text);
  std::string part;
  std::uint32_t value = 0;
  unsigned count = 0;
  while (std::getline(parts, part, '.')) {
    if (part.empty() || part.size() > 5 || count == 3 ||
        !std::all_of(part.begin(), part.end(),
                     [](char c) { return c >= '0' && c <= '9'; }))
      fail("invalid minimum macOS version");
    const auto number = std::stoul(part);
    if (number > (count == 0 ? 65535U : 255U))
      fail("invalid minimum macOS version");
    value |= number << (count == 0 ? 16 : count == 1 ? 8 : 0);
    ++count;
  }
  if (!count || text.back() == '.')
    fail("missing/invalid minimum macOS version");
  return value;
}
void native_binary(const PackageManifest &m, const std::string &path,
                   const std::string &header,
                   const std::set<std::string> &paths) {
  if (m.target.os == "any" || m.target.arch == "any")
    fail("prebuilt binary requires concrete target");
  if (header.size() >= 4 && u32(header, 0) == 0xfeedfacfU) {
    if (m.target.os != "macos")
      fail("Mach-O does not match target OS: " + path);
    const auto cpu = u32(header, 4);
    if ((m.target.arch == "aarch64" && cpu != 0x0100000cU) ||
        (m.target.arch == "x86_64" && cpu != 0x01000007U))
      fail("native architecture mismatch: " + path);
    if (u32(header, 12) != 6 && u32(header, 12) != 8)
      fail("native member is not a shared library: " + path);
    const auto commands = u32(header, 16), bytes = u32(header, 20);
    if (commands > 4096 || bytes > MiB || 32ULL + bytes > header.size())
      fail("oversized/truncated Mach-O load commands");
    std::size_t offset = 32;
    const auto declared_minimum = macos_version(m.target.min_os);
    for (unsigned i = 0; i < commands; ++i) {
      const auto cmd = u32(header, offset), size = u32(header, offset + 4);
      if (size < 8 || offset + size > 32ULL + bytes)
        fail("invalid Mach-O load command");
      if ((cmd == 0x32 && size < 24) || (cmd == 0x24 && size < 16))
        fail("truncated Mach-O minimum OS command");
      if (cmd == 0x32 && u32(header, offset + 8) != 1)
        fail("Mach-O platform is not macOS: " + path);
      if ((cmd == 0x32 && u32(header, offset + 12) > declared_minimum) ||
          (cmd == 0x24 && u32(header, offset + 8) > declared_minimum))
        fail("native library requires a newer macOS than its target: " + path);
      if (cmd == 0xc || cmd == 0x80000018U || cmd == 0x8000001fU ||
          cmd == 0x80000023U || cmd == 0x8000001cU) {
        const auto start = u32(header, offset + 8);
        if (start >= size)
          fail("invalid Mach-O string offset");
        const auto terminator = header.find('\0', offset + start);
        if (terminator == std::string::npos || terminator >= offset + size)
          fail("unterminated Mach-O import");
        const auto name =
            header.substr(offset + start, terminator - offset - start);
        if (name.rfind("/usr/lib/", 0) == 0 ||
            name.rfind("/System/Library/", 0) == 0) {
        } else if (name.rfind("@loader_path/", 0) == 0) {
          const auto relative = (fs::path(path).parent_path() / name.substr(13))
                                    .lexically_normal()
                                    .generic_string();
          path_check(relative);
          bool external = false;
          for (const auto &r : m.native_requirements)
            if (r.kind == "runtime" && r.provider != "bundled")
              for (const auto &lib : r.libraries)
                external = external || relative == "lib/vendor/" + lib;
          if (cmd != 0x8000001cU && !paths.count(relative) && !external)

            fail("missing bundled native import: " + relative);
        } else if (name.rfind("@rpath/", 0) == 0 && cmd != 0x8000001cU) {
          const auto base = name.substr(7);
          if (!paths.count("lib/" + base) && !paths.count("lib/vendor/" + base))
            fail("unresolved rpath import: " + name);
        } else
          fail("nonrelocatable native import/rpath: " + name);
      }
      offset += size;
    }
  } else if (header.size() >= 20 && header.compare(0, 4, "\177ELF", 4) == 0) {
    if (m.target.os != "linux" ||
        (m.target.libc != "gnu" && m.target.libc != "musl"))
      fail("ELF requires matching Linux/libc target");
    const unsigned machine = static_cast<unsigned char>(header[18]) |
                             unsigned(static_cast<unsigned char>(header[19]))
                                 << 8;
    if ((m.target.arch == "aarch64" && machine != 183) ||
        (m.target.arch == "x86_64" && machine != 62))
      fail("ELF architecture mismatch");
  } else
    fail("unsupported native binary format: " + path);
}
// Independently drain and validate gzip trailers and the entire tar framing.
// libarchive can report tar EOF before it has consumed the gzip trailer/tail.
std::uint64_t validate_gzip(const fs::path &path, const Limits &lim) {
  std::ifstream file(path, std::ios::binary);
  std::array<unsigned char, 65536> input{}, output{};
  z_stream stream{};
  if (inflateInit2(&stream, 15 + 16) != Z_OK)
    fail("cannot initialize gzip reader");
  struct End {
    z_stream *s;
    ~End() { inflateEnd(s); }
  } end{&stream};
  std::array<unsigned char, 512> block{};
  std::size_t used = 0;
  std::uint64_t expanded = 0, skip = 0, tail = 0;
  unsigned zeros = 0, headers = 0;
  bool done = false, stream_end = false;
  while (true) {
    if (!stream.avail_in) {
      file.read(reinterpret_cast<char *>(input.data()), input.size());
      stream.avail_in = static_cast<uInt>(file.gcount());
      stream.next_in = input.data();
      if (!stream.avail_in)
        break;
    }
    if (stream_end) {
      if (inflateReset2(&stream, 31) != Z_OK)
        fail("gzip reset failed");
      stream_end = false;
    }
    stream.next_out = output.data();
    stream.avail_out = output.size();
    const int status = inflate(&stream, Z_NO_FLUSH);
    if (status != Z_OK && status != Z_STREAM_END)
      fail("invalid or truncated gzip stream");
    const auto produced = output.size() - stream.avail_out;
    if (produced > lim.expanded - expanded)
      fail("expanded archive exceeds budget");
    expanded += produced;
    for (std::size_t i = 0; i < produced; ++i) {
      const auto c = output[i];
      if (done) {
        if (c || ++tail > 10240)
          fail("trailing tar payload/padding exceeds limit");
        continue;
      }
      if (skip) {
        --skip;
        continue;
      }
      block[used++] = c;
      if (used != 512)
        continue;
      used = 0;
      if (std::all_of(block.begin(), block.end(), [](auto b) { return !b; })) {
        if (++zeros == 2)
          done = true;
        continue;
      }
      if (zeros || ++headers > MaxMembers * 2)
        fail("malformed tar framing");
      const auto type = block[156];
      if (type != 0 && type != '0' && type != 'x')
        fail("unsupported tar member type");
      std::uint64_t size = 0;
      for (unsigned k = 124; k < 136; ++k) {
        if (!block[k] || block[k] == ' ')
          continue;
        if (block[k] < '0' || block[k] > '7' || size > lim.expanded / 8)
          fail("invalid tar member size");
        size = size * 8 + block[k] - '0';
      }
      if (size > (type == 'x' ? 65536ULL : lim.member))
        fail("tar member exceeds budget");
      skip = ((size + 511) / 512) * 512;
    }
    stream_end = status == Z_STREAM_END;
  }
  if (!file.eof() || !stream_end || !done || skip || used)
    fail("incomplete gzip/tar archive");
  return expanded;
}
struct ReadResult {
  DistributionInfo info;
  std::map<std::string, std::string> metadata;
};
ReadResult read_archive(const fs::path &file,
                        const fs::path &destination = {}, int input_fd = -1,
                        const std::string &required_profile = {}) {
  Limits lim = limits(required_profile.empty() ? "native-large" : required_profile);
  if (!fs::is_regular_file(file) || fs::file_size(file) > lim.compressed)
    fail("artifact exceeds compressed budget");
  Archive a(archive_read_new());
  archive_ok(archive_read_support_format_tar(a.get()), a.get());
  archive_ok(archive_read_support_filter_gzip(a.get()), a.get());
  if (input_fd >= 0) {
    if (lseek(input_fd, 0, SEEK_SET) < 0) fail("cannot seek archive input");
    archive_ok(archive_read_open_fd(a.get(), input_fd, 65536), a.get());
  } else {
    archive_ok(archive_read_open_filename(a.get(), file.c_str(), 65536), a.get());
  }
  ReadResult result;
  Facts bytecode_metadata, member_digests;
  std::set<std::string> paths;
  std::map<std::string, std::string> native_headers;
  struct archive_entry *entry = nullptr;
  int status;
  std::array<char, 65536> buffer{};
  while ((status = archive_read_next_header(a.get(), &entry)) == ARCHIVE_OK) {
    if (result.info.members.size() >= MaxMembers)
      fail("too many archive members");
    const char *raw = archive_entry_pathname(entry);
    if (!raw)
      fail("missing member path");
    const std::string path = raw;
    path_check(path);
    if (!paths.insert(path).second)
      fail("duplicate member: " + path);
    if (archive_entry_filetype(entry) != AE_IFREG ||
        archive_entry_symlink(entry) || archive_entry_hardlink(entry) ||
        archive_entry_sparse_count(entry))
      fail("archive permits only regular nonsparse files");
    const auto signed_size = archive_entry_size(entry);
    if (signed_size < 0 || std::uint64_t(signed_size) > lim.member)
      fail("member exceeds size budget");
    if (result.info.members.empty() && path != "sputnik.build.json")
      fail("manifest must be the first archive member");
    if (result.info.members.size() == 1 && path != "checksums.sha256")
      fail("inventory must be the second archive member");
    const bool metadata = path == "sputnik.build.json" ||
                          path == "checksums.sha256" ||
                          path == "meta/build.json";
    if (metadata && signed_size > static_cast<std::int64_t>(MiB))
      fail("metadata exceeds 1 MiB");
    const bool bytecode = fs::path(path).extension() == ".sputnikbc";
    const auto source_entry =
        std::find_if(result.info.manifest.modules.begin(),
                     result.info.manifest.modules.end(), [&](const auto &m) {
                       return m.representation == "source" && m.path == path;
                     });
    const bool source = source_entry != result.info.manifest.modules.end();
    if (source && signed_size > static_cast<std::int64_t>(64 * MiB))
      fail("source module exceeds size budget");
    // ELF SONAMEs commonly end in a version (libcublas.so.13). Inspect
    // every declared native library regardless of its filename suffix.
    bool native = fs::path(path).extension() == ".dylib" ||
                  fs::path(path).extension() == ".so" ||
                  fs::path(path).extension() == ".dll";
    for (const auto &extension : result.info.manifest.native_extensions) {
      native = native || extension.library == path;
      for (const auto &library : extension.libraries)
        native = native || library.path == path;
    }
    for (const auto &provider : result.info.manifest.native_providers)
      for (const auto &library : provider.libraries)
        native = native || library.path == path;
    if (bytecode && signed_size > 64 * static_cast<std::int64_t>(MiB))
      fail("bytecode exceeds size budget");
    std::string captured;
    std::ofstream out;
    if (!destination.empty()) {
      const auto target = destination / path;
      fs::create_directories(target.parent_path());
      out.open(target, std::ios::binary | std::ios::trunc);
      if (!out)
        fail("cannot create extracted member");
    }
    Hash hash;
    std::uint64_t size = 0;
    while (true) {
      const auto n = archive_read_data(a.get(), buffer.data(), buffer.size());
      if (n < 0)
        fail(archive_error_string(a.get()) ? archive_error_string(a.get())
                                           : "archive read failed");
      if (!n)
        break;
      if (std::uint64_t(n) > lim.member - size)
        fail("member exceeds byte budget");
      hash.add(buffer.data(), n);
      size += n;
      if (metadata || bytecode || source)
        captured.append(buffer.data(), n);
      else if (native && captured.size() < MiB + 32)
        captured.append(buffer.data(),
                        std::min<std::size_t>(n, MiB + 32 - captured.size()));
      if (out.is_open()) {
        out.write(buffer.data(), n);
        if (!out)
          fail("extracted member write failed");
      }
    }
    if (size != std::uint64_t(signed_size))
      fail("member size mismatch");
    if (size > lim.expanded - result.info.expanded_size)
      fail("total member budget exceeded");
    result.info.expanded_size += size;
    result.info.members.push_back({path, hash.finish(), size});
    member_digests[path] = result.info.members.back().sha256;
    if (metadata)
      result.metadata[path] = captured;
    if (path == "sputnik.build.json") {
      result.info.manifest = manifest(captured);
      if (!required_profile.empty() && result.info.manifest.limit_profile != required_profile)
        fail("archive profile differs from admitted profile");
      lim = limits(result.info.manifest.limit_profile);
      if (fs::file_size(file) > lim.compressed)
        fail("artifact exceeds declared profile");
    }
    if (bytecode) {
      bytecode_metadata[path] = bytecode_facts(captured);
    }
    if (source)
      source_module(*source_entry, captured);
    if (native)
      native_headers[path] = std::move(captured);
  }
  if (status != ARCHIVE_EOF)
    fail(archive_error_string(a.get()) ? archive_error_string(a.get())
                                       : "archive header read failed");
  if (archive_filter_code(a.get(), 0) != ARCHIVE_FILTER_GZIP)
    fail("package must be gzip-compressed tar");
  archive_ok(archive_read_close(a.get()), a.get());
  unique_paths(result.info.members);
  if (result.info.members.size() < 2)
    fail("missing archive metadata");
  std::map<std::string, std::string> inventory;
  std::istringstream lines(result.metadata.at("checksums.sha256"));
  std::string line, previous;
  while (std::getline(lines, line)) {
    if (line.size() < 67 || line.substr(64, 2) != "  ")
      fail("invalid inventory line");
    const auto name = line.substr(66), digest = line.substr(0, 64);
    path_check(name);
    if (name == "checksums.sha256" || (!previous.empty() && name <= previous))
      fail("inventory must be sorted and unique");
    previous = name;
    if (!std::all_of(digest.begin(), digest.end(), [](unsigned char c) {
          return std::isdigit(c) || (c >= 'a' && c <= 'f');
        }))
      fail("invalid inventory SHA-256");
    inventory[name] = digest;
  }
  if (inventory.size() + 1 != result.info.members.size())
    fail("inventory/member set mismatch");
  for (const auto &m : result.info.members)
    if (m.path != "checksums.sha256") {
      if (!inventory.count(m.path) || inventory.at(m.path) != m.sha256)
        fail("member digest mismatch: " + m.path);
    }
  if (result.info.manifest.explicit_files) {
    std::set<std::string> expected(result.info.manifest.files.begin(), result.info.manifest.files.end());
    expected.insert("sputnik.build.json"); expected.insert("checksums.sha256");
    if (expected != paths) fail("archive files differ from manifest inventory");
  }
  for (const auto &m : result.info.manifest.modules) {
    if (!paths.count(m.path))
      fail("missing module member: " + m.path);
    if (m.representation == "source")
      ++result.info.source_modules;
    else
      ++result.info.bytecode_modules;
  }
  for (const auto &ext : result.info.manifest.native_extensions) {
    if (ext.kind == "prebuilt") {
      if (!ext.sources.empty() || ext.library.empty() ||
          !native_headers.count(ext.library))
        fail("invalid prebuilt extension");
      native_binary(result.info.manifest, ext.library,
                    native_headers.at(ext.library), paths);
    } else if (ext.kind == "source") {
      if (ext.sources.empty())
        fail("source extension has no source files");
      for (const auto *names : {&ext.sources, &ext.headers})
        for (const auto &name : *names)
          if (!paths.count(name))
            fail("missing native member: " + name);
    } else
      fail("unknown native extension kind");
    for (const auto &lib : ext.libraries) {
      if (!native_headers.count(lib.path))
        fail("missing bundled native library: " + lib.path);
      native_binary(result.info.manifest, lib.path, native_headers.at(lib.path),
                    paths);
    }
  }
  for (const auto &provider : result.info.manifest.native_providers) {
    for (const auto &name : provider.files)
      if (!paths.count(name))
        fail("missing provider asset: " + name);
    for (const auto &lib : provider.libraries) {
      if (!native_headers.count(lib.path))
        fail("missing provider library: " + lib.path);
      native_binary(result.info.manifest, lib.path, native_headers.at(lib.path),
                    paths);
    }
  }
  if (!bytecode_metadata.empty() ||
      !result.info.manifest.native_extensions.empty()) {

    if (!result.metadata.count("meta/build.json"))
      fail("missing bytecode/native build metadata");
    const auto &text = result.metadata.at("meta/build.json");
    const std::string prefix = "{\n\"schema\":\"sputnik.package-build.v1\","
                               "\n\"toolchain_revision\":\"";
    if (text.rfind(prefix, 0) != 0)
      fail("invalid build metadata schema/framing");
    const auto end = text.find('"', prefix.size());
    if (end == std::string::npos ||
        text != build_metadata(result.info.manifest,
                               text.substr(prefix.size(), end - prefix.size()),
                               bytecode_metadata, member_digests))
      fail("build metadata does not match actual payload");
  }
  if (input_fd >= 0 && lseek(input_fd, 0, SEEK_SET) < 0) fail("cannot seek archive input");
  result.info.expanded_size = validate_gzip(file, lim);
  result.info.compressed_size = fs::file_size(file);
  if (input_fd >= 0 && lseek(input_fd, 0, SEEK_SET) < 0) fail("cannot seek archive input");
  result.info.sha256 = file_sha256(file);
  return result;
}
} // namespace

std::string file_sha256(const fs::path &file) {
  std::ifstream input(file, std::ios::binary);
  if (!input)
    fail("cannot open for hashing: " + file.string());
  Hash hash;
  std::array<char, 65536> buffer{};
  while (input.read(buffer.data(), buffer.size()) || input.gcount())
    hash.add(buffer.data(), input.gcount());
  if (!input.eof())
    fail("file hash read failed");
  return hash.finish();
}

std::string render_distribution_manifest(const PackageManifest &manifest) {
  return render_manifest(manifest);
}

DistributionInfo build_distribution(const PackageManifest &input,
                                    const fs::path &directory,
                                    const fs::path &output,
                                    const DistributionBuildOptions &options) {
  const auto root = fs::canonical(directory);
  PackageManifest m = input;
  if (!language::satisfies(SPUTNIK_VERSION, m.sputnik_requirement))
    fail("LANGUAGE_VERSION_MISMATCH: package requires Sputnik " + m.sputnik_requirement + ", host is " SPUTNIK_VERSION);
  if (!m.explicit_sputnik_requirement) m.sputnik_requirement = language::current_requirement();
  if (m.explicit_files) {
    const std::set<std::string> files(m.files.begin(), m.files.end());
    for (const auto &file : files)
      if (!fs::is_regular_file(contained(root, file))) fail("files must list regular files: " + file);
    const auto declared = [&](const std::string &file) {
      if (!files.count(file)) fail("native input is absent from files: " + file);
    };
    for (const auto &n : m.native_extensions) {
      for (const auto *items : {&n.sources, &n.headers}) for (const auto &file : *items) declared(file);
      if (n.kind == "prebuilt") declared(n.library);
      for (const auto &lib : n.libraries) declared(lib.path);
    }
    for (const auto &provider : m.native_providers) {
      for (const auto &file : provider.files) declared(file);
      for (const auto &lib : provider.libraries) declared(lib.path);
    }
  }
  for (const auto &dependency : m.dependencies)
    if (dependency.source == "path")
      fail("local path dependencies cannot be distributed; replace them with versioned dependencies: " + dependency.name);
  m.schema = "sputnik.build.v1";
  if (!options.variant.empty())
    m.variant = options.variant;
  else if (m.variant == "source")
    m.variant =
        options.module_mode == "source" && !options.bytecode_modules.empty()
            ? "mixed"
            : options.module_mode;
  identity(m);
  const auto lim = limits(m.limit_profile);
  if (options.module_mode != "source" && options.module_mode != "bytecode" &&
      options.module_mode != "mixed")
    fail("module mode must be source, bytecode or mixed");
  Inputs inputs;
  std::set<std::string> chosen;
  for (auto &module : m.modules) {
    const auto original = module;
    path_check(original.path);
    const bool binary = original.representation == "bytecode" ||
                        options.module_mode == "bytecode" ||
                        options.bytecode_modules.count(original.name);
    if (options.bytecode_modules.count(original.name))
      chosen.insert(original.name);
    if (binary && original.representation != "bytecode") {
      if (!fs::is_regular_file(contained(root, original.path)))
        fail("module is not a regular file: " + original.path);
      if (!options.compile)
        fail("bytecode packing needs a compiler callback");
      auto bytes = options.compile(original);
      if (!bytecode::deserialize_module(bytes).ok())
        fail("compiler returned invalid bytecode");
      module.path = "bytecode/" + original.name + ".sputnikbc";
      module.representation = "bytecode";
      add_bytes(inputs, module.path, std::string(bytes.begin(), bytes.end()));
    } else {
      module.representation = binary ? "bytecode" : "source";
      add_file(inputs, root, original.path, lim);
    }
  }
  if (chosen != options.bytecode_modules)
    fail("unknown --bytecode-module selection");
  for (const auto &file : m.files) {
    // Explicit source files selected for bytecode delivery are replaced by their compiled payload.
    const auto original = std::find_if(input.modules.begin(), input.modules.end(), [&](const auto &v) { return v.path == file; });
    if (original != input.modules.end()) {
      const auto selected = std::find_if(m.modules.begin(), m.modules.end(), [&](const auto &v) { return v.name == original->name; });
      if (selected->path != original->path) continue;
    }
    add_file(inputs, root, file, lim);
  }
  for (const auto &n : m.native_extensions) {
    for (const auto *names : {&n.sources, &n.headers})
      for (const auto &file : *names)
        add_file(inputs, root, file, lim);
    if (n.kind == "prebuilt")
      add_file(inputs, root, n.library, lim);
    for (const auto &lib : n.libraries)
      add_file(inputs, root, lib.path, lim);
    for (const auto *dirs :
         {&n.include_dirs, &n.library_dirs, &n.runtime_library_dirs})
      for (const auto &dir : *dirs) {
        if (dir == ".")
          continue;
        path_check(dir);
      }
  }
  for (const auto &p : m.native_providers) {
    for (const auto &f : p.files)
      add_file(inputs, root, f, lim);
    for (const auto &l : p.libraries)
      add_file(inputs, root, l.path, lim);
  }
  // Original source files of bytecode-only modules may not sneak in through a

  // directory packlist.
  for (const auto &original : input.modules) {
    auto selected =
        std::find_if(m.modules.begin(), m.modules.end(),
                     [&](const auto &v) { return v.name == original.name; });
    if (selected->representation == "bytecode" &&
        original.representation != "bytecode" && inputs.count(original.path))
      fail("bytecode module source leaked through packlist: " + original.path);
  }
  Facts bytecode_metadata, member_digests;
  for (const auto &entry : inputs) {
    member_digests[entry.first] = entry.second.digest;
    if (fs::path(entry.first).extension() == ".sputnikbc")
      bytecode_metadata[entry.first] = bytecode_facts(
          entry.second.file.empty() ? entry.second.bytes
                                    : read_small(entry.second.file));
  }
  if (!bytecode_metadata.empty() || !m.native_extensions.empty()) {
    if (inputs.count("meta/build.json"))
      fail("meta/build.json is generated by the packer; use another path for "
           "publisher build notes");
    add_bytes(inputs, "meta/build.json",
              build_metadata(m, options.toolchain_revision, bytecode_metadata,
                             member_digests));
  }
  m.files.clear();
  for (const auto &entry : inputs) m.files.push_back(entry.first);
  const auto manifest_text = render_distribution_manifest(m);
  manifest(manifest_text);
  add_bytes(inputs, "sputnik.build.json", manifest_text);
  std::ostringstream inventory;
  std::uint64_t total = 0;
  std::vector<DistributionMember> members;
  for (const auto &item : inputs) {
    if (item.second.size > lim.member ||
        item.second.size > lim.expanded - total)
      fail("package member budget exceeded");
    total += item.second.size;
    members.push_back({item.first, item.second.digest, item.second.size});
    inventory << item.second.digest << "  " << item.first << '\n';
    if (!item.second.file.empty() &&
        fs::weakly_canonical(output) == item.second.file)
      fail("output overwrites an input member");
  }
  unique_paths(members);
  if (inventory.str().size() > MiB || manifest_text.size() > MiB)
    fail("metadata budget exceeded");
  add_bytes(inputs, "checksums.sha256", inventory.str());
  fs::create_directories(fs::absolute(output).parent_path());
  std::string pattern = fs::absolute(output).string() + ".tmp.XXXXXX";
  std::vector<char> name(pattern.begin(), pattern.end());
  name.push_back('\0');
  const int fd = ::mkstemp(name.data());
  if (fd < 0)
    fail("cannot create output staging file");
  const fs::path temporary(name.data());
  struct Cleanup {
    fs::path p;
    ~Cleanup() {
      std::error_code ec;
      fs::remove(p, ec);
    }
  } cleanup{temporary};
  ::close(fd);
  Archive a(archive_write_new());
  archive_ok(archive_write_set_format_pax_restricted(a.get()), a.get());
  archive_ok(archive_write_add_filter_gzip(a.get()), a.get());
  archive_ok(archive_write_set_options(
                 a.get(), "gzip:compression-level=6,gzip:!timestamp"),
             a.get());
  archive_ok(archive_write_set_bytes_per_block(a.get(), 512), a.get());
  archive_ok(archive_write_open_filename(a.get(), temporary.c_str()), a.get());
  std::vector<std::string> order{"sputnik.build.json", "checksums.sha256"};
  for (const auto &item : inputs)
    if (item.first != "sputnik.build.json" && item.first != "checksums.sha256")
      order.push_back(item.first);
  std::array<char, 65536> buffer{};
  for (const auto &path : order) {
    const auto &source = inputs.at(path);
    std::unique_ptr<struct archive_entry, decltype(&archive_entry_free)> entry(
        archive_entry_new(), archive_entry_free);
    archive_entry_set_pathname(entry.get(), path.c_str());
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_perm(entry.get(), 0644);
    archive_entry_set_uid(entry.get(), 0);
    archive_entry_set_gid(entry.get(), 0);
    archive_entry_set_uname(entry.get(), "");
    archive_entry_set_gname(entry.get(), "");
    archive_entry_set_mtime(entry.get(), 0, 0);
    archive_entry_set_size(entry.get(), source.size);
    archive_ok(archive_write_header(a.get(), entry.get()), a.get());
    Hash hash;
    std::uint64_t written = 0;
    const auto write = [&](const char *data, std::size_t size) {
      hash.add(data, size);
      written += size;
      if (archive_write_data(a.get(), data, size) !=
          static_cast<la_ssize_t>(size))
        fail("archive member write failed");
    };
    if (source.file.empty())
      write(source.bytes.data(), source.bytes.size());
    else {
      std::ifstream in(source.file, std::ios::binary);
      while (in.read(buffer.data(), buffer.size()) || in.gcount())
        write(buffer.data(), in.gcount());
      if (!in.eof())
        fail("input read failed during packing");
    }
    if (written != source.size || hash.finish() != source.digest)
      fail("input changed during package build: " + path);
  }
  archive_ok(archive_write_close(a.get()), a.get());
  auto info = inspect_distribution(temporary);
  fs::rename(temporary, output);
  return info;
}

DistributionInfo inspect_distribution(const fs::path &file) {
  return read_archive(file).info;
}
DistributionInfo inspect_distribution_fd(int fd) {
  return inspect_distribution_fd(fd, {});
}
DistributionInfo inspect_distribution_fd(int fd, const std::string &required_profile) {
  const auto position = lseek(fd, 0, SEEK_CUR);
  if (position < 0) fail("inspection requires a seekable regular file");
  struct RestoreOffset {
    int fd; off_t offset;
    ~RestoreOffset() { (void)lseek(fd, offset, SEEK_SET); }
  } restore{fd, position};
  return read_archive("/dev/fd/" + std::to_string(fd), {}, fd, required_profile).info;
}
DistributionInfo extract_distribution(const fs::path &file,
                                      const fs::path &destination) {
  if (fs::exists(destination) || fs::is_symlink(destination))
    fail("extraction destination must not exist");
  const auto expected = inspect_distribution(file);
  fs::create_directories(destination);
  try {
    auto actual = read_archive(file, fs::canonical(destination)).info;
    if (actual.sha256 != expected.sha256)
      fail("artifact changed during extraction");
    return actual;
  } catch (...) {
    fs::remove_all(destination);
    throw;
  }
}
namespace {
std::string provider_read(const fs::path &file, std::uint64_t cap = MiB) {
  if (!fs::is_regular_file(file) || fs::file_size(file) > cap)
    fail("missing/oversized provider metadata: " + file.string());
  std::ifstream stream(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), {}};
}
fs::path provider_file(const fs::path &root, const std::string &path) {
  path_check(path);
  const auto file = fs::canonical(root / path);
  const auto relative = file.lexically_relative(fs::canonical(root));
  if (relative.empty() || *relative.begin() == ".." ||
      !fs::is_regular_file(file))
    fail("provider member escapes root: " + path);
  return file;
}
Facts provider_inventory(const fs::path &root) {
  Facts inventory;
  std::istringstream in(provider_read(root / "checksums.sha256"));
  std::string line;
  while (std::getline(in, line)) {
    if (line.size() < 67 || line.substr(64, 2) != "  ")
      fail("invalid provider inventory");
    auto path = line.substr(66);
    path_check(path);
    if (!inventory.emplace(path, line.substr(0, 64)).second)
      fail("duplicate provider inventory member");
  }
  if (!inventory.count("sputnik.build.json") ||
      file_sha256(provider_file(root, "sputnik.build.json")) !=
          inventory.at("sputnik.build.json"))
    fail("provider manifest/inventory mismatch");
  return inventory;
}
struct ProviderLock {
  int fd = -1;
  explicit ProviderLock(const fs::path &path) {
    fd = ::open(path.c_str(), O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
    if (fd < 0 || ::flock(fd, LOCK_EX) != 0)
      fail("cannot lock provider cache");
  }
  ~ProviderLock() {
    if (fd >= 0) {
      ::flock(fd, LOCK_UN);
      ::close(fd);
    }
  }
};
struct BoundProvider {
  PackageNativeProvider provider;
  fs::path root;
  std::string fingerprint;
};
BoundProvider bind_provider(const PackageNativeProvider &p,
                            const fs::path &root, const fs::path &cache) {
  const auto inventory = provider_inventory(root);
  std::set<std::string> files(p.files.begin(), p.files.end());
  for (const auto &l : p.libraries)
    files.insert(l.path);
  if (files.empty())
    fail("provider has no assets: " + p.id);
  std::string material = p.id + "\n" + p.kind + "\n" + p.version + "\n" +
                         p.abi + "\n" + p.build_id + "\n" + p.runtime_build_id +
                         "\n";
  for (const auto *exports :
       {&p.include_dirs, &p.library_dirs, &p.link_libraries}) {
    material += "exports\n";
    for (const auto &value : *exports)
      material += value + "\n";
  }
  for (const auto &f : files) {
    if (!inventory.count(f) ||
        file_sha256(provider_file(root, f)) != inventory.at(f))
      fail("provider file/inventory mismatch: " + f);
    material += f + "=" + inventory.at(f) + "\n";
  }
  const auto fingerprint = lexer::sha256_hex(material);
  const auto destination = cache / "providers" / fingerprint;
  fs::create_directories(destination);
  ProviderLock lock(destination / "prepare.lock");
  for (const auto &f : files) {
    const auto out = destination / f;
    if (fs::exists(out)) {
      if (fs::is_symlink(out) || file_sha256(out) != inventory.at(f))
        fail("prepared provider tamper: " + f);
    } else {
      fs::create_directories(out.parent_path());
      const auto temporary = out.string() + ".tmp";
      std::error_code error;
      // Runtime image identity matters to embedded interpreters: their native
      // modules may also refer to the same installed library by absolute path.
      // Retain its inode when the explicit provider is on this filesystem.
      if (p.kind == "runtime") fs::create_hard_link(provider_file(root, f), temporary, error);
      if (p.kind != "runtime" || error)
        fs::copy_file(provider_file(root, f), temporary,
                      fs::copy_options::overwrite_existing);
      fs::rename(temporary, out);
    }
  }
  for (const auto *dirs : {&p.include_dirs, &p.library_dirs})
    for (const auto &dir : *dirs) {
      if (!fs::is_directory(destination / dir))
        fail("provider export has no assets: " + dir);
      for (const auto &entry :
           fs::recursive_directory_iterator(destination / dir))
        if (!entry.is_directory() &&
            (entry.is_symlink() ||
             !files.count(entry.path()
                              .lexically_relative(destination)
                              .generic_string())))
          fail("undeclared prepared provider asset: " + entry.path().string());
    }
  return {p, destination, fingerprint};
}
} // namespace

std::vector<PackageNativeExtension>
prepare_native_providers(const PackageManifest &m, const fs::path &root,
                         const fs::path &cache) {
  if (m.native_requirements.empty())
    return m.native_extensions;
  identity(m);
  std::vector<std::pair<PackageNativeProvider, fs::path>> external;
  if (const char *bindings = std::getenv("SPUTNIK_NATIVE_PROVIDERS")) {
    std::istringstream paths(bindings);
    std::string path;
    while (std::getline(paths, path, ':')) {
      if (path.empty())
        fail("empty native provider binding");
      const auto base = fs::canonical(path);
      const auto descriptor = manifest(provider_read(base / "sputnik.build.json"));
      if (descriptor.kind != "native")
        fail("external binding needs a native provider descriptor");
      // Reuse the static archive target contract on the selected host.
#if defined(__APPLE__)
      if (descriptor.target.os != "macos")
        fail("provider target OS mismatch");
#endif
#if defined(__aarch64__) || defined(__arm64__)
      if (descriptor.target.arch != "aarch64")
        fail("provider target architecture mismatch");
#elif defined(__x86_64__)
      if (descriptor.target.arch != "x86_64")
        fail("provider target architecture mismatch");
#endif
      for (const auto &p : descriptor.native_providers)
        external.emplace_back(p, base);
    }
  }
  std::map<std::string, BoundProvider> bound;
  for (const auto &r : m.native_requirements) {
    std::optional<PackageNativeProvider> selected;
    fs::path selected_root;
    const auto select = [&](const auto &p, const fs::path &base) {
      if (p.id != r.id)
        return;
      if (selected)
        fail("ambiguous native provider binding: " + r.id);
      selected = p;
      selected_root = base;
    };
    if (r.provider == "bundled") {
      for (const auto &p : m.native_providers)
        select(p, root);
    } else if (r.provider == "package") {
      std::vector<PackageDependency> dependencies = m.dependencies;
      dependencies.insert(dependencies.end(),m.artifact_dependencies.begin(),m.artifact_dependencies.end());
      for (const auto &d : dependencies) if (d.name == r.package) {
        VariantPolicy policy; policy.allow_native = true; policy.bundle = "auto";
        policy.native_requirements.push_back(r);
        policy.backend = m.target.compute_backend == "none" ? "cpu" : m.target.compute_backend;
        const auto artifact = find_registry_package(package_home(),d.name,d.version,policy);
        const auto base = artifact.parent_path();
        const auto descriptor = manifest(provider_read(base / "sputnik.build.json"));
        for (const auto &p : descriptor.native_providers) select(p,base);
      }
    } else {
      for (const auto &entry : external)
        select(entry.first, entry.second);
    }
    if (!selected)
      fail("missing native provider: " + r.id +
           " (set SPUTNIK_NATIVE_PROVIDERS)");
    if (selected->kind != r.kind || selected->version != r.version ||
        (!r.abi.empty() && selected->abi != r.abi) ||
        (!r.build_id.empty() && selected->build_id != r.build_id))
      fail("NATIVE_ABI_CONFLICT: " + r.id);
    for (const auto &name : r.libraries)
      if (std::none_of(selected->libraries.begin(), selected->libraries.end(),
                       [&](const auto &lib) {
                         return fs::path(lib.path).filename() == name;
                       }))
        fail("missing runtime export: " + name);
    bound.emplace(r.id, bind_provider(*selected, selected_root, cache));
  }
  for (const auto &r : m.native_requirements)
    if (r.kind == "sdk" && (bound.at(r.id).provider.runtime_build_id !=
                            bound.at(r.runtime).provider.build_id))
      fail("NATIVE_ABI_CONFLICT: SDK/runtime build mismatch");
  // A process cannot safely host independently prepared singleton runtimes.
  static std::map<std::string, std::string> process_providers;
  for (const auto &entry : bound) {
    if (entry.second.provider.kind != "runtime")
      continue;
    const auto old = process_providers.find(entry.first);
    if (old != process_providers.end() &&
        old->second != entry.second.fingerprint)
      fail("NATIVE_ABI_CONFLICT: process provider " + entry.first);
  }
  for (const auto &entry : bound)
    if (entry.second.provider.kind == "runtime")
      process_providers[entry.first] = entry.second.fingerprint;
  auto result = m.native_extensions;
  for (auto &extension : result) {
    std::vector<const BoundProvider *> runtimes;
    for (const auto &id : extension.requirements) {
      const auto &b = bound.at(id);
      extension.provider_fingerprints.push_back(id + "=" + b.fingerprint);
      if (b.provider.kind == "runtime") {
        runtimes.push_back(&b);
        for (const auto &dir : b.provider.library_dirs) {
          extension.library_dirs.push_back((b.root / dir).string());
          extension.runtime_library_dirs.push_back((b.root / dir).string());
        }
        extension.link_libraries.insert(extension.link_libraries.end(),
                                        b.provider.link_libraries.begin(),
                                        b.provider.link_libraries.end());
      } else if (extension.kind == "source") {
        for (const auto &dir : b.provider.include_dirs)
          extension.include_dirs.push_back((b.root / dir).string());
      }
    }
    if (extension.kind == "prebuilt") {
      if (runtimes.empty()) fail("prebuilt provider view needs runtime libraries");
      const auto original = provider_file(root, extension.library);
      const auto digest = file_sha256(original);
      const auto inventory = provider_inventory(root);
      if (!inventory.count(extension.library) ||
          inventory.at(extension.library) != digest)
        fail("extension file/inventory mismatch");

      std::string binding = digest;
      for (const auto *runtime : runtimes) binding += runtime->fingerprint;
      const auto view = cache / "views" / lexer::sha256_hex(binding);
      fs::create_directories(view / "lib");
      ProviderLock lock(view / "prepare.lock");
      const auto library = view / "lib" / original.filename();
      if (!fs::exists(library))
        fs::copy_file(original, library);
      if (fs::is_symlink(library) || file_sha256(library) != digest)
        fail("prepared extension tamper");
      const auto vendor = view / "lib/vendor";
      if (fs::is_symlink(vendor)) fail("prepared provider binding tamper");
      fs::create_directories(vendor);
      for (const auto *runtime : runtimes) for (const auto &lib : runtime->provider.libraries) {
        const auto target = runtime->root / lib.path;
        const auto destination = vendor / fs::path(lib.path).filename();
        if (!fs::exists(destination)) {
          std::error_code error; fs::create_hard_link(target,destination,error);
          if (error) fs::copy_file(target,destination);
        }
        if (fs::is_symlink(destination) || file_sha256(destination) != file_sha256(target)) fail("prepared provider binding tamper");
      }
      extension.library = library.string();
    }
    extension.provider_prepared = true;
  }
  return result;
}

std::string distribution_to_json(const DistributionInfo &info) {
  std::ostringstream out;
  out << "{\n  \"schema\": \"sputnik.package-inspection.v1\",\n  \"format\": "
         "\"sputnik.pkg.v1\",\n"
      << "  \"name\": " << quote(info.manifest.name)
      << ",\n  \"version\": " << quote(info.manifest.version)
      << ",\n  \"variant\": " << quote(info.manifest.variant)
      << ",\n  \"kind\": " << quote(info.manifest.kind)
      << ",\n  \"bundle\": " << quote(info.manifest.bundle)
      << ",\n  \"native_adapter\": "
      << quote(info.manifest.native_extensions.empty()
                   ? "none"
                   : info.manifest.native_extensions.front().kind)
      << ",\n  \"sha256\": " << quote("sha256:" + info.sha256)
      << ",\n  \"compressed_size\": " << info.compressed_size
      << ",\n  \"expanded_size\": " << info.expanded_size
      << ",\n  \"source_modules\": " << info.source_modules
      << ",\n  \"bytecode_modules\": " << info.bytecode_modules
      << ",\n  \"target\": {\"os\":" << quote(info.manifest.target.os)
      << ",\"arch\":" << quote(info.manifest.target.arch)
      << ",\"min_os\":" << quote(info.manifest.target.min_os)
      << ",\"libc\":" << quote(info.manifest.target.libc)
      << ",\"min_libc\":" << quote(info.manifest.target.min_libc)
      << ",\"compute_backend\":" << quote(info.manifest.target.compute_backend)
      << ",\"cuda_version\":" << quote(info.manifest.target.cuda_version)
      << ",\"min_cuda_driver\":" << quote(info.manifest.target.min_cuda_driver)
      << "},\n  \"native_requirements\": [";
  bool first = true;
  for (const auto &r : info.manifest.native_requirements) {
    if (!first) out << ','; first = false;
    out << "{\"id\":" << quote(r.id) << ",\"kind\":" << quote(r.kind)
        << ",\"version\":" << quote(r.version) << ",\"abi\":" << quote(r.abi)
        << ",\"build_id\":" << quote(r.build_id) << ",\"provider\":" << quote(r.provider)
        << ",\"package\":" << quote(r.package) << ",\"runtime\":" << quote(r.runtime) << '}';
  }
  out << "],\n  \"native_providers\": ["; first = true;
  for (const auto &p : info.manifest.native_providers) {
    if (!first) out << ','; first = false;
    out << "{\"id\":" << quote(p.id) << ",\"kind\":" << quote(p.kind)
        << ",\"version\":" << quote(p.version) << ",\"abi\":" << quote(p.abi)
        << ",\"build_id\":" << quote(p.build_id) << ",\"runtime_build_id\":" << quote(p.runtime_build_id) << '}';
  }
  out << "],\n  \"members\": [";
  first = true;
  for (const auto &m : info.members) {
    if (!first)
      out << ',';
    first = false;
    out << "\n    {\"path\":" << quote(m.path) << ",\"size\":" << m.size
        << ",\"sha256\":" << quote("sha256:" + m.sha256) << '}';
  }
  out << "\n  ]\n}\n";
  return out.str();
}
} // namespace sputnik::pkg
