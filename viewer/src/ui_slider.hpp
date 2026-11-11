#pragma once

#include <algorithm>
#include <cstring>

#include <imgui.h>

namespace openrts::viewer {

// A settings slider with a box beside it to type the value into (the field
// of view, a distance to the metre): what is typed is taken on Enter or when
// the box loses the focus, kept within the slider's range. The two together
// take the width the slider alone would; `label` after them as ImGui puts a
// slider's label (nothing for a "##id"). True when the value changed.
inline bool slider_with_input(const char* label, float* value, const float min, const float max, const char* format,
                              const char* typed_format, const ImGuiSliderFlags flags = 0) {
    const auto& style = ImGui::GetStyle();
    const float box = ImGui::CalcTextSize("000000.0").x + style.FramePadding.x * 2.0f;
    const float total = ImGui::CalcItemWidth();
    ImGui::PushID(label);
    ImGui::SetNextItemWidth((std::max)(total - box - style.ItemInnerSpacing.x, 40.0f));
    bool changed = ImGui::SliderFloat("##slider", value, min, max, format, flags);
    ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
    ImGui::SetNextItemWidth(box);
    float typed = *value;
    ImGui::InputFloat("##typed", &typed, 0.0f, 0.0f, typed_format);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        *value = std::clamp(typed, min, max);
        changed = true;
    }
    const char* hidden = std::strstr(label, "##");
    const char* end = hidden ? hidden : label + std::strlen(label);
    if (end != label) {
        ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
        ImGui::TextUnformatted(label, end);
    }
    ImGui::PopID();
    return changed;
}

} // namespace openrts::viewer
