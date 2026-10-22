#include "bytecode/format.h"
#include "frontend/lexer/token.h"
#include "package/distribution.h"

#include <archive.h>
#include <archive_entry.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace sputnik::pkg;
namespace {
unsigned checks = 0;
void expect(bool value, const std::string &message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}
template <class F> void rejects(const std::string &name, F f) {
  bool rejected = false;
  try {
    f();
  } catch (const std::exception &) {
    rejected = true;
  }
  expect(rejected, "accepted invalid input: " + name);
}
void write(const fs::path &path, const std::string &bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary);
  stream.write(bytes.data(), bytes.size());
  expect(stream.good(), "fixture write failed");
}
std::string read(const fs::path &path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), {}};
}
using Files = std::map<std::string, std::string>;
void write_archive(const fs::path &path, Files files,
                   const std::string &special = {}, bool symlink = false,
                   bool duplicate = false, bool wrong_digest = false) {
  std::string inventory;
  for (const auto &entry : files)
    if (entry.first != "checksums.sha256")
      inventory +=
          sputnik::lexer::sha256_hex(entry.second) + "  " + entry.first + "\n";
  if (wrong_digest)
    inventory[0] = inventory[0] == '0' ? '1' : '0';
  files["checksums.sha256"] = inventory;
  auto *a = archive_write_new();
  expect(archive_write_set_format_pax_restricted(a) == ARCHIVE_OK, "tar init");
  expect(archive_write_add_filter_gzip(a) == ARCHIVE_OK, "gzip init");
  expect(archive_write_open_filename(a, path.c_str()) == ARCHIVE_OK,
         "tar open");
  const auto member = [&](const std::string &name) {
    auto *entry = archive_entry_new();
    archive_entry_set_pathname(entry, name.c_str());
    archive_entry_set_filetype(entry, symlink && name == special ? AE_IFLNK
                                                                 : AE_IFREG);
    archive_entry_set_perm(entry, 0644);
    if (symlink && name == special)
      archive_entry_set_symlink(entry, "/tmp/outside");
    else
      archive_entry_set_size(entry, files.at(name).size());
    expect(archive_write_header(a, entry) == ARCHIVE_OK, "tar header");
    if (!(symlink && name == special))
      expect(
          archive_write_data(a, files.at(name).data(), files.at(name).size()) ==
              static_cast<la_ssize_t>(files.at(name).size()),
          "tar write");
    archive_entry_free(entry);
  };
  member("sputnik.toml");
  member("checksums.sha256");
  for (const auto &entry : files)
    if (entry.first != "sputnik.toml" && entry.first != "checksums.sha256")
      member(entry.first);
  if (duplicate)
    member(special);
  expect(archive_write_close(a) == ARCHIVE_OK, "tar close");
  archive_write_free(a);
}
} // namespace

int main() {
  const auto root = fs::temp_directory_path() /
                    ("sputnik-distribution-" + std::to_string(getpid()));
  struct Clean {
    fs::path path;
    ~Clean() { fs::remove_all(path); }
  } clean{root};
  try {
    const auto source = root / "source";
    write(source / "src/demo.s",
          "package demo\nexport answer\ndef answer(): 42\n");
    write(source / "src/helper.s",
          "package demo.helper\nexport answer\ndef answer(): 42\n");
    write(source / "resources/model.bin", std::string("\0\1\2", 3));
    PackageManifest m;
    m.name = "demo";
    m.version = "1.0.0";
    m.root_module = "demo";
    m.modules = {{"demo", "src/demo.s"}, {"demo.helper", "src/helper.s"}};
    m.files = {"resources"};
    auto first = build_distribution(m, source, root / "one.sputnikpkg");
    fs::last_write_time(source / "src/demo.s",
                        fs::file_time_type::clock::now());
    auto second = build_distribution(m, source, root / "two.sputnikpkg");
    expect(first.sha256 == second.sha256,
           "source archive is not deterministic");
    expect(first.source_modules == 2 && first.bytecode_modules == 0,
           "source inventory");
    const int inspected_fd = open((root / "one.sputnikpkg").c_str(), O_RDONLY);
    expect(inspected_fd >= 0, "open inspector fixture");
    lseek(inspected_fd, 2, SEEK_SET);
    expect(inspect_distribution_fd(inspected_fd, "standard").sha256 == first.sha256,
           "trusted profile preserves valid inspection");
    rejects("different admitted profile", [&] {
      inspect_distribution_fd(inspected_fd, "native-large");
    });
    expect(lseek(inspected_fd, 0, SEEK_CUR) == 2, "failed inspection preserves offset");
    rejects("unknown admitted profile", [&] {
      inspect_distribution_fd(inspected_fd, "unlimited");
    });
    close(inspected_fd);
    auto large_manifest = m;
    large_manifest.limit_profile = "native-large";
    Files hostile{{"sputnik.toml", render_distribution_manifest(large_manifest)},
                  {"../payload", "must not be inspected"}};
    write_archive(root / "profile-mismatch.sputnikpkg", hostile);
    const int mismatch_fd = open((root / "profile-mismatch.sputnikpkg").c_str(), O_RDONLY);
    bool early_profile_rejection = false;
    try {
      inspect_distribution_fd(mismatch_fd, "standard");
    } catch (const std::exception &error) {
      early_profile_rejection = std::string(error.what()).find("admitted profile") != std::string::npos;
    }
    close(mismatch_fd);
    expect(early_profile_rejection, "profile rejected before inspecting hostile payload");
    extract_distribution(root / "one.sputnikpkg", root / "extracted");
    expect(read(root / "extracted/resources/model.bin") ==
               std::string("\0\1\2", 3),
           "resource roundtrip");
    rejects("existing extraction destination", [&] {
      extract_distribution(root / "one.sputnikpkg", root / "extracted");
    });

    DistributionBuildOptions opts;
    opts.module_mode = "mixed";
    opts.bytecode_modules = {"demo.helper"};
    opts.compile = [](const auto &) {
      return sputnik::bytecode::serialize_module(sputnik::bytecode::BcModule{});
    };
    const auto mixed =
        build_distribution(m, source, root / "mixed.sputnikpkg", opts);
    expect(mixed.source_modules == 1 && mixed.bytecode_modules == 1,
           "mixed inventory");
    extract_distribution(root / "mixed.sputnikpkg", root / "mixed");
    expect(!fs::exists(root / "mixed/src/helper.s"), "closed source leaked");
    expect(fs::exists(root / "mixed/meta/build.json"),
           "missing derived build metadata");
    expect(mixed.sha256 ==
               build_distribution(m, source, root / "mixed2.sputnikpkg", opts)
                   .sha256,
           "bytecode archive is not deterministic");
    auto leak = m;
    leak.files.push_back("src");
    rejects("closed source in packlist",
            [&] { build_distribution(leak, source, root / "bad", opts); });
    opts.bytecode_modules.insert("absent");
    rejects("unknown closed module",
            [&] { build_distribution(m, source, root / "bad", opts); });
    opts.bytecode_modules = {"demo.helper"};
    opts.compile = [](const auto &) { return std::vector<std::uint8_t>{0, 1}; };
    rejects("invalid compiler bytecode",
            [&] { build_distribution(m, source, root / "bad", opts); });
    write(root / "outside.txt", "secret");
    fs::create_symlink(root / "outside.txt", source / "resources/escape");
    rejects("escaping symlink",
            [&] { build_distribution(m, source, root / "bad"); });
    fs::remove(source / "resources/escape");

    Files files;
    for (const auto &member : first.members)
      files[member.path] = read(root / "extracted" / member.path);
    const auto negative = [&](const std::string &name, const Files &input,
                              const std::string &special = {},
                              bool link = false, bool dup = false,
                              bool wrong_digest = false) {
      write_archive(root / "malicious", input, special, link, dup,
                    wrong_digest);
      rejects(name, [&] { inspect_distribution(root / "malicious"); });
    };
    auto bad = files;
    bad["src/demo.s"] = "package wrong\nexport answer\ndef answer(): 42\n";
    negative("source declaration mismatch", bad);
    bad = files;
    bad["src/demo.s"] += "def broken(\n";
    negative("invalid source syntax", bad);
    bad = files;
    bad["../escaped"] = "bad";
    negative("traversal", bad);
    bad = files;
    bad["/absolute"] = "bad";
    negative("absolute path", bad);
    bad = files;
    bad["src/DEMO.s"] = bad["src/demo.s"];
    negative("case collision", bad);
    bad = files;
    bad["src"] = "parent";
    negative("parent file collision", bad);
    negative("archive symlink", files, "src/demo.s", true);
    negative("duplicate member", files, "src/demo.s", false, true);
    negative("inventory digest mismatch", files, {}, false, false, true);
    bad = files;
    bad.erase("src/demo.s");
    negative("missing declared module", bad);
    bad = files;
    bad["sputnik.toml"] += "\n[package]\nversion = \"2.0.0\"\n";
    negative("duplicate metadata field", bad);
    bad = files;
    bad["sputnik.toml"].replace(
        bad["sputnik.toml"].find("limit_profile = \"standard\""), 26,
        "limit_profile = \"unlimited\"");
    negative("unknown budget profile", bad);
    bad = files;
    bad["hidden.sputnikbc"] = "bad";
    negative("undeclared invalid bytecode", bad);
    bad = files;
    bad["origin.json"] = "{}";
    negative("reserved store metadata", bad);
    Files binary;
    for (const auto &member : mixed.members)
      binary[member.path] = read(root / "mixed" / member.path);
    binary["meta/build.json"].replace(
        binary["meta/build.json"].find("profile_flags\":0"), 16,
        "profile_flags\":1");
    negative("build assertions disagree with bytecode", binary);

    const auto original = read(root / "one.sputnikpkg");
    auto corrupted = original;
    corrupted[corrupted.size() - 8] ^= 1;
    write(root / "corrupt", corrupted);
    rejects("gzip CRC", [&] { inspect_distribution(root / "corrupt"); });
    write(root / "corrupt", original.substr(0, original.size() - 4));
    rejects("gzip truncation", [&] { inspect_distribution(root / "corrupt"); });
    write(root / "corrupt", original + "garbage");
    rejects("trailing garbage",
            [&] { inspect_distribution(root / "corrupt"); });
    write(root / "corrupt", original + original);
    rejects("appended second tar",
            [&] { inspect_distribution(root / "corrupt"); });
    rejects("invalid extraction cleans up", [&] {
      extract_distribution(root / "corrupt", root / "invalid-extract");
    });
    expect(!fs::exists(root / "invalid-extract"), "invalid extract left files");
    // Local providers are real native-only artifacts, with no fake module.
    PackageManifest runtime;
    runtime.name = "fixture-runtime";
    runtime.version = "1.0.0";
    runtime.kind = "native";
    runtime.target.os = "macos";
#if defined(__aarch64__) || defined(__arm64__)
    runtime.target.arch = "aarch64";
#else
    runtime.target.arch = "x86_64";
#endif
    runtime.target.min_os = "13.0";
    PackageNativeProvider provider;
    provider.id = "fixture-runtime";
    provider.kind = "runtime";
    provider.version = "1.0.0";
    provider.abi = "fixture-1";
    provider.build_id = "sha256:" + std::string(64, 'a');
    provider.files = {"data/runtime.bin"};
    provider.library_dirs = {"data"};
    runtime.native_providers = {provider};
    write(root / "provider-input/data/runtime.bin", "runtime resources");
    build_distribution(runtime, root / "provider-input",
                       root / "provider.sputnikpkg");
    const auto provider_info = extract_distribution(
        root / "provider.sputnikpkg", root / "runtime-provider");
    expect(provider_info.manifest.modules.empty() &&
               provider_info.manifest.kind == "native",
           "native-only roundtrip");
    auto soname_provider = runtime;
    soname_provider.name = "fixture-soname";
    soname_provider.target.os = "linux";
    soname_provider.target.arch = "x86_64";
    soname_provider.target.libc = "gnu";
    soname_provider.target.min_os.clear();
    soname_provider.native_providers.front().files.clear();
    soname_provider.native_providers.front().libraries = {
        {"lib/libfixture.so.13", "dependency", "fixture-1"}};
    std::string elf(64, '\0');
    elf.replace(0, 4, "\177ELF", 4);
    elf[18] = 62;
    write(root / "provider-input/lib/libfixture.so.13", elf);
    const auto soname = build_distribution(soname_provider, root / "provider-input",
                                          root / "soname.sputnikpkg");
    expect(soname.manifest.native_providers.front().libraries.front().path ==
               "lib/libfixture.so.13",
           "versioned ELF SONAME roundtrip");
    write(root / "provider-input/lib/libfixture.so.13", "invalid ELF");
    rejects("versioned SONAME binary validation", [&] {
      build_distribution(soname_provider, root / "provider-input",
                         root / "invalid-soname.sputnikpkg");
    });
    auto sdk = runtime;
    sdk.name = "fixture-sdk";
    auto &sdk_provider = sdk.native_providers.front();
    sdk_provider.id = "fixture-sdk";
    sdk_provider.kind = "sdk";
    sdk_provider.runtime_build_id = provider.build_id;
    sdk_provider.files = {"include/api.h"};
    sdk_provider.library_dirs.clear();
    sdk_provider.include_dirs = {"include"};
    write(root / "provider-input/include/api.h", "// fixture SDK\n");
    build_distribution(sdk, root / "provider-input", root / "sdk.sputnikpkg");
    extract_distribution(root / "sdk.sputnikpkg", root / "sdk-provider");
    auto consumer = m;
    PackageNativeRequirement req;
    req.id = provider.id;
    req.kind = "runtime";
    req.version = "1.0.0";
    req.abi = "fixture-1";
    req.build_id = provider.build_id;
    auto sdk_req = req;
    sdk_req.id = "fixture-sdk";
    sdk_req.kind = "sdk";
    sdk_req.runtime = req.id;
    consumer.native_requirements = {req, sdk_req};
    PackageNativeExtension ext;
    ext.name = "consumer";
    ext.kind = "source";
    ext.language = "c++";
    ext.sources = {"native/adapter.cpp"};
    ext.requirements = {req.id, sdk_req.id};
    consumer.native_extensions = {ext};
    write(source / "native/adapter.cpp", "// adapter source\n");
    const auto slim =
        build_distribution(consumer, source, root / "slim.sputnikpkg");
    expect(slim.manifest.bundle == "none" &&
               slim.manifest.native_requirements.size() == 2,
           "requirement roundtrip");
    const char *old_bindings = std::getenv("SPUTNIK_NATIVE_PROVIDERS");
    const std::string saved_bindings = old_bindings ? old_bindings : "";
    struct RestoreBindings {
      bool had;
      std::string value;
      ~RestoreBindings() {
        if (had)
          setenv("SPUTNIK_NATIVE_PROVIDERS", value.c_str(), 1);
        else
          unsetenv("SPUTNIK_NATIVE_PROVIDERS");
      }
    } restore_bindings{old_bindings != nullptr, saved_bindings};
    unsetenv("SPUTNIK_NATIVE_PROVIDERS");
    rejects("missing explicit provider", [&] {
      prepare_native_providers(consumer, source, root / "cache");
    });
    const auto bindings = (root / "runtime-provider").string() + ":" +
                          (root / "sdk-provider").string();
    setenv("SPUTNIK_NATIVE_PROVIDERS", bindings.c_str(), 1);
    const auto prepared =
        prepare_native_providers(consumer, source, root / "cache");
    expect(prepared.front().provider_prepared &&
               prepared.front().provider_fingerprints.size() == 2,
           "missing prepared fingerprints");
    expect(prepared.front().include_dirs ==
               prepare_native_providers(consumer, source, root / "cache")
                   .front()
                   .include_dirs,
           "providers did not share canonical roots");
    auto mismatch = consumer;
    mismatch.native_requirements.front().version = "2.14.0";
    rejects("hard runtime version mismatch", [&] {
      prepare_native_providers(mismatch, source, root / "cache");
    });
    mismatch = consumer;
    mismatch.native_requirements.front().abi = "another-cxx-abi";
    rejects("hard runtime ABI mismatch", [&] {
      prepare_native_providers(mismatch, source, root / "cache");
    });
    mismatch = consumer;
    mismatch.native_requirements.front().build_id =
        "sha256:" + std::string(64, 'b');
    rejects("wrong runtime build", [&] {
      prepare_native_providers(mismatch, source, root / "cache");
    });
    auto wrong_sdk = sdk;
    wrong_sdk.native_providers.front().runtime_build_id =
        "sha256:" + std::string(64, 'b');
    build_distribution(wrong_sdk, root / "provider-input",
                       root / "wrong-sdk.sputnikpkg");
    extract_distribution(root / "wrong-sdk.sputnikpkg", root / "wrong-sdk");
    const auto wrong_bindings = (root / "runtime-provider").string() + ":" +
                                (root / "wrong-sdk").string();
    setenv("SPUTNIK_NATIVE_PROVIDERS", wrong_bindings.c_str(), 1);
    rejects("SDK runtime mismatch", [&] {
      prepare_native_providers(consumer, source, root / "cache");
    });
    setenv("SPUTNIK_NATIVE_PROVIDERS",
           (bindings + ":" + (root / "runtime-provider").string()).c_str(), 1);
    rejects("ambiguous providers", [&] {
      prepare_native_providers(consumer, source, root / "cache");
    });
    setenv("SPUTNIK_NATIVE_PROVIDERS", bindings.c_str(), 1);
    write(root / "runtime-provider/data/runtime.bin", "tampered runtime");
    rejects("external provider tamper", [&] {
      prepare_native_providers(consumer, source, root / "cache");
    });
    write(root / "runtime-provider/data/runtime.bin", "runtime resources");
    write(fs::path(prepared.front().include_dirs.front()) / "api.h",
          "tampered SDK cache");
    rejects("prepared provider tamper", [&] {
      prepare_native_providers(consumer, source, root / "cache");
    });
    auto conflicting_runtime = runtime;
    conflicting_runtime.native_providers.front().build_id =
        "sha256:" + std::string(64, 'b');
    build_distribution(conflicting_runtime, root / "provider-input",
                       root / "conflict.sputnikpkg");
    extract_distribution(root / "conflict.sputnikpkg",
                         root / "conflict-provider");
    auto another_consumer = consumer;
    another_consumer.native_requirements = {req};
    another_consumer.native_requirements.front().build_id.clear();
    another_consumer.native_extensions.front().requirements = {req.id};
    setenv("SPUTNIK_NATIVE_PROVIDERS", (root / "conflict-provider").c_str(), 1);
    rejects("two consumers with conflicting singleton runtime", [&] {
      prepare_native_providers(another_consumer, source, root / "cache");
    });
    auto invalid_profile = m;
    invalid_profile.bundle = "sdk";
    rejects("SDK bundle without native sources", [&] {
      build_distribution(invalid_profile, source,
                         root / "invalid-sdk.sputnikpkg");
    });
    auto missing_asset = sdk;
    missing_asset.native_providers.front().files = {"missing.h"};
    rejects("missing provider resource", [&] {
      build_distribution(missing_asset, root / "provider-input",
                         root / "invalid-provider.sputnikpkg");
    });

    std::cout << "package_distribution_tests: PASS (" << checks << " checks)\n";
  } catch (const std::exception &error) {
    std::cerr << "package_distribution_tests: " << error.what() << '\n';
    return 1;
  }
}
