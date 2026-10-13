#pragma once

#include "bytecode/format.h"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace sputnik::bytecode {

// A syntactic body summary, never a method-resolution or call-shape guard.
// Callers must first resolve the live method and validate its arguments. No
// handlers, captures, branches, mutation or nonlocal returns are summarized.
struct TrivialBody {
  enum class Kind { None, Argument, Constant, Null, Bool, Self, Ivar, IvarSend0 };
  Kind kind = Kind::None;
  std::uint32_t index = 0;
  std::uint32_t selector = 0;
  std::uint32_t call_flags = 0;
};

inline TrivialBody trivial_body(const BcModule &module, const BcCode &code,
                                std::size_t param_count) {
  using Kind = TrivialBody::Kind;
  if (code.kind != CodeKind::Method || code.flags != 0U ||
      !code.capture_layout.empty() || !code.handler_table.empty() ||
      code.instructions.size() < 2U || code.instructions.size() > 5U)
    return {};
  const auto &ins = code.instructions;
  const auto &ret = ins.back();
  if (ret.opcode != Opcode::Return || ret.operands.size() != 1U || ret.operands[0].value < 0 ||
      static_cast<std::uint64_t>(ret.operands[0].value) >= code.reg_count)
    return {};
  const auto returned = static_cast<std::uint32_t>(ret.operands[0].value);
  // A bodiless method closes its locals before synthesizing the implicit
  // Null return; an explicit `null` expression loads it before CLOSE_UPVALUES.
  if (ins.size() == 3U && ins[0].opcode == Opcode::CloseUpvalues &&
      ins[0].operands.size() == 1U && ins[0].operands[0].value == 0 &&
      ins[1].opcode == Opcode::LoadNull && ins[1].operands.size() == 1U &&
      ins[1].operands[0].value == returned)
    return {Kind::Null};
  const auto &close = ins[ins.size() - 2U];
  if (close.opcode != Opcode::CloseUpvalues || close.operands.size() != 1U ||
      close.operands[0].value != 0)
    return {};
  if (ins.size() == 2U)
    return returned < param_count ? TrivialBody{Kind::Argument, returned}
                                 : TrivialBody{};
  const auto &load = ins[0];
  if (load.operands.empty() || load.operands[0].value < 0 ||
      static_cast<std::uint64_t>(load.operands[0].value) >= code.reg_count)
    return {};
  if (ins.size() == 3U && returned == load.operands[0].value) {
    if (load.opcode == Opcode::LoadSelf && load.operands.size() == 1U)
      return {Kind::Self};
    if (load.opcode == Opcode::LoadNull && load.operands.size() == 1U)
      return {Kind::Null};
    if (load.opcode == Opcode::LoadBool && load.operands.size() == 2U)
      return {Kind::Bool, load.operands[1].value != 0 ? 1U : 0U};
    if (load.opcode == Opcode::LoadK && load.operands.size() == 2U &&
        load.operands[1].value >= 0 &&
        static_cast<std::uint64_t>(load.operands[1].value) < module.const_pool.size()) {
      const auto id = static_cast<std::uint32_t>(load.operands[1].value);
      switch (module.const_pool[id].kind) {
      case ConstantKind::Null:
      case ConstantKind::Bool:
      case ConstantKind::Integer:
      case ConstantKind::Float:
        return {Kind::Constant, id};
      default: break;
      }
    }
    return {};
  }
  const auto &ivar = ins[1];
  if (load.opcode != Opcode::LoadSelf || load.operands.size() != 1U ||
      ivar.opcode != Opcode::LoadIvar || ivar.operands.size() != 4U ||
      ivar.operands[1].value != load.operands[0].value ||
      ivar.operands[0].value < 0 ||
      static_cast<std::uint64_t>(ivar.operands[0].value) >= code.reg_count ||
      ivar.operands[2].value < 0 ||
      static_cast<std::uint64_t>(ivar.operands[2].value) >= module.symbols.size())
    return {};
  const auto symbol = static_cast<std::uint32_t>(ivar.operands[2].value);
  if (ins.size() == 4U && returned == ivar.operands[0].value)
    return {Kind::Ivar, symbol};
  if (ins.size() != 5U) return {};
  const auto &send = ins[2];
  if (send.opcode != Opcode::Send || send.operands.size() != 7U ||
      send.operands[0].value != returned ||
      send.operands[1].value != ivar.operands[0].value ||
      send.operands[2].value < 0 ||
      static_cast<std::uint64_t>(send.operands[2].value) >= module.symbols.size() ||
      send.operands[3].value != 0 || send.operands[4].value != 0 ||
      (send.operands[5].value != -1 &&
       send.operands[5].value != std::numeric_limits<std::uint32_t>::max()) ||
      send.operands[6].value < 0 ||
      static_cast<std::uint64_t>(send.operands[6].value) >= code.call_site_table.size())
    return {};
  return {Kind::IvarSend0, symbol,
          static_cast<std::uint32_t>(send.operands[2].value),
          code.call_site_table[send.operands[6].value].flags};
}

// An empty init can bind instance fields without constructing a callee frame.
// Validate every assignment before applying any, including the local/register
// mapping used by the ordinary binder. Class variables retain the full path.
inline bool positional_ivar_auto_assigns(const BcModule &module,
                                         const BcMethod &method,
                                         const BcCode &code) {
  for (const auto &assign : method.auto_assign_desc) {
    if (assign.target_name_str_id >= module.strings.size()) return false;
    const auto &target = module.strings[assign.target_name_str_id];
    if (target.size() < 2U || target[0] != '@' || target[1] == '@') return false;
    std::size_t index = 0;
    for (; index < method.params.size(); ++index) {
      if (method.params[index].local_name_str_id == assign.local_name_str_id)
        break;
    }
    if (index == method.params.size()) return false;
    bool mapped = false;
    for (const auto &local : code.local_layout) {
      if (local.name_str_id == assign.local_name_str_id) {
        mapped = local.slot == index;
        break;
      }
    }
    if (!mapped) return false;
  }
  return true;
}

} // namespace sputnik::bytecode
