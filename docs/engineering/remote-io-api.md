# IO primitives for persistent Sputnik services

The remote notebook agent requires durable snapshots, a single process owning
each state directory, and local IPC whose lifetime is independent of HTTP.
The public API additions below were approved on 2026-10-14; `fs.sync` was
approved on 2026-10-15.

`fs.atomic_write_text(path, text, durable: true)` and
`fs.atomic_write_bytes(path, bytes, durable: true)` create a temporary regular
file in the destination directory with permissions 0600. They write the whole
value, sync the file, replace the destination with `rename`, then sync its
parent directory. Existing destination symlinks are replaced, not followed.
The parent must exist. With `durable: false`, replacement is still atomic but
both sync operations are omitted. Failures after rename mean that the new
contents are visible but durability was not confirmed; they raise an IO error.
The operations require `fs.write` for the destination and reject replay without
a provider capable of preserving their semantics.

`fs.sync(path)` accepts a `Str` or `fs.Path` naming an existing regular file or
directory and returns `null` after `fsync` and descriptor close succeed. It never
creates or truncates its target. Empty paths and embedded NUL are rejected.
The final path component must not be a symlink; intermediate components follow
normal OS path resolution. Special files are rejected without waiting for a
FIFO peer. It accepts no keywords or block. The call requires `fs.write` and
uses the existing external write/replay policy, cancellation checks and IO
accounting. OS failures are raised, including filesystems that do not support
directory sync. VM and direct native execution share the same OS helper.

Syncing a file does not sync its containing directory. To publish a newly
created tree, sync files and then directories in postorder, rename the sealed
root into place, and sync the parent of that rename before committing a durable
metadata reference. The remote core follows this order, including its file
manifest. Guarantees depend on the filesystem and storage honoring `fsync`;
these tests do not simulate a physical power loss.

`fs.lock(path, exclusive: true, nonblock: true)` opens or creates a regular
0600 lock file without following a symlink and holds a POSIX advisory lock on
its descriptor. The returned resource has `close!` and `closed?`; a call-site
block closes it on return and on unwinding. Contention in nonblocking mode raises
`ResourceBusyError`. Blocking mode checks cancellation between bounded retries.
Closing the descriptor or process exit releases the lock. The lock file remains;
deleting it while another process owns the lock would defeat mutual exclusion.
Acquisition requires `fs.write`. Lock resources are confined to their owning
strand.

`fs.entries(path)` returns sorted `fs.Path` values for immediate children,
including dotfiles and symlinks. It performs no recursion or symlink traversal.
The caller decides which members to package, using `fs.metadata` (lstat) before
descending. Enumeration requires `fs.metadata` and follows the existing replay
policy for external IO.

`net.unix.listen(path, backlog: 128, isolation: :checked)` and
`net.unix.connect(path, timeout: null, isolation: :checked)` use filesystem Unix
stream sockets. Listeners support `accept!`, `try_accept!`, `local_path`,
`close!`, and `closed?`; streams support existing byte IO, `local_path`,
`remote_path`, shutdown, and close. Both constructors support scoped blocks.
The implementation uses the existing reactor, deadlines, cancellation and
resource confinement machinery. Paths must fit `sockaddr_un` and cannot contain
NUL. Abstract namespace sockets are outside this API.

Listen never deletes an existing path. It creates a socket with permissions
0600 and removes it on close only if the path still names the same socket inode.
Connect requires `net.connect=unix:PATH`; listen requires `net.listen=unix:PATH`
and `fs.write=PATH`. The containing directory should be private when the IPC
belongs to one user. Replay is rejected through the existing external IO policy.

`Json.parse(text, duplicate_keys: :reject)` rejects repeated object keys at every
nesting depth, after decoding their escapes. `:last` preserves the default
behavior. The option works alongside `map: StrictMap` and does not change the
number or string representation. Applications that preserve source documents
should store their original text rather than regenerate it from parsed values.

`tests/remote_io_api.s` exercises replacement, file/directory sync, missing paths,
argument errors, nonblocking rejection of FIFOs, lock contention and scoped release,
symlink handling, JSON duplicates, a real Unix socket exchange, and safe listener
cleanup. It also checks buffered line reads, their Str result, and stream write
shutdown. Both VM and `--require-full-native` executables pass on macOS and Linux; native
coverage reports zero fallback bodies and no VM bridge. `make test-remote-io`
also checks that all six entry operations fail without capability grants. On
Linux, syncing a regular procfs pseudo-file also verifies propagation of an
unsupported `fsync` error.
