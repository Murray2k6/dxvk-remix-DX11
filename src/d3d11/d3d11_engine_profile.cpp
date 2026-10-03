#include "d3d11_engine_profile.h"

#include "../util/log/log.h"
#include "../util/util_string.h"
#include "../dxvk/dxvk_scoped_annotation.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <mutex>

namespace dxvk {

  namespace {

    using DC = D3D11DepthConvention;
    using TS = D3D11TransformSource;
    using TL = D3D11TiledLightLayout;
    constexpr uint32_t kPre   = D3D11MultiPassDepthPrepass;
    constexpr uint32_t kLpp   = D3D11MultiPassLightPrepass;
    constexpr uint32_t kFwd   = D3D11MultiPassForwardPerLight;
    constexpr uint32_t kVel   = D3D11MultiPassVelocity;
    constexpr uint32_t kSec   = D3D11MultiPassSecondaryView;

    // One row per family, in enum order. Each value is backed by the knowledge
    // file named in the last column; "Unknown"/false where no source exists.
    constexpr std::array<D3D11EngineFacts, size_t(D3D11EngineFamily::Count)> kFacts = {{
      { D3D11EngineFamily::Unknown,      "Unknown",          DC::Unknown,  false, 0,                 TS::Unknown,        false, false, TL::None,         "" },
      { D3D11EngineFamily::Unity,        "Unity",            DC::Reversed, false, kPre | kFwd | kVel,       TS::ConstantBuffer, false, false, TL::Hdrp224,      "unity.md" },
      { D3D11EngineFamily::Unreal3,      "Unreal Engine 3",  DC::Unknown,  true,  kPre | kFwd,       TS::ConstantBuffer, false, false, TL::None,         "unreal.md" },
      { D3D11EngineFamily::Unreal,       "Unreal Engine 4/5",DC::Reversed, true,  kPre | kVel,       TS::ConstantBuffer, false, true,  TL::None,         "unreal.md" },
      { D3D11EngineFamily::CryEngine,    "CRYENGINE",        DC::Reversed, true,  kPre | kLpp,       TS::ConstantBuffer, true,  true,  TL::CryEngine208, "cryengine_frostbite.md" },
      { D3D11EngineFamily::Creation,     "Creation Engine",  DC::Standard, true,  kPre,              TS::ConstantBuffer, false, false, TL::Creation48,   "creation_source2_gamemaker.md" },
      { D3D11EngineFamily::Frostbite,    "Frostbite",        DC::Unknown,  true,  kPre,              TS::ConstantBuffer, true,  true,  TL::Frostbite96,  "cryengine_frostbite.md" },
      { D3D11EngineFamily::Source2,      "Source 2",         DC::Unknown,  true,  kPre,              TS::Buffer,         false, false, TL::None,         "creation_source2_gamemaker.md" },
      { D3D11EngineFamily::GameMaker,    "GameMaker",        DC::Standard, false, 0,                 TS::CompositeWvp,   true,  false, TL::None,         "creation_source2_gamemaker.md" },
      { D3D11EngineFamily::Stingray,     "Bitsquid/Stingray",DC::Unknown,  false, 0,                 TS::ConstantBuffer, false, true,  TL::None,         "bitsquid_phyre_katana_ego.md" },
      { D3D11EngineFamily::Phyre,        "PhyreEngine",      DC::Unknown,  false, 0,                 TS::ConstantBuffer, false, false, TL::None,         "bitsquid_phyre_katana_ego.md" },
      { D3D11EngineFamily::Katana,       "Katana Engine",    DC::Unknown,  false, 0,                 TS::ConstantBuffer, true,  true,  TL::None,         "bitsquid_phyre_katana_ego.md" },
      { D3D11EngineFamily::Ego,          "Codemasters EGO",  DC::Unknown,  false, kSec,              TS::ConstantBuffer, false, false, TL::None,         "bitsquid_phyre_katana_ego.md" },
      { D3D11EngineFamily::Rage,         "RAGE",             DC::Reversed, false, 0,                 TS::ConstantBuffer, false, false, TL::None,         "rage_red_anvil_dunia_snowdrop_disrupt_glacier.md" },
      { D3D11EngineFamily::RedEngine,    "REDengine 3",      DC::Reversed, false, 0,                 TS::ConstantBuffer, false, false, TL::None,         "rage_red_anvil_dunia_snowdrop_disrupt_glacier.md" },
      { D3D11EngineFamily::Anvil,        "Anvil",            DC::Unknown,  false, kPre,              TS::ConstantBuffer, false, false, TL::None,         "rage_red_anvil_dunia_snowdrop_disrupt_glacier.md" },
      { D3D11EngineFamily::Dunia,        "Dunia 2",          DC::Unknown,  true,  kPre,              TS::ConstantBuffer, true,  false, TL::None,         "rage_red_anvil_dunia_snowdrop_disrupt_glacier.md" },
      { D3D11EngineFamily::Snowdrop,     "Snowdrop",         DC::Unknown,  false, 0,                 TS::Unknown,        false, false, TL::None,         "rage_red_anvil_dunia_snowdrop_disrupt_glacier.md" },
      { D3D11EngineFamily::Disrupt,      "Disrupt",          DC::Unknown,  true,  0,                 TS::ConstantBuffer, true,  false, TL::None,         "rage_red_anvil_dunia_snowdrop_disrupt_glacier.md" },
      { D3D11EngineFamily::Glacier,      "Glacier 2",        DC::Unknown,  false, kPre,              TS::ConstantBuffer, false, false, TL::None,         "rage_red_anvil_dunia_snowdrop_disrupt_glacier.md" },
      { D3D11EngineFamily::Fox,          "Fox Engine",       DC::Reversed, false, kPre,              TS::ConstantBuffer, true,  true,  TL::None,         "fox_luminous_northlight_apex_4a_chrome_foundation.md" },
      { D3D11EngineFamily::Luminous,     "Luminous Studio",  DC::Unknown,  false, 0,                 TS::Unknown,        false, false, TL::None,         "fox_luminous_northlight_apex_4a_chrome_foundation.md" },
      { D3D11EngineFamily::Northlight,   "Northlight",       DC::Unknown,  false, kLpp,              TS::Unknown,        false, false, TL::None,         "fox_luminous_northlight_apex_4a_chrome_foundation.md" },
      { D3D11EngineFamily::Apex,         "Avalanche Apex",   DC::Reversed, true,  0,                 TS::ConstantBuffer, false, false, TL::None,         "fox_luminous_northlight_apex_4a_chrome_foundation.md" },
      { D3D11EngineFamily::FourA,        "4A Engine",        DC::Unknown,  false, 0,                 TS::ConstantBuffer, false, false, TL::None,         "fox_luminous_northlight_apex_4a_chrome_foundation.md" },
      { D3D11EngineFamily::Chrome,       "Chrome Engine 6",  DC::Unknown,  false, 0,                 TS::ConstantBuffer, false, false, TL::None,         "fox_luminous_northlight_apex_4a_chrome_foundation.md" },
      { D3D11EngineFamily::Foundation,   "Foundation",       DC::Unknown,  false, kPre | kLpp,       TS::ConstantBuffer, false, false, TL::None,         "fox_luminous_northlight_apex_4a_chrome_foundation.md" },
      { D3D11EngineFamily::Dawn,         "Dawn Engine",      DC::Unknown,  false, kVel,              TS::ConstantBuffer, false, false, TL::None,         "dawn_void_re_mtf_clausewitz_genome_serious.md" },
      { D3D11EngineFamily::Void,         "Void Engine",      DC::Reversed, false, kPre | kVel,              TS::ConstantBuffer, false, false, TL::None,         "dawn_void_re_mtf_clausewitz_genome_serious.md" },
      { D3D11EngineFamily::ReEngine,     "RE Engine",        DC::Unknown,  false, kPre,              TS::ConstantBuffer, false, false, TL::None,         "dawn_void_re_mtf_clausewitz_genome_serious.md" },
      { D3D11EngineFamily::MtFramework,  "MT Framework",     DC::Unknown,  false, kLpp,              TS::ConstantBuffer, false, false, TL::None,         "dawn_void_re_mtf_clausewitz_genome_serious.md" },
      { D3D11EngineFamily::Clausewitz,   "Clausewitz/Jomini",DC::Standard, false, 0,                 TS::ConstantBuffer, true,  false, TL::None,         "dawn_void_re_mtf_clausewitz_genome_serious.md" },
      { D3D11EngineFamily::Genome,       "Genome",           DC::Unknown,  false, 0,                 TS::Unknown,        false, false, TL::None,         "dawn_void_re_mtf_clausewitz_genome_serious.md" },
      { D3D11EngineFamily::Serious,      "Serious Engine 4", DC::Unknown,  false, kPre,              TS::ConstantBuffer, false, false, TL::None,         "dawn_void_re_mtf_clausewitz_genome_serious.md" },
      { D3D11EngineFamily::Kex,          "Kex Engine",       DC::Unknown,  false, 0,                 TS::Unknown,        false, false, TL::None,         "kex_telltale_asura_dragon_diesel_cobra_creation.md" },
      { D3D11EngineFamily::Telltale,     "Telltale Tool",    DC::Unknown,  false, 0,                 TS::Unknown,        false, false, TL::None,         "kex_telltale_asura_dragon_diesel_cobra_creation.md" },
      { D3D11EngineFamily::Asura,        "Asura",            DC::Unknown,  false, 0,                 TS::ConstantBuffer, false, false, TL::None,         "kex_telltale_asura_dragon_diesel_cobra_creation.md" },
      { D3D11EngineFamily::DragonEngine, "Dragon Engine",    DC::Reversed, false, 0,                 TS::ConstantBuffer, false, false, TL::None,         "kex_telltale_asura_dragon_diesel_cobra_creation.md" },
      { D3D11EngineFamily::Diesel,       "Diesel",           DC::Unknown,  false, 0,                 TS::ConstantBuffer, true,  false, TL::None,         "kex_telltale_asura_dragon_diesel_cobra_creation.md" },
      { D3D11EngineFamily::Cobra,        "Cobra",            DC::Unknown,  false, 0,                 TS::Unknown,        false, false, TL::None,         "kex_telltale_asura_dragon_diesel_cobra_creation.md" },
      { D3D11EngineFamily::Framework2D,  "2D framework",     DC::Unknown,  false, 0,                 TS::CompositeWvp,   false, false, TL::None,         "frameworks_2d_web.md" },
    }};

    static_assert(kFacts[size_t(D3D11EngineFamily::Framework2D)].family == D3D11EngineFamily::Framework2D,
                  "kFacts must stay in D3D11EngineFamily order");

    struct ExeRule { const char* exe; D3D11EngineFamily family; };

    // Lower-case executable names of known DX11 titles. Module and file
    // signatures below catch everything not listed here.
    constexpr ExeRule kExeRules[] = {
      { "fallout4.exe", D3D11EngineFamily::Creation },     { "skyrimse.exe", D3D11EngineFamily::Creation },
      { "fallout76.exe", D3D11EngineFamily::Creation },
      { "gta5.exe", D3D11EngineFamily::Rage },
      { "witcher3.exe", D3D11EngineFamily::RedEngine },
      { "ac3sp.exe", D3D11EngineFamily::Anvil },           { "ac4bfsp.exe", D3D11EngineFamily::Anvil },
      { "acu.exe", D3D11EngineFamily::Anvil },             { "acs.exe", D3D11EngineFamily::Anvil },
      { "acorigins.exe", D3D11EngineFamily::Anvil },       { "acodyssey.exe", D3D11EngineFamily::Anvil },
      { "farcry3_d3d11.exe", D3D11EngineFamily::Dunia },   { "farcry4.exe", D3D11EngineFamily::Dunia },
      { "farcry5.exe", D3D11EngineFamily::Dunia },         { "fcprimal.exe", D3D11EngineFamily::Dunia },
      { "farcrynewdawn.exe", D3D11EngineFamily::Dunia },
      { "thedivision.exe", D3D11EngineFamily::Snowdrop },  { "thedivision2.exe", D3D11EngineFamily::Snowdrop },
      { "watch_dogs.exe", D3D11EngineFamily::Disrupt },    { "watchdogs2.exe", D3D11EngineFamily::Disrupt },
      { "hitman.exe", D3D11EngineFamily::Glacier },        { "hitman2.exe", D3D11EngineFamily::Glacier },
      { "hitman3.exe", D3D11EngineFamily::Glacier },       { "hma.exe", D3D11EngineFamily::Glacier },
      { "mgsvtpp.exe", D3D11EngineFamily::Fox },           { "mgsvgz.exe", D3D11EngineFamily::Fox },
      { "ffxv_s.exe", D3D11EngineFamily::Luminous },
      { "quantumbreak.exe", D3D11EngineFamily::Northlight },{ "control_dx11.exe", D3D11EngineFamily::Northlight },
      { "justcause3.exe", D3D11EngineFamily::Apex },       { "justcause4.exe", D3D11EngineFamily::Apex },       { "madmax.exe", D3D11EngineFamily::Apex },
      { "metro.exe", D3D11EngineFamily::FourA },           { "metroexodus.exe", D3D11EngineFamily::FourA },
      { "dyinglightgame.exe", D3D11EngineFamily::Chrome },
      { "tombraider.exe", D3D11EngineFamily::Foundation }, { "rottr.exe", D3D11EngineFamily::Foundation }, { "sottr.exe", D3D11EngineFamily::Foundation },
      { "dxmd.exe", D3D11EngineFamily::Dawn },
      { "dishonored2.exe", D3D11EngineFamily::Void },      { "dishonored_do.exe", D3D11EngineFamily::Void },
      { "re2.exe", D3D11EngineFamily::ReEngine },          { "re3.exe", D3D11EngineFamily::ReEngine },
      { "re7.exe", D3D11EngineFamily::ReEngine },          { "devilmaycry5.exe", D3D11EngineFamily::ReEngine },
      { "monsterhunterworld.exe", D3D11EngineFamily::MtFramework },
      { "eu4.exe", D3D11EngineFamily::Clausewitz },        { "hoi4.exe", D3D11EngineFamily::Clausewitz },
      { "stellaris.exe", D3D11EngineFamily::Clausewitz },  { "ck2.exe", D3D11EngineFamily::Clausewitz },
      { "ck3.exe", D3D11EngineFamily::Clausewitz },        { "imperator.exe", D3D11EngineFamily::Clausewitz },
      { "victoria3.exe", D3D11EngineFamily::Clausewitz },
      { "elex.exe", D3D11EngineFamily::Genome },           { "elexii.exe", D3D11EngineFamily::Genome },
      { "sam4.exe", D3D11EngineFamily::Serious },          { "talos.exe", D3D11EngineFamily::Serious },
      { "sniperelite3.exe", D3D11EngineFamily::Asura },    { "sniperelite4_dx11.exe", D3D11EngineFamily::Asura },
      { "yakuza6.exe", D3D11EngineFamily::DragonEngine },  { "yakuzakiwami2.exe", D3D11EngineFamily::DragonEngine },
      { "yakuzalikeadragon.exe", D3D11EngineFamily::DragonEngine }, { "judgment.exe", D3D11EngineFamily::DragonEngine },
      { "lostjudgment.exe", D3D11EngineFamily::DragonEngine },
      { "payday2_win32_release.exe", D3D11EngineFamily::Diesel }, { "raid_win64_release.exe", D3D11EngineFamily::Diesel },
      { "elitedangerous64.exe", D3D11EngineFamily::Cobra }, { "planetcoaster.exe", D3D11EngineFamily::Cobra },
      { "planetzoo.exe", D3D11EngineFamily::Cobra },       { "jwe.exe", D3D11EngineFamily::Cobra },
      { "dragonageinquisition.exe", D3D11EngineFamily::Frostbite }, { "masseffectandromeda.exe", D3D11EngineFamily::Frostbite },
      { "bf4.exe", D3D11EngineFamily::Frostbite },         { "bf1.exe", D3D11EngineFamily::Frostbite },
      { "starwarsbattlefront.exe", D3D11EngineFamily::Frostbite }, { "mirrorsedgecatalyst.exe", D3D11EngineFamily::Frostbite },
      { "bfh.exe", D3D11EngineFamily::Frostbite },
      { "prey.exe", D3D11EngineFamily::CryEngine },        { "kingdomcome.exe", D3D11EngineFamily::CryEngine },
      { "huntgame.exe", D3D11EngineFamily::CryEngine },    { "crysis2.exe", D3D11EngineFamily::CryEngine },
      { "crysis3.exe", D3D11EngineFamily::CryEngine },     { "ryse.exe", D3D11EngineFamily::CryEngine },
      { "vermintide2.exe", D3D11EngineFamily::Stingray },
      { "ed8_3_pc.exe", D3D11EngineFamily::Phyre },        { "ed8_4_pc.exe", D3D11EngineFamily::Phyre },
      { "hnk.exe", D3D11EngineFamily::Phyre },             { "tokyoxanadu.exe", D3D11EngineFamily::Phyre },
    };

    struct ModuleRule { const wchar_t* module; D3D11EngineFamily family; };

    constexpr ModuleRule kModuleRules[] = {
      { L"UnityPlayer.dll",       D3D11EngineFamily::Unity },
      { L"CryRenderD3D11.dll",    D3D11EngineFamily::CryEngine },
      { L"CrySystem.dll",         D3D11EngineFamily::CryEngine },
      { L"Cry3DEngine.dll",       D3D11EngineFamily::CryEngine },
      { L"rendersystemdx11.dll",  D3D11EngineFamily::Source2 },
      { L"job_animation_player_win32_release.dll", D3D11EngineFamily::Stingray },
    };

    // Engine archive formats, so every title on an engine is detected, not
    // only the executables listed above. `dir` is relative to the exe folder;
    // `ext` matches a file extension, or a whole file/folder name when it has
    // no leading dot. Formats are the ones the engines' modding tools read.
    struct FileSignature { const char* dir; const char* ext; D3D11EngineFamily family; bool subdirs = false; };

    constexpr FileSignature kFileSignatures[] = {
      { ".",                 ".forge",      D3D11EngineFamily::Anvil },        // Anvil/AnvilNext containers
      { ".",                 ".tiger",      D3D11EngineFamily::Foundation },   // Crystal/Foundation (TR 2013+)
      { ".",                 ".asr",        D3D11EngineFamily::Asura },        // Rebellion Asura
      { ".",                 ".kpf",        D3D11EngineFamily::Kex },          // Nightdive Kex packs
      { ".",                 "re_chunk_000.pak", D3D11EngineFamily::ReEngine },
      { ".",                 "nativePC",    D3D11EngineFamily::MtFramework },
      { ".",                 "chunk0.bin",  D3D11EngineFamily::MtFramework },  // MH World
      { "master/0",          "00.dat",      D3D11EngineFamily::Fox },          // MGSV, PES
      { "Archives",          ".ttarch2",    D3D11EngineFamily::Telltale },
      { "archives_win64",    ".tab",        D3D11EngineFamily::Apex },         // Avalanche Apex
      { "datas",             ".earc",       D3D11EngineFamily::Luminous },
      { "data",              ".rmdp",       D3D11EngineFamily::Northlight },
      { "DW",                ".rpack",      D3D11EngineFamily::Chrome },
      { ".",                 "content.vfx", D3D11EngineFamily::FourA },
      { "../Runtime",        ".rpkg",       D3D11EngineFamily::Glacier },      // Glacier 2 (HITMAN 2016+)
      { "win64/ovldata",     ".ovl",        D3D11EngineFamily::Cobra },        // Planet Coaster/Zoo
      { "assets",            ".bundle",     D3D11EngineFamily::Diesel },       // PAYDAY 2, RAID
      { "../../Content",     ".gro",        D3D11EngineFamily::Serious, true }, // Serious Engine 3/4
    };

    std::string toLower(std::string s) {
      std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
      return s;
    }

    bool anyFileWithExtension(const std::filesystem::path& dir, const char* ext, uint32_t maxEntries = 4000) {
      std::error_code ec;
      uint32_t seen = 0;
      for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end && seen < maxEntries; it.increment(ec), ++seen) {
        if (toLower(it->path().extension().string()) == ext)
          return true;
      }
      return false;
    }

    // One level down: Serious Engine keeps .gro under Content/<Game>/.
    bool anyFileInSubdirWithExtension(const std::filesystem::path& dir, const char* ext, uint32_t maxDirs = 32) {
      std::error_code ec;
      uint32_t seen = 0;
      for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end && seen < maxDirs; it.increment(ec)) {
        if (!it->is_directory(ec))
          continue;
        ++seen;
        if (anyFileWithExtension(it->path(), ext, 512))
          return true;
      }
      return false;
    }

    bool pathExists(const std::filesystem::path& p) {
      std::error_code ec;
      return std::filesystem::exists(p, ec);
    }

    BOOL CALLBACK findUnrealWindow(HWND hwnd, LPARAM lparam) {
      DWORD pid = 0;
      GetWindowThreadProcessId(hwnd, &pid);
      if (pid != GetCurrentProcessId())
        return TRUE;
      wchar_t cls[64] = {};
      GetClassNameW(hwnd, cls, 64);
      if (wcscmp(cls, L"UnrealWindow") == 0) {
        *reinterpret_cast<bool*>(lparam) = true;
        return FALSE;
      }
      return TRUE;
    }

    D3D11EngineProfile detect() {
      ScopedCpuProfileZoneN("D3D11EngineProfile::detect");
      D3D11EngineProfile profile;
      auto pick = [&](D3D11EngineFamily family, std::string why) {
        profile.facts = &kFacts[size_t(family)];
        profile.evidence = std::move(why);
      };

      wchar_t exePathW[MAX_PATH] = {};
      GetModuleFileNameW(nullptr, exePathW, MAX_PATH);
      const std::filesystem::path exePath(exePathW);
      const std::filesystem::path exeDir = exePath.parent_path();
      const std::string exeName = toLower(exePath.filename().string());

      // 1. Known executables.
      for (const auto& rule : kExeRules) {
        if (exeName == rule.exe) {
          pick(rule.family, "exe " + exeName);
          return profile;
        }
      }

      // 2. Engine modules already loaded into the process.
      for (const auto& rule : kModuleRules) {
        if (GetModuleHandleW(rule.module) != nullptr) {
          pick(rule.family, "module " + str::fromws(rule.module));
          return profile;
        }
      }

      // 3. Files the engine ships next to the executable.
      const std::string stem = exePath.stem().string();
      if (pathExists(exeDir / "data.win")) { pick(D3D11EngineFamily::GameMaker, "data.win"); return profile; }
      if (pathExists(exeDir / (stem + "_Data"))) { pick(D3D11EngineFamily::Unity, stem + "_Data"); return profile; }
      if (pathExists(exeDir / "Data" / "Win32") || pathExists(exeDir / "Data" / "cas.cat")) {
        pick(D3D11EngineFamily::Frostbite, "Data/Win32 superbundles"); return profile;
      }
      if (pathExists(exeDir / "LINKDATA.BIN") || anyFileWithExtension(exeDir, ".ktsl2asbin")) {
        pick(D3D11EngineFamily::Katana, "Katana data files"); return profile;
      }
      if (anyFileWithExtension(exeDir, ".nefs") || anyFileWithExtension(exeDir, ".erp")) {
        pick(D3D11EngineFamily::Ego, "EGO archives"); return profile;
      }
      if (anyFileWithExtension(exeDir / "Data", ".ba2") || anyFileWithExtension(exeDir / "Data", ".bsa")) {
        pick(D3D11EngineFamily::Creation, "Bethesda archives"); return profile;
      }
      for (const auto& sig : kFileSignatures) {
        const std::filesystem::path dir = exeDir / sig.dir;
        const bool hit = sig.ext[0] == '.'
          ? anyFileWithExtension(dir, sig.ext) || (sig.subdirs && anyFileInSubdirWithExtension(dir, sig.ext))
          : pathExists(dir / sig.ext);
        if (hit) {
          pick(sig.family, std::string(sig.dir) + "/" + sig.ext);
          return profile;
        }
      }
      // Clausewitz/Jomini: script folders beside the exe, or under ../game (CK3, Victoria 3).
      for (const char* root : { ".", "../game" }) {
        const std::filesystem::path r = exeDir / root;
        if (pathExists(r / "common") && pathExists(r / "gfx") && pathExists(r / "interface")) {
          pick(D3D11EngineFamily::Clausewitz, std::string(root) + "/common+gfx+interface");
          return profile;
        }
      }
      // UE3 ships <Game>/Binaries/Win32|Win64/<exe> beside <Game>/<Name>Game/CookedPC*.
      {
        std::error_code ec;
        const std::filesystem::path gameRoot = exeDir.parent_path().parent_path();
        for (std::filesystem::directory_iterator it(gameRoot, ec), end; !ec && it != end; it.increment(ec)) {
          if (!it->is_directory(ec))
            continue;
          for (const char* cooked : { "CookedPC", "CookedPCConsole", "CookedPCServer" }) {
            if (pathExists(it->path() / cooked)) {
              pick(D3D11EngineFamily::Unreal3, std::string("UE3 ") + cooked);
              return profile;
            }
          }
        }
      }
      // Unreal ships <Project>/Binaries/Win64/<exe> beside <Project>/Content/Paks.
      if (pathExists(exeDir.parent_path().parent_path() / "Content" / "Paks")) {
        pick(D3D11EngineFamily::Unreal, "Content/Paks"); return profile;
      }

      // 4. Window class (only once the game has created its window).
      bool unrealWindow = false;
      EnumWindows(findUnrealWindow, reinterpret_cast<LPARAM>(&unrealWindow));
      if (unrealWindow) {
        pick(D3D11EngineFamily::Unreal, "window class UnrealWindow"); return profile;
      }

      // 5. 2D frameworks (frameworks_2d_web.md section 11). Last, because the
      // ANGLE pair also ships beside 3D games' embedded browsers.
      for (const char* file : { "MonoGame.Framework.dll", "FNA.dll", "directx.hdll", "OpenRA.Game.dll",
                                "Data.wolf", "nw.pak", "www/js/rpg_core.js", "js/rmmz_core.js", "libGLESv2.dll" }) {
        if (pathExists(exeDir / file)) {
          pick(D3D11EngineFamily::Framework2D, file);
          return profile;
        }
      }
      if (anyFileWithExtension(exeDir, ".xp3")) {
        pick(D3D11EngineFamily::Framework2D, "KiriKiri .xp3");
        return profile;
      }

      pick(D3D11EngineFamily::Unknown, "no signature for " + exeName);
      return profile;
    }

  }

  namespace {
    using CF = D3D11CameraField;

    // Names from the knowledge files (RDEF dumps, engine source). Matched
    // exactly; a name only counts when its cbuffer is bound to the VS.
    constexpr D3D11CameraNameRule kCameraNames[] = {
      // CRYENGINE 3/5 (cryengine_frostbite.md)
      { "CV_ViewMatr", CF::View }, { "CV_InvViewMatr", CF::InvView },
      { "CV_ViewProjMatr", CF::ViewProj }, { "CV_ViewProjZeroMatr", CF::RelViewProj },
      { "CV_InvViewProj", CF::InvViewProj }, { "CV_WorldViewPos", CF::Eye },
      { "CV_PrevViewProjMatr", CF::PrevViewProj },
      { "g_VS_ViewProjMatr", CF::ViewProj }, { "g_VS_ViewProjZeroMatr", CF::RelViewProj },
      { "g_VS_WorldViewPos", CF::Eye },
      // Lumberyard
      { "PerView_ViewMatr", CF::View }, { "PerView_ProjMatr", CF::Proj },
      { "PerView_ViewProjMatr", CF::ViewProj }, { "PerView_ViewProjZeroMatr", CF::RelViewProj },
      { "PerView_WorldViewPos", CF::Eye }, { "PerView_ViewProjMatrPrev", CF::PrevViewProj },
      // Frostbite
      { "viewMatrix", CF::View }, { "projMatrix", CF::Proj }, { "viewProjMatrix", CF::ViewProj },
      { "crViewProjMatrix", CF::RelViewProj }, { "cameraPos", CF::Eye },
      { "g_invViewProjMatrix", CF::InvViewProj }, { "g_cameraPos", CF::Eye },
      // Unity built-in / URP (development builds keep names)
      { "unity_MatrixV", CF::View }, { "glstate_matrix_projection", CF::Proj },
      { "unity_MatrixVP", CF::ViewProj }, { "unity_MatrixInvV", CF::InvView },
      { "unity_MatrixInvVP", CF::InvViewProj }, { "_WorldSpaceCameraPos", CF::Eye },
      { "_PrevViewProjMatrix", CF::PrevViewProj },
      // Unity HDRP (camera-relative by default). Its _ViewMatrix and
      // _InvViewMatrix share names with Dunia's absolute ones below; a view
      // with zero translation plus an eye is treated as camera-relative by
      // the runtime validation, so one entry serves both.
      { "_ProjMatrix", CF::Proj }, { "_ViewProjMatrix", CF::RelViewProj },
      { "_WorldSpaceCameraPos_Internal", CF::Eye },
      // Unreal Engine 4/5 (Development builds) and UE3
      { "View_TranslatedWorldToClip", CF::RelViewProj }, { "View_ViewToClip", CF::Proj },
      { "View_TranslatedWorldToView", CF::RelView }, { "View_WorldCameraOrigin", CF::Eye },
      { "View_PreViewTranslation", CF::NegEye },
      { "ViewProjectionMatrix", CF::RelViewProj }, { "PreViewTranslation", CF::NegEye },
      // UE 5.0-5.3 large world coordinates (UE 5.0.3 SceneView.cpp):
      // RelativeWorldCameraOrigin = ViewOrigin - tile offset,
      // RelativePreViewTranslation = PreViewTranslation + tile offset,
      // tile offset = ViewTilePosition * 2097152.
      { "View_RelativeWorldCameraOrigin", CF::Eye }, { "View_RelativePreViewTranslation", CF::NegEye },
      { "View_ViewTilePosition", CF::EyeTile },
      // UE 5.4+ DoubleFloat view origin (dx12_unreal_unity.md, 5.8 View
      // uniform buffer head): PreViewTranslation = High + Low.
      { "View_PreViewTranslationHigh", CF::NegEye }, { "View_PreViewTranslationLow", CF::EyeLow },
      // Source 2
      { "g_matWorldToView", CF::View }, { "g_matViewToProjection", CF::Proj },
      { "g_matWorldToProjection", CF::ViewProj }, { "g_vCameraPositionWs", CF::Eye },
      // Dunia 2 / Disrupt (worldPos - _CameraPosition) * _ViewRotProjectionMatrix
      { "_ViewMatrix", CF::View }, { "_ProjectionMatrix", CF::Proj },
      { "_ViewProjectionMatrix", CF::ViewProj }, { "_ViewRotProjectionMatrix", CF::RelViewProj },
      { "_InvViewMatrix", CF::InvView }, { "_CameraPosition", CF::Eye },
      { "_CameraPosition_DistanceScale", CF::Eye },
      { "_ViewRotProjectionMatrix_Previous", CF::PrevViewProj },
      // Katana (KTGL)
      { "mW2V", CF::View }, { "mW2P", CF::ViewProj }, { "mV2W", CF::InvView },
      { "mP2W", CF::InvViewProj }, { "vEye", CF::Eye }, { "vCameraPos", CF::Eye },
      { "g_mtPrevW2P", CF::PrevViewProj },
      // Fox Engine (cVSScene)
      { "m_projectionView", CF::ViewProj }, { "m_projection", CF::Proj }, { "m_view", CF::View },
      { "m_eyepos", CF::Eye },
      // PhyreEngine (Falcom)
      { "scene_View", CF::View }, { "scene_ViewProjection", CF::ViewProj },
      { "scene_EyePosition", CF::Eye }, { "ViewInverse", CF::InvView },
      // RAGE
      { "gViewInverse", CF::InvView },
      // 4A (LL / 2033 Redux)
      { "m_V", CF::View }, { "m_iV", CF::InvView }, { "m_P", CF::Proj }, { "m_VP", CF::ViewProj },
      { "m_iVP", CF::InvViewProj }, { "eye_position", CF::Eye },
      // Foundation (Rise of the Tomb Raider)
      { "CameraViewProject", CF::ViewProj }, { "Projection", CF::Proj }, { "ViewInv", CF::InvView },
      { "__CameraPosition", CF::Eye }, { "PrevViewProject", CF::PrevViewProj },
      // Anvil (AC3/AC4)
      { "g_WorldToView", CF::View }, { "g_ViewToWorld", CF::InvView },
      // Stride (Xenko) PerView (longtail_3d_group4.md)
      { "View", CF::View }, { "ViewInverse", CF::InvView }, { "ViewProjection", CF::ViewProj },
      { "Eye", CF::Eye },
      // VRAGE 2 (Space/Medieval Engineers) EnvironmentSettings b0, compiled at
      // runtime so names survive. View and ViewProj are camera-at-origin;
      // world_offset is the camera position (longtail_3d_group2.md).
      { "view_matrix", CF::View }, { "projection_matrix", CF::Proj },
      { "view_projection_matrix", CF::RelViewProj }, { "inv_view_matrix", CF::InvView },
      { "world_offset", CF::Eye },
      // bgfx predefined uniforms (longtail_3d_group3.md, bgfx source).
      { "u_view", CF::View }, { "u_invView", CF::InvView }, { "u_proj", CF::Proj },
      { "u_viewProj", CF::ViewProj }, { "u_invViewProj", CF::InvViewProj },
      // Halo MCC (Blam: H3/ODST/Reach ViewVS; H4/H2A MP EngineViewVS), from the
      // MCC editing kits' HLSL (longtail_3d_group2.md). World-space, row-vector.
      { "View_Projection", CF::ViewProj }, { "Camera_To_World", CF::InvView },
      { "vs_view_view_projection_matrix", CF::ViewProj },
      { "vs_view_camera_to_world_matrix", CF::InvView },
      { "vs_previousViewProjectionMatrix", CF::PrevViewProj },
    };

    constexpr int32_t N = -1;
    // offsets order: View, Proj, ViewProj, InvView, InvViewProj, Eye, NegEye, RelView, RelViewProj, PrevViewProj,
    //                EyeTile, EyeLow
    constexpr D3D11CameraRegisterLayout kCameraLayouts[] = {
      // FO4 cb12 (RE in this repo; fo4-decomp MODLOG): rotation-only view.
      { D3D11EngineFamily::Creation,   "Fallout 4 cb12",            12, { N, 64, N, 192, 256, 560, N, 0, 128, N, N, N }, -1 },
      // MGSV cVSScene b2.
      { D3D11EngineFamily::Fox,        "MGSV:TPP cVSScene",          2, { 128, 64, 0, N, N, 320, N, N, N, N, N, N }, -1 },
      // BF4 viewConstants b2.
      { D3D11EngineFamily::Frostbite,  "BF4 viewConstants",          2, { 48, 112, 176, N, N, 320, N, N, 240, N, N, N }, -1 },
      // Prey (CE5 fork) CBPerViewGlobal b13.
      { D3D11EngineFamily::CryEngine,  "Prey CBPerViewGlobal",      13, { 784, N, 80, 848, N, N, N, N, 0, 272, N, N }, -1 },
      // Witcher 3 cb12: eye c0, view c1, inv view c9, proj c13, inv VP c210.
      { D3D11EngineFamily::RedEngine,  "Witcher 3 cb12",            12, { 16, 208, N, 144, 3360, 0, N, N, N, N, N, N }, -1 },
      // Rise of the Tomb Raider b2.
      { D3D11EngineFamily::Foundation, "RotTR View b2",              2, { 0, 1376, 1216, 1440, N, 160, N, N, N, 928, N, N }, -1 },
      // Metro LL/2033 Redux cb_main_matrices1 b1, eye in cb_misc_1 b4.
      { D3D11EngineFamily::FourA,      "Metro Redux b1/b4",          1, { 0, 96, 224, 48, 288, 0, N, N, N, N, N, N }, 4 },
      // Dying Light b2: VP c0..3, eye c13.
      { D3D11EngineFamily::Chrome,     "Dying Light b2",             2, { N, N, 0, N, N, 208, N, N, N, N, N, N }, -1 },
      // Mad Max b0 Globals[0..3] = ViewProj.
      { D3D11EngineFamily::Apex,       "Mad Max Globals b0",         0, { N, N, 0, N, N, N, N, N, N, N, N, N }, -1 },
    };
    // Every layout row must list every field (a missing one would read as
    // offset 0, a present field).
    static_assert(size_t(D3D11CameraField::Count) == 12, "update kCameraLayouts rows");
  }

  const D3D11CameraNameRule* GetCameraNameRules(size_t& count) {
    count = std::size(kCameraNames);
    return kCameraNames;
  }

  const D3D11CameraRegisterLayout* GetCameraRegisterLayouts(size_t& count) {
    count = std::size(kCameraLayouts);
    return kCameraLayouts;
  }

  D3D11EngineFamily D3D11EngineProfile::family() const {
    return facts != nullptr ? facts->family : D3D11EngineFamily::Unknown;
  }

  const char* D3D11EngineProfile::name() const {
    return facts != nullptr ? facts->name : "Unknown";
  }

  const D3D11EngineFacts& GetD3D11EngineFacts(D3D11EngineFamily family) {
    return kFacts[std::min(size_t(family), kFacts.size() - 1u)];
  }

  namespace {
    // Anti-cheat: a replaced d3d11.dll in a protected game can get the
    // player's account banned, whatever this layer then does. Nothing done
    // in-process makes that safe, so this only warns as loudly as possible.
    // Folders the anti-cheat runtimes install, plus titles whose anti-cheat
    // ships elsewhere (longtail_3d_group4.md).
    std::string findAntiCheat() {
      wchar_t exePathW[MAX_PATH] = {};
      GetModuleFileNameW(nullptr, exePathW, MAX_PATH);
      const std::filesystem::path exePath(exePathW);
      const std::filesystem::path exeDir = exePath.parent_path();
      for (const char* dir : { "EasyAntiCheat", "BattlEye", "../EasyAntiCheat", "../BattlEye",
                               "../../EasyAntiCheat", "../../BattlEye" }) {
        if (pathExists(exeDir / dir))
          return std::string("folder ") + dir;
      }
      const std::string exeName = toLower(exePath.filename().string());
      for (const char* exe : { "destiny2.exe", "league of legends.exe", "blackdesert64.exe",
                               "iracingsim64dx11.exe", "survarium.exe", "robloxplayerbeta.exe",
                               "diabotical.exe" }) {
        if (exeName == exe)
          return std::string("exe ") + exe;
      }
      return std::string();
    }
  }

  float GetD3D11EngineUnitsPerCentimetre(D3D11EngineFamily family) {
    switch (family) {
      // Centimetre engines.
      case D3D11EngineFamily::Unreal3:
      case D3D11EngineFamily::Unreal:     return 1.0f;
      // Bethesda units: 1 unit = 1.428 cm (70 units per metre).
      case D3D11EngineFamily::Creation:   return 0.7f;
      // Source 2: 1 unit = 1 inch.
      case D3D11EngineFamily::Source2:    return 1.0f / 2.54f;
      // Metre engines.
      case D3D11EngineFamily::Unity:
      case D3D11EngineFamily::CryEngine:
      case D3D11EngineFamily::Frostbite:
      case D3D11EngineFamily::Rage:
      case D3D11EngineFamily::Dunia:
      case D3D11EngineFamily::Anvil:
      case D3D11EngineFamily::Apex:       return 0.01f;
      default:                            return 0.0f;
    }
  }

  const D3D11EngineProfile& GetD3D11EngineProfile() {
    static std::once_flag once;
    static D3D11EngineProfile profile;
    std::call_once(once, [] {
      profile = detect();

      // Chromium process model: only the GPU process (or a browser process
      // running the GPU in-process, which has no --type) draws WebGL.
      {
        const std::wstring cmd = GetCommandLineW();
        const size_t type = cmd.find(L"--type=");
        profile.chromiumHelperProcess = type != std::wstring::npos
          && cmd.compare(type, wcslen(L"--type=gpu-process"), L"--type=gpu-process") != 0;

        if (profile.chromiumHelperProcess) {
          Logger::info("[D3D11Engine] Chromium helper process (--type other than gpu-process): Remix passes through");
          const std::string info = "Remix DX11: Chromium helper process, pass-through";
          TracyAppInfo(info.c_str(), info.size());
        }
      }

      const std::string antiCheat = findAntiCheat();
      if (!antiCheat.empty()) {
        Logger::err(str::format("[D3D11Engine] ANTI-CHEAT DETECTED (", antiCheat, "). A replaced d3d11.dll in an "
          "anti-cheat protected game can get the account banned. Remove this d3d11.dll from this game."));
        const std::string warning = "Remix DX11: anti-cheat detected (" + antiCheat + ")";
        TracyAppInfo(warning.c_str(), warning.size());
      }
      const D3D11EngineFacts& f = *profile.facts;
      // Every Tracy capture then records which engine profile was active.
      const std::string appInfo = std::string("Remix DX11 engine profile: ") + f.name
        + " (" + profile.evidence + ")";
      TracyAppInfo(appInfo.c_str(), appInfo.size());
      Logger::info(str::format("[D3D11Engine] detected=", f.name, " (", profile.evidence, ")",
        " depth=", f.depth == D3D11DepthConvention::Reversed ? "reversed"
                 : f.depth == D3D11DepthConvention::Standard ? "standard" : "unknown",
        " cameraRelative=", f.cameraRelative ? 1 : 0,
        " multiPass=0x", std::hex, f.multiPass, std::dec,
        " transforms=", uint32_t(f.transforms),
        " reflectionNames=", f.reflectionNamesKept ? 1 : 0,
        " decalsInGBuffer=", f.decalsInGBuffer ? 1 : 0,
        " tiledLights=", uint32_t(f.lights),
        " knowledge=documentation/engine_knowledge/", f.knowledgeFile));
    });
    return profile;
  }

}
