#include "key_layout.hpp"

#include "ui_text.hpp"

namespace openrts::viewer {
namespace {

KeyLayout current = KeyLayout::openrts;
bool hints = true;

} // namespace

KeyLayout key_layout() noexcept { return current; }

void set_key_layout(const KeyLayout layout) noexcept { current = layout; }

bool key_hints() noexcept { return hints; }

void set_key_hints(const bool shown) noexcept { hints = shown; }

const char* window_key_name() { return current == KeyLayout::rts ? "Num0" : tr("'-'"); }

const char* head_out_key_name() { return current == KeyLayout::rts ? "Num1" : tr("'='"); }

const char* car_menu_key_name() { return current == KeyLayout::rts ? "Num3" : tr("'`'"); }

} // namespace openrts::viewer
