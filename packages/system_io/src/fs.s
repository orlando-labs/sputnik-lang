package sdk.fs

export Root, File, absolute

native def absolute(path as Str) -> Str from "spfs_absolute"

# Root resolves every directory component without following symbolic links.
# All member paths are relative; "..", "." and absolute paths are rejected.
native class RawRoot from "sdk.fs.Root" owned:
  def init(path as Str, create as Bool, permissions as Int) from "spfs_root_new" blocking
  def destroy!() from "spfs_root_free"
  def _open(path as Str, mode as Str, create as Bool, exclusive as Bool, truncate as Bool, permissions as Int) from "spfs_root_open" blocking
  def _mkdir(path as Str, parents as Bool, permissions as Int) from "spfs_root_mkdir" blocking
  def _metadata(path as Str) from "spfs_root_metadata" blocking
  def _entries(path as Str, limit as Int) -> List from "spfs_root_entries" blocking
  def _remove(path as Str, missing_ok as Bool) from "spfs_root_remove" blocking
  def _rename(source as Str, target as Str) from "spfs_root_rename" blocking
  def _link(source as Str, target as Str) -> Bool from "spfs_root_link" blocking
  def _sync(path as Str) from "spfs_root_sync" blocking
  def _chmod(path as Str, permissions as Int) from "spfs_root_chmod" blocking
  def _lock(path as Str, nonblock as Bool) from "spfs_root_lock" blocking

native class RawFile from "sdk.fs.File" owned:
  def destroy!() from "spfs_file_free"
  def write(data as Bytes) -> Int from "spfs_file_write" blocking
  def read(max_bytes as Int) from "spfs_file_read" blocking
  def sync() from "spfs_file_sync" blocking
  def truncate(size as Int) from "spfs_file_truncate" blocking
  def _metadata() -> List from "spfs_file_metadata"
  def _sha256() -> Str from "spfs_file_sha256" blocking

def parse_metadata(row):
  return null unless row
  {size: row[0], mtime: row[1], permissions: row[2], uid: row[3], file: row[4], directory: row[5], symlink: row[6]}

class File:
  attr raw
  def init(@raw)
  def close(): @raw.destroy!()
  def write(data): @raw.write(data)
  def read(max_bytes: 65536): @raw.read(max_bytes)
  def sync(): @raw.sync()
  def truncate(size): @raw.truncate(size)
  def metadata(): parse_metadata(@raw._metadata())
  def sha256(): @raw._sha256()

class Root:
  def init(path, create: false, permissions: 448):
    @raw = RawRoot(path, create, permissions)
  def close(): @raw.destroy!()
  def open(path, mode: :read, create: false, exclusive: false, truncate: false, permissions: 384):
    File(@raw._open(path, mode.to_str, create, exclusive, truncate, permissions))
  def mkdir(path, parents: false, permissions: 448): @raw._mkdir(path, parents, permissions)
  def metadata(path): parse_metadata(@raw._metadata(path))
  def entries(path, limit: 1000): @raw._entries(path, limit)
  def remove(path, missing_ok: true): @raw._remove(path, missing_ok)
  def rename(source, target): @raw._rename(source, target)
  # Returns false if target already exists. Never replaces a directory entry.
  def link(source, target): @raw._link(source, target)
  def sync(path = ""): @raw._sync(path)
  def chmod(path, permissions): @raw._chmod(path, permissions)
  def lock(path, nonblock: false): File(@raw._lock(path, nonblock))
