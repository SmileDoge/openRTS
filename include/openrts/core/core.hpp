#pragma once

#include <string>
#include <string_view>

namespace openrts::core {

[[nodiscard]] std::string_view module_name() noexcept;
// openRTS's version, CMake's project version ("0.4.2"): the update check
// compares it with the releases' tags.
[[nodiscard]] std::string_view version() noexcept;
// What kind of build this is, from CMake's OPENRTS_BUILD_LABEL: "nightly
// f1a6b90" for CI's nightly build (a push to master, its commit), empty
// for a version release or a local build.
[[nodiscard]] std::string_view build_label() noexcept;
// The version with the label for the windows' titles and the menu:
// "0.6.2 (nightly f1a6b90)", or "0.6.2".
[[nodiscard]] std::string version_text();

} // namespace openrts::core
