#include "ui_sound.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

namespace openrts::game::menu {

void UiSound::load(const std::filesystem::path& folder) {
    select_ = folder / "select.wav";
    click_ = folder / "click.wav";
}

void UiSound::play(const std::filesystem::path& file) const {
#ifdef _WIN32
    if (!file.empty()) PlaySoundW(file.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
#else
    (void)file;
#endif
}

void UiSound::tick() {
    const auto* context = ImGui::GetCurrentContext();
    const auto hovered = context ? static_cast<unsigned int>(context->HoveredId) : 0u;
    const bool any_hovered = ImGui::IsAnyItemHovered();
    if ((any_hovered && !any_hovered_) || (hovered != 0 && hovered != hovered_)) play(select_);
    any_hovered_ = any_hovered;
    hovered_ = hovered;
    if (any_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) play(click_);
}

} // namespace openrts::game::menu
