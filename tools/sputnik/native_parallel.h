// Included inside the driver's anonymous namespace, after its command helpers.
// Keep the C++ frontend single-pass; parallelize LLVM optimization/code generation
// and retain cross-partition inlining with ThinLTO.
struct NativeParallelBuild {
  std::size_t jobs = 1;
  std::vector<std::string> objects;
  std::filesystem::path directory;
  std::string linker;
};

[[maybe_unused]] std::string native_llvm_tool(const std::string &name, const std::string &major,
                             const std::string &cxx) {
  const auto sibling = std::filesystem::path(cxx).parent_path() / name;
  const std::vector<std::string> candidates = {
      name + "-" + major, sibling.string(), name};
  for (const auto &candidate : candidates) {
    const std::string version = capture_command_output(
        shell_command({candidate, "--version"}) + " 2>/dev/null");
    if (version.find("version " + major + ".") != std::string::npos ||
        version.find("LLD " + major + ".") != std::string::npos) {
      std::string executable = capture_command_output(
          shell_command({"command", "-v", candidate}) + " 2>/dev/null");
      while (!executable.empty() && (executable.back() == '\n' || executable.back() == '\r')) {
        executable.pop_back();
      }
      if (!executable.empty()) return executable;
    }
  }
  return {};
}

NativeParallelBuild prepare_native_parallel_build(
    const std::string &cxx, const std::filesystem::path &source,
    const std::filesystem::path &runtime_root, std::size_t code_count) {
  NativeParallelBuild result;
#if defined(__linux__)
  bool required = false;
  std::size_t jobs = std::min<std::size_t>(
      8U, std::max(1U, std::thread::hardware_concurrency()));
  if (const char *setting = std::getenv("SPUTNIK_NATIVE_JOBS")) {
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(setting, &end, 10);
    if (end == setting || *end != '\0' || *setting == '-' || parsed > 64UL) {
      throw std::runtime_error("SPUTNIK_NATIVE_JOBS must be an integer from 0 to 64");
    }
    if (parsed == 1UL) return result;
    if (parsed > 1UL) {
      jobs = static_cast<std::size_t>(parsed);
      required = true;
    }
  }
  // Small programs are faster through a single optimizer invocation.
  if (jobs < 2U || (!required && code_count < 512U)) return result;
  const std::string version = capture_command_output(
      shell_command({cxx, "--version"}) + " 2>/dev/null");
  const auto marker = version.find("clang version ");
  std::string major;
  if (marker != std::string::npos) {
    const auto begin = marker + std::string("clang version ").size();
    const auto end = version.find('.', begin);
    if (end != std::string::npos) major = version.substr(begin, end - begin);
  }
  std::map<std::string, std::string> llvm;
  if (!major.empty() && std::atoi(major.c_str()) >= 19) {
    for (const auto &tool : {"llvm-dis", "llvm-as", "llvm-split", "opt", "ld.lld"}) {
      llvm[tool] = native_llvm_tool(tool, major, cxx);
    }
  }
  if (llvm.empty() || std::any_of(llvm.begin(), llvm.end(),
                                [](const auto &item) { return item.second.empty(); })) {
    if (required) {
      throw std::runtime_error(
          "parallel native builds require Clang 19+ and matching llvm-dis, "
          "llvm-as, llvm-split, opt and ld.lld; set SPUTNIK_NATIVE_CXX or "
          "SPUTNIK_NATIVE_JOBS=1 for a serial build");
    }
    return result;
  }
  result.jobs = jobs;
  result.directory = source.string() + ".llvm";
  result.linker = llvm.at("ld.lld");
  std::filesystem::create_directories(result.directory);
  const auto path = [&](const std::string &name) {
    return (result.directory / name).string();
  };
  const auto run = [&](const std::vector<std::string> &arguments) {
    const std::string command = shell_command(arguments);
    if (std::system(command.c_str()) != 0) {
      throw std::runtime_error("parallel native compile failed: " + command);
    }
  };
  std::vector<std::string> frontend{cxx};
  const auto &flags = native_runtime_compile_flags();
  frontend.insert(frontend.end(), flags.begin(), flags.end());
  frontend.insert(frontend.end(), {"-I", runtime_root.string(), "-Xclang",
      "-disable-llvm-passes", "-emit-llvm", "-c", source.string(), "-o", path("input.bc")});
  run(frontend);
  run({llvm.at("llvm-dis"), path("input.bc"), "-o", path("input.ll")});
  std::string ir = read_file(path("input.ll"));
  // A partition may call an inline C++ definition owned by another partition.
  // Keep its body until the ThinLTO linker has seen every reference. linkonce
  // bodies would otherwise be dropped by pre-link GlobalDCE in the owner.
  std::istringstream lines(ir);
  std::ostringstream retained_ir;
  std::string line;
  while (std::getline(lines, line)) {
    std::size_t linkage = std::string::npos;
    if (line.rfind("define ", 0U) == 0U) {
      linkage = 7U;
    } else if (!line.empty() && line.front() == '@') {
      const auto equals = line.find(" = ");
      if (equals != std::string::npos) linkage = equals + 3U;
    }
    if (linkage != std::string::npos) {
      for (const std::string available : {"available_externally ", "linkonce_odr "}) {
        if (line.compare(linkage, available.size(), available) == 0) {
          line.replace(linkage, available.size(), "weak_odr ");
          break;
        }
      }
    }
    retained_ir << line << '\n';
  }
  write_file(path("input.ll"), retained_ir.str());
  run({llvm.at("llvm-as"), path("input.ll"), "-o", path("retained.bc")});
  const std::size_t partitions = jobs * 2U;
  run({llvm.at("llvm-split"), "-j", std::to_string(partitions),
       "-o", path("part"), path("retained.bc")});
  for (std::size_t start = 0; start < partitions; start += jobs) {
    std::ostringstream batch;
    batch << "status=0\n";
    const std::size_t end = std::min(partitions, start + jobs);
    for (std::size_t i = start; i < end; ++i) {
      const std::string part = path("part" + std::to_string(i));
      result.objects.push_back(part + ".o");
      batch << "(" << shell_command({llvm.at("opt"), "-passes=thinlto-pre-link<O2>",
          "-module-summary", part, "-o", part + ".o"}) << ") &\np" << i << "=$!\n";
    }
    for (std::size_t i = start; i < end; ++i) {
      batch << "wait \"$p" << i << "\" || status=1\n";
    }
    batch << "exit \"$status\"\n";
    if (std::system(batch.str().c_str()) != 0) {
      throw std::runtime_error("parallel native LLVM optimization failed");
    }
  }
#else
  (void)cxx;
  (void)source;
  (void)runtime_root;
  (void)code_count;
#endif
  return result;
}
