#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

struct lua_State;

// Luau, the scripting language of openRTS's own modules (docs/scripting.md).
namespace openrts::script {

// A chunk compiled to Luau bytecode, shared by every VM that runs it.
struct Chunk {
    std::string name;     // the file, for messages ("vr292.lua:12: ...")
    std::string bytecode;
};

// Compiles `source`; nullopt and the message ("name:line: what") on an error.
[[nodiscard]] std::optional<Chunk> compile(std::string_view source, std::string name, std::string& error);

// One sandboxed Luau state running one chunk:
// - Luau's own safe libraries only (no files, no processes; `os` has only
//   the clock), their globals frozen (luaL_sandbox);
// - the script's globals in a table of its own (luaL_sandboxthread);
// - every run and call under a time limit: a script that loops for ever is
//   stopped with an error instead of hanging the game.
class Vm {
public:
    Vm();
    ~Vm();
    Vm(const Vm&) = delete;
    Vm& operator=(const Vm&) = delete;

    [[nodiscard]] bool ok() const noexcept;
    // Runs the chunk (it defines its functions); false and the message.
    bool run(const Chunk& chunk, std::string& error);
    // The script's thread: push a function and its arguments with lua.h,
    // then call().
    [[nodiscard]] lua_State* thread() const noexcept;
    // Calls the function under the arguments pushed on thread(); false and
    // the message on an error or a timeout (nothing left on the stack then).
    bool call(int arguments, int results, std::string& error);
    void set_time_limit(std::chrono::milliseconds limit) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openrts::script
