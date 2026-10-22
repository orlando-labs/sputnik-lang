package system_io.selftest

import sdk.fs as safe_fs
import sdk.digest as digest
import sdk.process as process
import sdk.pkg as pkg

export main

def check(value, message):
  raise ValueError(message) unless value

def rejects(&block):
  rejected = false
  try:
    block()
  rescue:
    rejected = true
  rejected

def check_rejected(message, &block):
  check(rejects(&block), message)

def main():
  parser = ArgParser()
  parser.pos("directory")
  args = parser.parse_or_raise()
  path = args[:directory]
  directory = safe_fs.Root(path)
  try:
    check_rejected("parent traversal"): directory.open("../outside/secret")
    check_rejected("absolute member path"): directory.open("/etc/passwd")
    check_rejected("symlink ancestor"): directory.open("linked/secret")
    check_rejected("symlink leaf"): directory.open("symlink")
    check_rejected("symlink write"): directory.open("symlink", mode: :write, create: true)
    check_rejected("symlink root"): safe_fs.Root(path + "/linked")
    check_rejected("non-regular file"): directory.open("folder")
    artifact = directory.open("archive.sputnikpkg")
    try:
      check(artifact.read(max_bytes: 2).hex() == "1f8b", "gzip framing")
      facts = pkg.inspect(artifact)
      check(facts[:distribution][:name] == "tests/file", "verified package identity")
      check(facts[:manifest][:module_representations][0][1] == "source", "verified module representation")
      check(facts[:manifest][:target][:cpu_features] == [] and facts[:manifest][:target][:compute_backends] == [], "complete target requirements")
      check(facts[:compatibility][:bytecode_versions] == [] and facts[:compatibility][:sputnik_ext_abi] == null, "source compatibility facts")
      check(not facts[:manifest][:native_payload], "source is not native payload")
      check(pkg.inspect(artifact, required_profile: "standard")[:distribution][:name] == "tests/file", "admitted profile inspection")
      check_rejected("archive cannot select another profile"): pkg.inspect(artifact, required_profile: "native-large")
      # Inspection must restore the caller's existing offset (2), not rewind.
      check(artifact.read(max_bytes: 1).hex() == "08", "inspection preserves file offset")
    ensure:
      artifact.close()
    artifact = directory.open("archive-bytecode.sputnikpkg")
    try:
      artifact.read(max_bytes: 2)
      facts = pkg.inspect(artifact)
      check(facts[:manifest][:module_representations][0][1] == "bytecode", "verified bytecode representation")
      check(facts[:compatibility][:bytecode_versions].count == 1 and facts[:compatibility][:profiles].count == 1 and facts[:compatibility][:bytecode_abi_hashes].count == 1, "verified bytecode compatibility facts")
      check(artifact.read(max_bytes: 1).hex() == "08", "build metadata inspection preserves file offset")
    ensure:
      artifact.close()
    file = directory.open("new", mode: :read_write, create: true, exclusive: true)
    try:
      hash = digest.SHA256()
      try:
        hash.update("abc".bytes)
        check(hash.finish() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "incremental SHA-256")
        check_rejected("finalized digest cannot be reused"): hash.update("more".bytes)
      ensure:
        hash.destroy!()
      file.write("abc".bytes)
      file.sync()
      check(file.sha256() == Digest.sha256("abc".bytes).hex(), "descriptor digest")
      check(file.metadata()[:permissions] == 384, "private file permissions")
      check_rejected("exclusive create"): directory.open("new", mode: :write, create: true, exclusive: true)
      check_rejected("malformed archive rejected"): pkg.inspect(file)
      file.truncate(2)
      check(file.metadata()[:size] == 2, "truncate metadata")
    ensure:
      file.close()
    check(directory.link("new", "hardlink"), "new hard link")
    check(not directory.link("new", "hardlink"), "hard link cannot replace")
    directory.rename("hardlink", "renamed")
    directory.chmod("renamed", 256)
    check(directory.metadata("renamed")[:permissions] == 256, "chmod")
    check(directory.metadata("symlink")[:symlink], "nofollow metadata")
    check(directory.entries("").contains?("renamed"), "descriptor directory listing")
    check(directory.metadata("missing") == null, "missing metadata")
    lock = directory.lock("test.lock", nonblock: true)
    try:
      check_rejected("exclusive process lock"): directory.lock("test.lock", nonblock: true)
    ensure:
      lock.close()
    directory.remove("renamed")
    directory.remove("new")
    directory.sync()
    check(process.executable().starts_with?("/"), "absolute executable")
    check(process.uid() >= 0, "effective uid")
    check(process.env("SYSTEM_IO_SELFTEST") == "working", "environment")
    true
  ensure:
    directory.close()
