#include "notebook/project.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace amber::notebook {
namespace {

constexpr std::size_t kMaxDocumentBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaxModuleBytes = 16U * 1024U * 1024U;
constexpr const char *kDocumentName = "project.json";

[[noreturn]] void io_error(const std::string &operation) {
  const int error = errno;
  throw std::runtime_error("notebook project: " + operation + ": " +
                           std::strerror(error));
}

class FileDescriptor {
public:
  explicit FileDescriptor(int fd) : fd_(fd) {}
  ~FileDescriptor() {
    if (fd_ >= 0)
      ::close(fd_);
  }
  FileDescriptor(const FileDescriptor &) = delete;
  FileDescriptor &operator=(const FileDescriptor &) = delete;
  FileDescriptor(FileDescriptor &&other) noexcept
      : fd_(std::exchange(other.fd_, -1)) {}
  FileDescriptor &operator=(FileDescriptor &&other) noexcept {
    if (this != &other) {
      if (fd_ >= 0)
        ::close(fd_);
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }
  int get() const { return fd_; }

private:
  int fd_;
};

FileDescriptor open_directory(const std::filesystem::path &path) {
  // Remove only a trailing slash/dot to make O_NOFOLLOW check the selected
  // directory itself. Do NOT lexically collapse '..': a symlink in a parent
  // changes its meaning, and opening a different directory would be unsafe.
  auto normalized = path;
  while (normalized != normalized.root_path() && normalized.has_parent_path() &&
         (normalized.filename().empty() || normalized.filename() == ".")) {
    normalized = normalized.parent_path();
  }
  const int fd = ::open(normalized.c_str(),
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
    io_error("open directory " + path.string());
  return FileDescriptor(fd);
}

void require_regular(int fd, const std::string &name) {
  struct stat info{};
  if (::fstat(fd, &info) != 0)
    io_error("stat " + name);
  if (!S_ISREG(info.st_mode)) {
    throw std::runtime_error("notebook project: not a regular file: " + name);
  }
}

FileDescriptor open_member(int root, const std::string &path) {
  // Paths have already passed schema validation. Traverse relative to the
  // open directory, rejecting symlinks at EVERY component, not just the leaf.
  FileDescriptor parent(::fcntl(root, F_DUPFD_CLOEXEC, 0));
  if (parent.get() < 0)
    io_error("duplicate project descriptor");
  std::size_t start = 0;
  while (true) {
    const auto slash = path.find('/', start);
    const bool leaf = slash == std::string::npos;
    const auto part = path.substr(start, leaf ? slash : slash - start);
    const int fd = ::openat(parent.get(), part.c_str(),
                            O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK |
                                (leaf ? 0 : O_DIRECTORY));
    if (fd < 0)
      io_error("open member " + path);
    parent = FileDescriptor(fd);
    if (leaf) {
      require_regular(parent.get(), path);
      return parent;
    }
    start = slash + 1;
  }
}

struct MemberParent {
  FileDescriptor directory;
  std::string leaf;
};

void sync_directory(int root);

MemberParent open_member_parent(int root, const std::string &path) {
  FileDescriptor parent(::fcntl(root, F_DUPFD_CLOEXEC, 0));
  if (parent.get() < 0)
    io_error("duplicate project descriptor");
  const auto last_slash = path.rfind('/');
  if (last_slash == std::string::npos)
    return {std::move(parent), path};
  std::size_t start = 0;
  while (start < last_slash) {
    const auto slash = path.find('/', start);
    const auto end = slash == std::string::npos ? last_slash : slash;
    const auto part = path.substr(start, end - start);
    const int fd =
        ::openat(parent.get(), part.c_str(),
                 O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | O_DIRECTORY);
    if (fd < 0)
      io_error("open parent for " + path);
    parent = FileDescriptor(fd);
    if (slash == std::string::npos || slash >= last_slash)
      break;
    start = slash + 1;
  }
  return {std::move(parent), path.substr(last_slash + 1)};
}

MemberParent open_or_create_member_parent(int root, const std::string &path) {
  FileDescriptor parent(::fcntl(root, F_DUPFD_CLOEXEC, 0));
  if (parent.get() < 0)
    io_error("duplicate project descriptor");
  const auto last_slash = path.rfind('/');
  if (last_slash == std::string::npos)
    return {std::move(parent), path};

  std::size_t start = 0;
  while (start < last_slash) {
    const auto slash = path.find('/', start);
    const auto end = slash == std::string::npos ? last_slash : slash;
    const auto part = path.substr(start, end - start);
    bool created = false;
    int fd = ::openat(parent.get(), part.c_str(),
                      O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK |
                          O_DIRECTORY);
    if (fd < 0 && errno == ENOENT) {
      if (::mkdirat(parent.get(), part.c_str(), 0700) != 0) {
        if (errno != EEXIST)
          io_error("create parent for " + path);
      } else {
        created = true;
      }
      // mkdirat has no no-follow flag. Re-open the component with
      // O_NOFOLLOW, which rejects a concurrently installed symlink and pins
      // the directory descriptor used for all later operations.
      fd = ::openat(parent.get(), part.c_str(),
                    O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK |
                        O_DIRECTORY);
    }
    if (fd < 0)
      io_error("open parent for " + path);
    FileDescriptor next(fd);
    if (created) {
      // Sync the directory containing the new entry as well as the new
      // directory itself. Syncing only the child would not make the parent
      // link durable after a crash.
      sync_directory(parent.get());
      sync_directory(next.get());
    }
    parent = std::move(next);
    if (slash == std::string::npos || slash >= last_slash)
      break;
    start = slash + 1;
  }
  return {std::move(parent), path.substr(last_slash + 1)};
}

void require_new_member_target(int directory, const std::string &leaf,
                               const std::string &path) {
  struct stat info{};
  if (::fstatat(directory, leaf.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0) {
    throw std::runtime_error("notebook project: module path already exists: " +
                             path);
  }
  if (errno != ENOENT)
    io_error("check module path " + path);
}

void validate_members(int root, const ProjectDocument &document) {
  for (const auto &module : document.modules) {
    (void)open_member(root, module.path);
  }
}

std::string read_member(int root, const std::string &path, std::size_t limit) {
  auto file = open_member(root, path);
  std::string result;
  char buffer[8192];
  while (true) {
    const auto count = ::read(file.get(), buffer, sizeof(buffer));
    if (count < 0) {
      if (errno == EINTR)
        continue;
      io_error("read " + path);
    }
    if (count == 0)
      break;
    if (result.size() + static_cast<std::size_t>(count) > limit) {
      throw std::runtime_error("notebook project: " + path + " exceeds 16 MiB");
    }
    result.append(buffer, static_cast<std::size_t>(count));
  }
  return result;
}

std::string read_document(int root) {
  return read_member(root, kDocumentName, kMaxDocumentBytes);
}

mode_t member_mode(int root, const std::string &path) {
  auto file = open_member(root, path);
  struct stat info{};
  if (::fstat(file.get(), &info) != 0)
    io_error("stat " + path);
  return info.st_mode & static_cast<mode_t>(0777);
}

class ProjectLock {
public:
  explicit ProjectLock(int root)
      : file_(::openat(root, ".iamber-project.lock",
                       O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK,
                       0600)) {
    if (file_.get() < 0)
      io_error("open save lock");
    require_regular(file_.get(), "save lock");
    // Never stall the UI behind another writer. The user can retry Save.
    if (::flock(file_.get(), LOCK_EX | LOCK_NB) != 0)
      io_error("lock project");
  }

private:
  FileDescriptor file_;
};

class StagedMember {
public:
  StagedMember(int directory, std::string target, const std::string &bytes,
               mode_t mode = 0600)
      : directory_(directory), target_(std::move(target)), file_(-1) {
    static std::atomic<unsigned long long> next{0};
    for (int attempt = 0; attempt != 128; ++attempt) {
      name_ = ".iamber.tmp-" + std::to_string(::getpid()) + "-" +
              std::to_string(next.fetch_add(1, std::memory_order_relaxed));
      const int fd =
          ::openat(directory_, name_.c_str(),
                   O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, mode);
      if (fd >= 0) {
        file_ = FileDescriptor(fd);
        break;
      }
      if (errno != EEXIST)
        io_error("create staged document");
    }
    if (file_.get() < 0)
      throw std::runtime_error(
          "notebook project: no temporary filename available");
    try {
      std::size_t offset = 0;
      while (offset != bytes.size()) {
        const auto count =
            ::write(file_.get(), bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR)
          continue;
        if (count <= 0)
          io_error("write staged document");
        offset += static_cast<std::size_t>(count);
      }
      if (::fsync(file_.get()) != 0)
        io_error("sync staged document");
    } catch (...) {
      ::unlinkat(directory_, name_.c_str(), 0);
      throw;
    }
  }
  ~StagedMember() {
    if (!name_.empty())
      ::unlinkat(directory_, name_.c_str(), 0);
  }
  void replace() {
    if (::renameat(directory_, name_.c_str(), directory_, target_.c_str()) !=
        0) {
      io_error("replace " + target_);
    }
    name_.clear();
  }
  void create() {
    // linkat is an atomic no-replace publication on both macOS and Linux.
    if (::linkat(directory_, name_.c_str(), directory_, target_.c_str(), 0) !=
        0) {
      io_error("create " + target_);
    }
  }

private:
  int directory_;
  std::string target_;
  FileDescriptor file_;
  std::string name_;
};

void sync_directory(int root) {
  if (::fsync(root) != 0 && errno != EINVAL && errno != ENOTSUP) {
    io_error("sync directory (project member has already been saved)");
  }
}

ProjectDocument module_manifest_snapshot(const LoadedProject &project) {
  ProjectDocument manifest = parse_project_document(project.baseline);
  if (serialize_project_document(project.document) !=
      serialize_project_document(manifest)) {
    throw std::runtime_error(
        "notebook project: in-memory module manifest changed; save the "
        "project or reopen before accessing modules");
  }
  return manifest;
}

} // namespace

LoadedProject load_project(const std::filesystem::path &directory) {
  auto root = open_directory(directory);
  LoadedProject result;
  result.directory = std::filesystem::canonical(directory);
  result.baseline = read_document(root.get());
  result.document = parse_project_document(result.baseline);
  validate_members(root.get(), result.document);
  return result;
}

LoadedProject create_project(const std::filesystem::path &directory,
                             const ProjectDocument &document) {
  const auto bytes = serialize_project_document(document);
  if (!document.modules.empty()) {
    throw std::runtime_error(
        "notebook project: create an empty project before adding module files");
  }
  // Single-component creation: missing parents and existing directories fail.
  if (!std::filesystem::create_directory(directory)) {
    throw std::runtime_error("notebook project: destination already exists: " +
                             directory.string());
  }
  auto root = open_directory(directory);
  StagedMember staged(root.get(), kDocumentName, bytes);
  staged.create();
  sync_directory(root.get());
  return {std::filesystem::canonical(directory), document, bytes};
}

void save_project(LoadedProject *project, const ProjectDocument &document) {
  if (!project)
    throw std::invalid_argument("notebook project: null project");
  const auto bytes = serialize_project_document(document);
  // Allocate the post-commit state before touching disk. Publication must not
  // be followed by an allocation failure that leaves a stale baseline.
  ProjectDocument published = document;
  std::string baseline = bytes;
  auto root = open_directory(project->directory);
  ProjectLock lock(root.get());
  if (read_document(root.get()) != project->baseline) {
    throw std::runtime_error("notebook project: project.json changed "
                             "externally; reopen before saving");
  }
  validate_members(root.get(), document);
  if (bytes == project->baseline) {
    project->document = std::move(published);
    return;
  }
  StagedMember staged(root.get(), kDocumentName, bytes);
  // Recheck after staging too. Noncooperating writers can still race the
  // final rename; the format deliberately makes no stronger lock claim.
  if (read_document(root.get()) != project->baseline) {
    throw std::runtime_error("notebook project: project.json changed during "
                             "save; reopen before saving");
  }
  staged.replace();
  project->baseline.swap(baseline);
  project->document = std::move(published);
  sync_directory(root.get());
}

LoadedProjectModule load_project_module(const LoadedProject &project,
                                        const std::string &module_id) {
  // The exact disk baseline, not the caller-mutable document view, owns the
  // module mapping used for filesystem access.
  const ProjectDocument manifest = module_manifest_snapshot(project);
  auto root = open_directory(project.directory);
  if (read_document(root.get()) != project.baseline) {
    throw std::runtime_error(
        "notebook project: project.json changed externally; reopen before "
        "loading a module");
  }
  const auto found = std::find_if(
      manifest.modules.begin(), manifest.modules.end(),
      [&](const ProjectModule &module) { return module.id == module_id; });
  if (found == manifest.modules.end()) {
    throw std::runtime_error("notebook project: unknown module: " + module_id);
  }
  const auto source = read_member(root.get(), found->path, kMaxModuleBytes);
  return {found->id, found->path, source, source};
}

void save_project_module(const LoadedProject &project,
                         LoadedProjectModule *module,
                         const std::string &source) {
  if (!module)
    throw std::invalid_argument("notebook project: null module");
  if (source.size() > kMaxModuleBytes) {
    throw std::runtime_error("notebook project: module source exceeds 16 MiB");
  }
  // Allocate all post-commit state before disk publication. The swaps below
  // are noexcept, so a successful rename cannot leave stale editor baselines.
  std::string published_source = source;
  std::string baseline = source;
  const ProjectDocument manifest = module_manifest_snapshot(project);
  auto root = open_directory(project.directory);
  ProjectLock lock(root.get());
  if (read_document(root.get()) != project.baseline) {
    throw std::runtime_error(
        "notebook project: project.json changed externally; reopen before "
        "saving a module");
  }
  const auto found = std::find_if(
      manifest.modules.begin(), manifest.modules.end(),
      [&](const ProjectModule &entry) { return entry.id == module->id; });
  if (found == manifest.modules.end() || found->path != module->path) {
    throw std::runtime_error(
        "notebook project: module mapping changed; reopen before saving");
  }
  if (read_member(root.get(), module->path, kMaxModuleBytes) !=
      module->baseline) {
    throw std::runtime_error("notebook project: module " + module->id +
                             " changed externally; reopen before saving");
  }
  if (source == module->baseline) {
    module->source.swap(published_source);
    return;
  }
  const mode_t mode = member_mode(root.get(), module->path);
  MemberParent parent = open_member_parent(root.get(), module->path);
  StagedMember staged(parent.directory.get(), parent.leaf, source, mode);
  if (read_member(root.get(), module->path, kMaxModuleBytes) !=
      module->baseline) {
    throw std::runtime_error("notebook project: module " + module->id +
                             " changed during save; reopen before saving");
  }
  staged.replace();
  module->source.swap(published_source);
  module->baseline.swap(baseline);
  sync_directory(parent.directory.get());
}

LoadedProjectModule create_project_module(LoadedProject *project,
                                          const ProjectModule &entry,
                                          const std::string &source) {
  if (!project)
    throw std::invalid_argument("notebook project: null project");
  if (source.size() > kMaxModuleBytes) {
    throw std::runtime_error("notebook project: module source exceeds 16 MiB");
  }

  // Keep the API scoped to bundled modules. The general document validator
  // rejects traversal, absolute paths, symlinks-in-name tricks, and malformed
  // IDs; this additional check prevents publishing an arbitrary project
  // member through the create API.
  if (entry.path.rfind("modules/", 0) != 0 ||
      entry.path.substr(entry.path.rfind('/') + 1) != entry.id + ".am") {
    throw std::runtime_error(
        "notebook project: new module path must be modules/<id>.am");
  }

  // Validate the complete candidate before opening or creating any directory.
  // module_manifest_snapshot also rejects a dirty in-memory project view.
  ProjectDocument published_document = module_manifest_snapshot(*project);
  published_document.modules.push_back(entry);
  std::string published_baseline =
      serialize_project_document(published_document);
  // Allocate the return object before any publication. Its source and
  // baseline are both exact caller bytes, including incomplete/non-UTF-8
  // editor buffers.
  LoadedProjectModule published_module{entry.id, entry.path, source, source};

  auto root = open_directory(project->directory);
  ProjectLock lock(root.get());
  if (read_document(root.get()) != project->baseline) {
    throw std::runtime_error(
        "notebook project: project.json changed externally; reopen before "
        "creating a module");
  }

  const ProjectDocument manifest = parse_project_document(project->baseline);
  validate_members(root.get(), manifest);
  MemberParent parent = open_or_create_member_parent(root.get(), entry.path);
  // If open_or_create_member_parent created `modules/` or a nested parent,
  // make the directory entries durable before a manifest can reference the
  // eventual source. This also syncs the root edge to a newly-created
  // modules/ directory.
  sync_directory(root.get());
  require_new_member_target(parent.directory.get(), parent.leaf, entry.path);

  // Stage both pieces before making either visible. In particular, a failed
  // manifest serialization/write leaves the existing manifest untouched.
  StagedMember staged_module(parent.directory.get(), parent.leaf, source);
  StagedMember staged_manifest(root.get(), kDocumentName,
                               published_baseline);

  // Avoid needlessly creating an orphan when a non-cooperating writer changed
  // the manifest while we were staging.
  if (read_document(root.get()) != project->baseline) {
    throw std::runtime_error(
        "notebook project: project.json changed during module creation; "
        "reopen before creating a module");
  }

  bool module_published = false;
  try {
    staged_module.create();
    module_published = true;

    // Ensure the source directory entry is durable before the manifest can
    // point at it. A failure here intentionally leaves an explicit,
    // recoverable unreferenced source file.
    sync_directory(parent.directory.get());

    // The final exact check closes the normal cooperative-writer window. If a
    // non-cooperating writer wins the race, retain the newly-created source;
    // deleting it could destroy an unrelated replacement after a race.
    if (read_document(root.get()) != project->baseline) {
      throw std::runtime_error(
          "notebook project: project.json changed before module manifest "
          "commit");
    }
    validate_members(root.get(), published_document);
    staged_manifest.replace();
  } catch (const std::exception &error) {
    if (module_published) {
      throw std::runtime_error(
          "notebook project: module source was published at " + entry.path +
          " but its manifest was not committed; source remains recoverable "
          "and unreferenced: " + error.what());
    }
    throw;
  }

  // renameat above has committed the manifest. Move the preallocated state
  // into the caller before the directory fsync, because fsync errors report a
  // committed write and the caller must reconcile from these baselines.
  project->baseline.swap(published_baseline);
  project->document = std::move(published_document);
  sync_directory(root.get());
  return published_module;
}

} // namespace amber::notebook
