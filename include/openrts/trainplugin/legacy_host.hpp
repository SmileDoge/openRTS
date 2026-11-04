#pragma once

// The game side of openrts-dllhost32.exe: starts the 32-bit host process for
// one consist and exchanges messages with it over shared memory
// (include/openrts/dllhost/protocol.hpp, docs/train-dll.md). Windows only;
// elsewhere start() fails.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <openrts/dllhost/protocol.hpp>

namespace openrts::trainplugin {

class LegacyHost {
public:
    // Starts the host (`executable`, or openrts-dllhost32.exe next to the
    // running program) and checks the protocol. nullptr with `error` set
    // when it cannot run.
    [[nodiscard]] static std::unique_ptr<LegacyHost> start(std::string* error,
                                                           const std::filesystem::path& executable = {});
    [[nodiscard]] static std::filesystem::path default_executable();
    ~LegacyHost();
    LegacyHost(const LegacyHost&) = delete;
    LegacyHost& operator=(const LegacyHost&) = delete;

    bool setup(const dllhost::SetupRequest& request, std::vector<dllhost::ModuleReport>& modules);
    bool enter(const dllhost::EnterRequest& request, dllhost::StepReply& reply);
    bool reconsist(const dllhost::ReconsistRequest& request, dllhost::StepReply& reply);
    bool step(const dllhost::StepRequest& request, dllhost::StepReply& reply);
    bool can_switch(const dllhost::CanSwitchRequest& request, bool& allowed);
    bool drag_switch(const dllhost::DragSwitchRequest& request, bool& moved, dllhost::SwitchState& state);

    // False after the host crashed, hung or refused the protocol.
    [[nodiscard]] bool alive() const noexcept { return alive_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    // Lines the host logged (DLL failures, warnings), UTF-8; cleared.
    [[nodiscard]] std::string take_messages();
    // Milliseconds a step may take before the host counts as hung.
    void set_step_timeout(std::uint32_t ms) noexcept { step_timeout_ = ms; }

private:
    LegacyHost() = default;
    template <class Write, class Read>
    bool call(dllhost::Command command, Write&& write, Read&& read, std::uint32_t timeout_ms);
    void kill(const std::string& reason);

    void* mapping_ = nullptr;
    std::uint8_t* view_ = nullptr;
    void* request_ = nullptr;
    void* reply_ = nullptr;
    void* process_ = nullptr;
    std::uint32_t sequence_ = 0;
    std::uint32_t step_timeout_ = 3000;
    bool alive_ = false;
    std::string error_;
    std::string messages_;
};

[[nodiscard]] std::u16string utf8_to_utf16(std::string_view utf8);
[[nodiscard]] std::string utf16_to_utf8(std::u16string_view utf16);

} // namespace openrts::trainplugin
