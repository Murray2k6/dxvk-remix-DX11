#pragma once

#include <imgui\imgui.h>
#include <imgui\imgui_internal.h>
#include "..\rtx_render\rtx_option.h"
#include "..\util\util_string.h"
#include "..\util\util_vector.h"
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

namespace RemixGui {
  IMGUI_API bool CheckRtxOptionPopups(dxvk::RtxOptionImpl* impl,
                                      std::optional<XXH64_hash_t> hash,
                                      std::function<void()> onApplyAction);

  // Compatibility shim for older option widgets.
  // The reset/status row decorator used to rewrite ImGui work rects, clip rects,
  // draw channels, and item hitboxes around every option. That made some
  // settings visually present but unable to receive or commit clicks.
  template <typename T>
  struct RtxOptionUxWrapper {
    explicit RtxOptionUxWrapper(dxvk::RtxOption<T>*) {}
    ~RtxOptionUxWrapper() = default;
    RtxOptionUxWrapper(const RtxOptionUxWrapper&) = delete;
    RtxOptionUxWrapper& operator=(const RtxOptionUxWrapper&) = delete;
  };

  bool SliderFloat(const char* label, float* v, float v_min, float v_max, const char* format = "%.3f", ImGuiSliderFlags flags = 0, float overlayAlpha = 0.8f);
  bool SliderFloat2(const char* label, float v[2], float v_min, float v_max, const char* format = "%.3f", ImGuiSliderFlags flags = 0, float overlayAlpha = 0.8f);
  bool SliderFloat3(const char* label, float v[3], float v_min, float v_max, const char* format = "%.3f", ImGuiSliderFlags flags = 0, float overlayAlpha = 0.8f);
  bool SliderFloat4(const char* label, float v[4], float v_min, float v_max, const char* format = "%.3f", ImGuiSliderFlags flags = 0, float overlayAlpha = 0.8f);

  bool SliderInt(const char* label, int* v, int v_min, int v_max, const char* format = "%d", ImGuiSliderFlags flags = 0, float overlayAlpha = 0.8f);
  bool SliderInt2(const char* label, int v[2], int v_min, int v_max, const char* format = "%d", ImGuiSliderFlags flags = 0, float overlayAlpha = 0.8f);
  bool SliderInt3(const char* label, int v[3], int v_min, int v_max, const char* format = "%d", ImGuiSliderFlags flags = 0, float overlayAlpha = 0.8f);
  bool SliderInt4(const char* label, int v[4], int v_min, int v_max, const char* format = "%d", ImGuiSliderFlags flags = 0, float overlayAlpha = 0.8f);
  
  bool Checkbox(const char* label, bool* v, float boxScale = .9f);

  bool InputFloat(const char* label, float* v, float step = 0.0f, float step_fast = 0.0f, const char* format = "%.3f", ImGuiInputTextFlags flags = 0);
  bool InputFloat2(const char* label, float v[2], const char* format = "%.3f", ImGuiInputTextFlags flags = 0);
  bool InputFloat3(const char* label, float v[3], const char* format = "%.3f", ImGuiInputTextFlags flags = 0);
  bool InputFloat4(const char* label, float v[4], const char* format = "%.3f", ImGuiInputTextFlags flags = 0);
  bool InputInt(const char* label, int* v, int step = 1, int step_fast = 100, ImGuiInputTextFlags flags = 0);
  bool InputInt2(const char* label, int v[2], ImGuiInputTextFlags flags = 0);
  bool InputInt3(const char* label, int v[3], ImGuiInputTextFlags flags = 0);
  bool InputInt4(const char* label, int v[4], ImGuiInputTextFlags flags = 0);

  bool InputText(const char* label, char* buf, size_t buf_size, ImGuiInputTextFlags flags = 0, ImGuiInputTextCallback callback = NULL, void* user_data = NULL);

  bool DragFloat(const char* label, float* v, float v_speed = 1.0f, float v_min = 0.0f, float v_max = 0.0f, const char* format = "%.3f", ImGuiSliderFlags flags = 0);     // If v_min >= v_max we have no bound
  bool DragFloat2(const char* label, float v[2], float v_speed = 1.0f, float v_min = 0.0f, float v_max = 0.0f, const char* format = "%.3f", ImGuiSliderFlags flags = 0);
  bool DragFloat3(const char* label, float v[3], float v_speed = 1.0f, float v_min = 0.0f, float v_max = 0.0f, const char* format = "%.3f", ImGuiSliderFlags flags = 0);
  bool DragFloat4(const char* label, float v[4], float v_speed = 1.0f, float v_min = 0.0f, float v_max = 0.0f, const char* format = "%.3f", ImGuiSliderFlags flags = 0);
  bool DragInt(const char* label, int* v, float v_speed = 1.0f, int v_min = 0, int v_max = 0, const char* format = "%d", ImGuiSliderFlags flags = 0);  // If v_min >= v_max we have no bound
  bool DragInt2(const char* label, int v[2], float v_speed = 1.0f, int v_min = 0, int v_max = 0, const char* format = "%d", ImGuiSliderFlags flags = 0);
  bool DragInt3(const char* label, int v[3], float v_speed = 1.0f, int v_min = 0, int v_max = 0, const char* format = "%d", ImGuiSliderFlags flags = 0);
  bool DragInt4(const char* label, int v[4], float v_speed = 1.0f, int v_min = 0, int v_max = 0, const char* format = "%d", ImGuiSliderFlags flags = 0);
  bool DragIntRange2(const char* label, int* v_current_min, int* v_current_max, float v_speed = 1.0f, int v_min = 0, int v_max = 0, const char* format = "%d", const char* format_max = NULL, ImGuiSliderFlags flags = 0);
  bool OptionalDragFloat(const char* label, float enabledValue, float defaultValue, float* v, float boxScale = .9f, float vSpeed = 1.0f, float vMin = 0.0f, float vMax = 0.0f, const char* format = "%.3f", ImGuiSliderFlags flags = 0);
  
  bool Combo(const char* label, int* current_item, const char* const items[], int items_count, int popup_max_height_in_items = -1);
  bool Combo(const char* label, int* current_item, const char* items_separated_by_zeros, int popup_max_height_in_items = -1);
  bool Combo(const char* label, int* current_item, bool (*itemsGetter)(void*, int, const char**, const char**), void* data, int items_count, int popup_max_height_in_items = -1);

  bool CollapsingHeader(const char* label, ImGuiTreeNodeFlags flags = 0);

  void Separator();

  bool ColorEdit3(const char* label, float col[3], ImGuiColorEditFlags flags = 0);
  bool ColorEdit4(const char* label, float col[4], ImGuiColorEditFlags flags = 0);
  bool ColorPicker3(const char* label, float col[3], ImGuiColorEditFlags flags = 0);
  bool ColorPicker4(const char* label, float col[4], ImGuiColorEditFlags flags = 0, const float* ref_col = NULL);

} // namespace remixGui
