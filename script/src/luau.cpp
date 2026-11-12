#include <openrts/script/luau.hpp>

#include <cstdlib>

#include <lua.h>
#include <luacode.h>
#include <lualib.h>

namespace openrts::script {
namespace {

using Clock = std::chrono::steady_clock;

struct Limit {
    Clock::time_point deadline = Clock::time_point::max();
};

// Luau calls this now and then while code runs (gc < 0) and at GC steps.
void interrupt(lua_State* state, const int gc) {
    if (gc >= 0) return;
    auto* limit = static_cast<Limit*>(lua_callbacks(state)->userdata);
    if (!limit || Clock::now() <= limit->deadline) return;
    limit->deadline = Clock::time_point::max(); // once: the error unwinds the call
    luaL_error(state, "the script ran past its time limit");
}

} // namespace

std::optional<Chunk> compile(const std::string_view source, std::string name, std::string& error) {
    lua_CompileOptions options{};
    options.optimizationLevel = 1;
    options.debugLevel = 1; // line numbers in messages
    std::size_t size = 0;
    char* bytes = luau_compile(source.data(), source.size(), &options, &size);
    if (!bytes) {
        error = name + ": the Luau compiler failed";
        return std::nullopt;
    }
    std::string code(bytes, size);
    std::free(bytes);
    // An error comes back as a zero byte and the message (":line: what").
    if (code.empty() || code[0] == 0) {
        error = name + (code.size() > 1 ? code.substr(1) : std::string(": cannot be compiled"));
        return std::nullopt;
    }
    return Chunk{.name = std::move(name), .bytecode = std::move(code)};
}

struct Vm::Impl {
    lua_State* main = nullptr;
    lua_State* thread = nullptr;
    int thread_ref = 0;
    Limit limit;
    std::chrono::milliseconds time_limit{200};
};

Vm::Vm() : impl_(std::make_unique<Impl>()) {
    auto& i = *impl_;
    i.main = luaL_newstate();
    if (!i.main) return;
    luaL_openlibs(i.main);
    luaL_sandbox(i.main);
    lua_callbacks(i.main)->interrupt = interrupt;
    lua_callbacks(i.main)->userdata = &i.limit;
    i.thread = lua_newthread(i.main);
    i.thread_ref = lua_ref(i.main, -1);
    lua_pop(i.main, 1);
    luaL_sandboxthread(i.thread);
}

Vm::~Vm() {
    if (impl_ && impl_->main) lua_close(impl_->main);
}

bool Vm::ok() const noexcept { return impl_->main && impl_->thread; }

lua_State* Vm::thread() const noexcept { return impl_->thread; }

void Vm::set_time_limit(const std::chrono::milliseconds limit) noexcept { impl_->time_limit = limit; }

bool Vm::run(const Chunk& chunk, std::string& error) {
    if (!ok()) {
        error = chunk.name + ": no Luau state";
        return false;
    }
    const std::string name = "=" + chunk.name;
    if (luau_load(impl_->thread, name.c_str(), chunk.bytecode.data(), chunk.bytecode.size(), 0) != 0) {
        const char* message = lua_tostring(impl_->thread, -1);
        error = message ? message : chunk.name + ": cannot be loaded";
        lua_pop(impl_->thread, 1);
        return false;
    }
    return call(0, 0, error);
}

bool Vm::call(const int arguments, const int results, std::string& error) {
    auto& i = *impl_;
    i.limit.deadline = Clock::now() + i.time_limit;
    const int status = lua_pcall(i.thread, arguments, results, 0);
    i.limit.deadline = Clock::time_point::max();
    if (status == LUA_OK) return true;
    const char* message = lua_tostring(i.thread, -1);
    error = message ? message : "a Luau error";
    lua_pop(i.thread, 1);
    return false;
}

} // namespace openrts::script
