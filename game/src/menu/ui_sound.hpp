#pragma once

#include <filesystem>

namespace openrts::game::menu {

// The menu's own two one-shots. Windows' PlaySound is used on purpose: the
// menu must stay audible even before the game's audio engine has a voice free,
// and the files are tiny. The clips sit beside the program in the menu folder.
class UiSound final {
public:
    void load(const std::filesystem::path& folder);
    void tick();

private:
    void play(const std::filesystem::path& file) const;

    std::filesystem::path select_;
    std::filesystem::path click_;
    bool any_hovered_ = false;
    unsigned int hovered_ = 0;
};

} // namespace openrts::game::menu
