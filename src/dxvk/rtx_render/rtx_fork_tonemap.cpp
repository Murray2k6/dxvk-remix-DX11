// rtx_fork_tonemap.cpp
//
// Tonemap operator hooks: args population for global and local tonemappers,
// UI rendering for operator selection and per-operator parameter sliders,
// and Direct-mode query.

#include "rtx_fork_tonemap.h"
#include "rtx_imgui.h"
#include "rtx_options.h"

#include "../imgui/imgui.h"

namespace dxvk {
  namespace fork_tonemap {

    // Shared helper: copies the Hable / AgX / Lottes RtxOption values into
    // whatever args struct exposes the matching fields. AgX + Lottes source
    // classes are template parameters so global and local paths can share
    // this plumbing while reading from their own option sets.
    template<typename AgXClass, typename LottesClass, typename ArgsT>
    static void writeOperatorParams(ArgsT& args, TonemapOperator op) {
      // All operators share the same 8 float slots (mutually exclusive).
      // Hable: slot0=exposureBias, slot1=A, slot2=B, slot3=C, slot4=D, slot5=E, slot6=F, slot7=W
      // AgX:   slot0=gamma, slot1=saturation, slot2=exposureOffset, slot3=look, slot4=contrast, slot5=slope, slot6=power
      // Lottes: slot0=hdrMax, slot1=contrast, slot2=shoulder, slot3=midIn, slot4=midOut
      if (op == TonemapOperator::Lottes) {
        args.operatorSlot0 = LottesClass::hdrMax();
        args.operatorSlot1 = LottesClass::contrast();
        args.operatorSlot2 = LottesClass::shoulder();
        args.operatorSlot3 = LottesClass::midIn();
        args.operatorSlot4 = LottesClass::midOut();
        args.operatorSlot5 = 0.0f;
        args.operatorSlot6 = 0.0f;
        args.operatorSlot7 = 0.0f;
      } else if (op == TonemapOperator::AgX) {
        args.operatorSlot0 = AgXClass::gamma();
        args.operatorSlot1 = AgXClass::saturation();
        args.operatorSlot2 = AgXClass::exposureOffset();
        args.operatorSlot3 = static_cast<float>(AgXClass::look());
        args.operatorSlot4 = AgXClass::contrast();
        args.operatorSlot5 = AgXClass::slope();
        args.operatorSlot6 = AgXClass::power();
        args.operatorSlot7 = 0.0f;
      } else {
        // Hable Filmic (also default when other operators selected;
        // shader only reads these when op == HableFilmic).
        args.operatorSlot0 = RtxForkHableFilmic::exposureBias();
        args.operatorSlot1 = RtxForkHableFilmic::shoulderStrength();
        args.operatorSlot2 = RtxForkHableFilmic::linearStrength();
        args.operatorSlot3 = RtxForkHableFilmic::linearAngle();
        args.operatorSlot4 = RtxForkHableFilmic::toeStrength();
        args.operatorSlot5 = RtxForkHableFilmic::toeNumerator();
        args.operatorSlot6 = RtxForkHableFilmic::toeDenominator();
        args.operatorSlot7 = RtxForkHableFilmic::whitePoint();
      }
    }

    void populateTonemapOperatorArgs(ToneMappingApplyToneMappingArgs& args) {
      const TonemapOperator op = RtxForkGlobalTonemap::tonemapOperator();
      args.tonemapOperator    = static_cast<uint32_t>(op);
      args.directOperatorMode = (RtxOptions::tonemappingMode() == TonemappingMode::Direct) ? 1u : 0u;
      writeOperatorParams<RtxForkAgX, RtxForkLottes>(args, op);
    }

    void populateLocalTonemapOperatorArgs(FinalCombineArgs& args) {
      const TonemapOperator op = RtxForkLocalTonemap::tonemapOperator();
      args.tonemapOperator    = static_cast<uint32_t>(op);
      args.directOperatorMode = (RtxOptions::tonemappingMode() == TonemappingMode::Direct) ? 1u : 0u;
      writeOperatorParams<RtxForkLocalAgX, RtxForkLocalLottes>(args, op);
    }

    // Combo items string uses ImGui's \0-separated format.
    static const char* k_operatorItems = "None\0ACES\0ACES (Legacy)\0Hable Filmic\0AgX\0Lottes\0\0";

    // Shared slider rendering for per-operator parameter panels.
    static void showHableFilmicSliders() {
      ImGui::Indent();
      ImGui::Text("Hable Filmic Parameters:");
      if (ImGui::Button("Preset: Uncharted 2")) {
        RtxForkHableFilmic::shoulderStrengthObject().setDeferred(0.15f);
        RtxForkHableFilmic::linearStrengthObject()  .setDeferred(0.50f);
        RtxForkHableFilmic::linearAngleObject()     .setDeferred(0.10f);
        RtxForkHableFilmic::toeStrengthObject()     .setDeferred(0.20f);
        RtxForkHableFilmic::toeNumeratorObject()    .setDeferred(0.02f);
        RtxForkHableFilmic::toeDenominatorObject()  .setDeferred(0.30f);
        RtxForkHableFilmic::whitePointObject()      .setDeferred(11.2f);
      }
      ImGui::SameLine();
      if (ImGui::Button("Preset: Half-Life: Alyx")) {
        RtxForkHableFilmic::shoulderStrengthObject().setDeferred(0.319f);
        RtxForkHableFilmic::linearStrengthObject()  .setDeferred(0.5047f);
        RtxForkHableFilmic::linearAngleObject()     .setDeferred(0.1619f);
        RtxForkHableFilmic::toeStrengthObject()     .setDeferred(0.4667f);
        RtxForkHableFilmic::toeNumeratorObject()    .setDeferred(0.0f);
        RtxForkHableFilmic::toeDenominatorObject()  .setDeferred(0.7475f);
        RtxForkHableFilmic::whitePointObject()      .setDeferred(3.9996f);
      }
      RemixGui::DragFloat("Exposure Bias",     &RtxForkHableFilmic::exposureBiasObject(),     0.05f,  0.0f,  8.0f, "%.2f");
      RemixGui::DragFloat("Shoulder Strength", &RtxForkHableFilmic::shoulderStrengthObject(), 0.005f, 0.0f,  1.0f, "%.4f");
      RemixGui::DragFloat("Linear Strength",   &RtxForkHableFilmic::linearStrengthObject(),   0.005f, 0.0f,  1.0f, "%.4f");
      RemixGui::DragFloat("Linear Angle",      &RtxForkHableFilmic::linearAngleObject(),      0.005f, 0.0f,  1.0f, "%.4f");
      RemixGui::DragFloat("Toe Strength",      &RtxForkHableFilmic::toeStrengthObject(),      0.005f, 0.0f,  1.0f, "%.4f");
      RemixGui::DragFloat("Toe Numerator",     &RtxForkHableFilmic::toeNumeratorObject(),     0.001f, 0.0f,  0.5f, "%.4f");
      RemixGui::DragFloat("Toe Denominator",   &RtxForkHableFilmic::toeDenominatorObject(),   0.005f, 0.0f,  1.0f, "%.4f");
      RemixGui::DragFloat("White Point",       &RtxForkHableFilmic::whitePointObject(),       0.1f,   0.1f, 20.0f, "%.4f");
      ImGui::Unindent();
    }

    template<typename AgXClass>
    static void showAgXSlidersImpl(float minValue) {
      ImGui::Indent();
      ImGui::Text("AgX Controls:");
      ImGui::Separator();
      RemixGui::DragFloat("AgX Gamma",           &AgXClass::gammaObject(),          0.01f, minValue,  3.0f, "%.3f",    ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("AgX Saturation",      &AgXClass::saturationObject(),     0.01f, minValue,  2.0f, "%.3f",    ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("AgX Exposure Offset", &AgXClass::exposureOffsetObject(), 0.01f, -2.0f,     2.0f, "%.3f EV", ImGuiSliderFlags_AlwaysClamp);
      ImGui::Separator();
      RemixGui::Combo(    "AgX Look",            &AgXClass::lookObject(),           "None\0Punchy\0Golden\0Greyscale\0\0");
      ImGui::Separator();
      ImGui::Text("Advanced:");
      RemixGui::DragFloat("AgX Contrast",        &AgXClass::contrastObject(),       0.01f, minValue,  2.0f, "%.3f",    ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("AgX Slope",           &AgXClass::slopeObject(),          0.01f, minValue,  2.0f, "%.3f",    ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("AgX Power",           &AgXClass::powerObject(),          0.01f, minValue,  2.0f, "%.3f",    ImGuiSliderFlags_AlwaysClamp);
      ImGui::Unindent();
    }

    static void showGlobalAgXSliders() { showAgXSlidersImpl<RtxForkAgX>     (0.5f); }
    static void showLocalAgXSliders()  { showAgXSlidersImpl<RtxForkLocalAgX>(0.0f); }

    template<typename LottesClass>
    static void showLottesSlidersImpl() {
      ImGui::Indent();
      ImGui::Text("Lottes 2016 Parameters:");
      ImGui::Separator();
      RemixGui::DragFloat("HDR Max",         &LottesClass::hdrMaxObject(),   0.5f,   1.0f,  64.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Lottes Contrast", &LottesClass::contrastObject(), 0.01f,  1.0f,   3.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Shoulder",        &LottesClass::shoulderObject(), 0.01f,  0.5f,   2.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Mid In",          &LottesClass::midInObject(),    0.005f, 0.01f,  1.0f, "%.4f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Mid Out",         &LottesClass::midOutObject(),   0.005f, 0.01f,  1.0f, "%.4f", ImGuiSliderFlags_AlwaysClamp);
      ImGui::Unindent();
    }

    static void showGlobalLottesSliders() { showLottesSlidersImpl<RtxForkLottes>     (); }
    static void showLocalLottesSliders()  { showLottesSlidersImpl<RtxForkLocalLottes>(); }

    void showTonemapOperatorUI() {
      RemixGui::Combo("Tonemapping Operator",
                      &RtxForkGlobalTonemap::tonemapOperatorObject(),
                      k_operatorItems);

      const TonemapOperator op = RtxForkGlobalTonemap::tonemapOperator();
      if (op == TonemapOperator::HableFilmic) { showHableFilmicSliders(); }
      if (op == TonemapOperator::AgX)         { showGlobalAgXSliders();    }
      if (op == TonemapOperator::Lottes)      { showGlobalLottesSliders(); }
    }

    void showLocalTonemapOperatorUI() {
      RemixGui::Combo("Tonemapping Operator",
                      &RtxForkLocalTonemap::tonemapOperatorObject(),
                      k_operatorItems);

      const TonemapOperator op = RtxForkLocalTonemap::tonemapOperator();
      if (op == TonemapOperator::HableFilmic) { showHableFilmicSliders(); }
      if (op == TonemapOperator::AgX)         { showLocalAgXSliders();    }
      if (op == TonemapOperator::Lottes)      { showLocalLottesSliders(); }
    }

    bool shouldSkipToneCurve() {
      return RtxOptions::tonemappingMode() == TonemappingMode::Direct;
    }

  } // namespace fork_tonemap
} // namespace dxvk
