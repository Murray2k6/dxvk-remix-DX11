#pragma once

#include <cstdint>
#include <string>

namespace dxvk {

  // Engine families documented in documentation/engine_knowledge. The runtime
  // uses the detected family only for facts the knowledge base backs with a
  // source; everything else stays on the generic, game-agnostic paths.
  enum class D3D11EngineFamily : uint32_t {
    Unknown = 0,
    Unity, Unreal3, Unreal, CryEngine, Creation, Frostbite, Source2, GameMaker,
    Stingray, Phyre, Katana, Ego,
    Rage, RedEngine, Anvil, Dunia, Snowdrop, Disrupt, Glacier,
    Fox, Luminous, Northlight, Apex, FourA, Chrome, Foundation,
    Dawn, Void, ReEngine, MtFramework, Clausewitz, Genome, Serious,
    Kex, Telltale, Asura, DragonEngine, Diesel, Cobra,
    Framework2D,  // sprite/2D frameworks (frameworks_2d_web.md): MonoGame, FNA, Heaps, NW.js/ANGLE, ...
    Count
  };

  enum class D3D11DepthConvention : uint8_t {
    Unknown,   // not documented; detect from depth state at runtime
    Standard,  // near 0, far 1
    Reversed,  // near 1, far 0 (depth func GREATER, clear 0)
  };

  // Ways an engine draws the same mesh more than once per frame. Only one of
  // those draws may become RT geometry; the rest are duplicates.
  enum D3D11MultiPass : uint32_t {
    D3D11MultiPassNone            = 0,
    D3D11MultiPassDepthPrepass    = 1u << 0,  // depth-only pass before the main pass
    D3D11MultiPassLightPrepass    = 1u << 1,  // thin G-buffer, then a depth-EQUAL material pass
    D3D11MultiPassForwardPerLight = 1u << 2,  // one additive re-draw per extra light
    D3D11MultiPassVelocity        = 1u << 3,  // separate motion-vector re-draw
    D3D11MultiPassSecondaryView   = 1u << 4,  // low-quality env/reflection re-render
  };

  enum class D3D11TransformSource : uint8_t {
    Unknown,
    ConstantBuffer,  // per-draw world matrix in a cbuffer
    Buffer,          // structured/raw buffer indexed per instance (GPUScene etc.)
    CompositeWvp,    // only a combined world-view-projection is uploaded
  };

  enum class D3D11TiledLightLayout : uint8_t {
    None,
    Creation48,      // FO4: StructuredBuffer stride 48, view-space position
    CryEngine208,    // CE5: STiledLightShadeInfo, stride 208, camera-relative
    Hdrp224,         // Unity HDRP 2021-2022 LightData, stride 224, camera-relative (LightDefinition.cs.hlsl)
    Frostbite96,     // FB3 cbPunctualLightInfo: BaseLightInfo[128] of 96 B in a CS cbuffer
  };

  struct D3D11EngineFacts {
    D3D11EngineFamily     family;
    const char*           name;
    D3D11DepthConvention  depth;
    bool                  cameraRelative;      // VS subtracts the eye / uses a translated world
    uint32_t              multiPass;           // D3D11MultiPass bits
    D3D11TransformSource  transforms;
    bool                  reflectionNamesKept; // RDEF names survive in shipped DXBC
    bool                  decalsInGBuffer;     // decals are blended G-buffer re-draws
    D3D11TiledLightLayout lights;
    const char*           knowledgeFile;       // documentation/engine_knowledge/...
  };

  struct D3D11EngineProfile {
    const D3D11EngineFacts* facts = nullptr;
    std::string             evidence;  // what identified the engine
    // A Chromium-family child process that never renders the game
    // (frameworks_2d_web.md, injection recommendation 1): its command line
    // has --type=<kind> other than gpu-process. NW.js, Electron, CEF and
    // ANGLE-hosted games draw WebGL in the GPU process (or in the browser
    // process with --in-process-gpu, which has no --type). Remix stays a
    // pass-through there so renderer / utility processes cost nothing.
    bool                    chromiumHelperProcess = false;

    D3D11EngineFamily family() const;
    const char* name() const;
  };

  // Camera quantities an engine uploads. "Rel" variants have the camera
  // translation removed (camera-relative rendering); they need Eye or NegEye
  // to become absolute. NegEye is PreViewTranslation-style (-eye).
  // Unreal large world coordinates split the eye (unreal.md, dx12_unreal_unity.md):
  //   EyeTile: UE 5.0-5.3 ViewTilePosition; eye += tile * 2097152
  //            (UE_LWC_RENDER_TILE_SIZE, LargeWorldRenderPosition.cpp).
  //   EyeLow:  UE 5.4+ DoubleFloat low part, in the convention of the Eye /
  //            NegEye field it completes (added before the sign flip).
  enum class D3D11CameraField : uint8_t {
    View, Proj, ViewProj, InvView, InvViewProj, Eye, NegEye, RelView, RelViewProj, PrevViewProj,
    EyeTile, EyeLow, Count
  };

  // Shader reflection variable names that carry camera data, across every
  // engine in documentation/engine_knowledge whose shaders keep RDEF names.
  struct D3D11CameraNameRule {
    const char*        name;
    D3D11CameraField   field;
  };
  const D3D11CameraNameRule* GetCameraNameRules(size_t& count);

  // Register layout of the per-view cbuffer for engines whose shaders are
  // reflection-stripped. Byte offsets; -1 = not present. The cbuffer slot is
  // the VS slot. Every seed is validated against the live data (VP = P*V,
  // V orthonormal, eye consistent) before the camera uses it.
  struct D3D11CameraRegisterLayout {
    D3D11EngineFamily family;
    const char*       source;       // game/build the layout was taken from
    int32_t           slot;
    int32_t           offsets[size_t(D3D11CameraField::Count)];
    int32_t           eyeSlot;      // -1: same slot as the matrices
  };
  const D3D11CameraRegisterLayout* GetCameraRegisterLayouts(size_t& count);

  // Detects the engine of the current process once (exe name, loaded modules,
  // files next to the exe, window class) and returns the cached result.
  const D3D11EngineProfile& GetD3D11EngineProfile();

  const D3D11EngineFacts& GetD3D11EngineFacts(D3D11EngineFamily family);

  // Game units per centimetre for rtx.sceneScale (Remix renders in cm), or 0
  // when the engine's unit is not known.
  float GetD3D11EngineUnitsPerCentimetre(D3D11EngineFamily family);

}
