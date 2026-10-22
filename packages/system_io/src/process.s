package sdk.process

export env, executable, umask, trap_termination, termination_requested, limits, uid, stdin_text

native def env(name as Str) -> Str from "spos_env"
native def executable() -> Str from "spos_executable"
native def umask(permissions as Int) from "spos_umask"
native def trap_termination() from "spos_trap_termination"
native def termination_requested() -> Bool from "spos_termination_requested"
# Null leaves a limit unchanged; address_space is enforced where supported.
native def _limits(core, files, address_space) from "spos_limits"
native def uid() -> Int from "spos_uid"
native def _stdin_text(limit as Int) -> Str from "spos_stdin_text" blocking

def stdin_text(limit: 65536): _stdin_text(limit)

def limits(core: null, files: null, address_space: null):
  _limits(core, files, address_space)
