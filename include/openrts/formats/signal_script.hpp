#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <openrts/core/vfs.hpp>

namespace openrts::formats {

struct SignalConfig;

// The RTS signal script machine (docs/reverse/signals.md, "Compiler, VM and
// evaluation in detail"): 8 float registers, locals, head fields, built-ins.
enum class SignalOp : std::uint8_t {
    mov, call, push, jmp, jz,
    cmpe, cmpne, cmpg, cmpge, cmpl, cmple,       // integer compares
    cmpef, cmpnef, cmpgf, cmpgef, cmplf, cmplef, // float compares
    op_and, op_or, op_xor, op_not, neg, round, ret,
};

enum class SignalOperandKind : std::uint8_t { none, local, head, immediate, reg };

// Head fields by their RTS built-in ids.
enum class SignalHeadField : std::uint8_t { state = 7, draw_state = 8, enabled = 9, signal_mode = 13 };

enum class SignalBuiltin : std::uint8_t {
    none = 0,
    block_state = 1,
    route_set = 2,
    next_sig_lr = 3,
    next_sig_mr = 4,
    def_draw_state = 5,
    sig_feature = 6,
    dist_multi_sig_mr = 10,
    this_sig_mr = 11,
    this_sig_lr = 12,
};

struct SignalOperand {
    SignalOperandKind kind = SignalOperandKind::none;
    std::uint32_t index{}; // local / head field / register
    float value{};         // immediate
};

struct SignalInstruction {
    SignalOp op = SignalOp::ret;
    SignalOperand a;
    SignalOperand b;
    SignalBuiltin builtin = SignalBuiltin::none; // CALL
    std::uint32_t target{};                      // JMP / JZ: instruction index
};

struct SignalScript {
    std::string name;      // lower case: the signal type
    std::string assembly;  // RTS text assembly (what SignalScriptsGenerateAsm writes)
    std::vector<SignalInstruction> code;
    std::vector<std::string> locals;
    // A local may be read before it is written on some path: the result
    // then depends on earlier runs (RTS keeps locals between runs).
    bool reads_stale_local = false;
    std::vector<SignalBuiltin> builtins_used;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

struct SignalScriptFile {
    std::vector<SignalScript> scripts;
    std::vector<std::string> messages; // unknown types, duplicates, compile errors
    [[nodiscard]] const SignalScript* find(std::string_view type) const;
    // The text RTS writes to <file>.asm for these scripts (UTF-8 here).
    [[nodiscard]] std::string assembly() const;
};

// Compiles a sigscr file (ANSI bytes, as RTS reads it). With a config only
// scripts of its signal types are compiled, as RTS does; a second script for
// a type is reported and skipped.
[[nodiscard]] SignalScriptFile compile_signal_scripts(std::string_view bytes, const SignalConfig* config = nullptr);
// Every ScriptFile of the config, in order (paths relative to the route).
[[nodiscard]] SignalScriptFile load_signal_scripts(const core::Vfs& vfs, const core::VirtualPath& route,
                                                   const SignalConfig& config);

[[nodiscard]] const char* to_string(SignalOp op) noexcept;

} // namespace openrts::formats
