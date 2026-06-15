#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sstream>
#include <string>
#include <mutex>
#include <atomic>
#include <memory>
#include <chrono>
#include <cstdio>
#include <cstring>
#include "version.h"
#include "dx11_bridge_client.h"
#include "config/config.h"
#include "config/global_options.h"
#include "log/log.h"
#include "util_bridge_state.h"
#include "util_devicecommand.h"
#include "util_modulecommand.h"
#include "util_guid.h"
#include "util_process.h"
#include "util_semaphore.h"
#include "util_filesys.h"
#include "util_messagechannel.h"

using namespace bridge_util;

namespace dx11_bridge_client {

static HMODULE gModule = nullptr;
static std::mutex gAttachMutex;
static std::mutex gServerMutex;
static bool gAttached = false;
static std::string gRemixFolder;
static Guid gUniqueIdentifier;
static Process* gpServer = nullptr;
static NamedSemaphore* gpPresent = nullptr;
static std::chrono::steady_clock::time_point gTimeStart;
bool gbBridgeRunning = true;

static std::string GetFolderFromModule(HMODULE moduleHandle) {
  char path[MAX_PATH] = {};
  GetModuleFileNameA(moduleHandle ? moduleHandle : gModule, path, MAX_PATH);
  char* slash = strrchr(path, '\\');
  if (!slash) slash = strrchr(path, '/');
  if (slash) slash[1] = 0;
  return std::string(path);
}

static void AppendClientLog(const char* text) {
  const std::string dir = gRemixFolder.empty() ? GetFolderFromModule(gModule) : gRemixFolder;
  const std::string path = dir + "dx11_bridge_client.log";
  FILE* f = nullptr;
  fopen_s(&f, path.c_str(), "ab");
  if (!f) return;
  SYSTEMTIME st; GetLocalTime(&st);
  fprintf(f, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s\r\n",
    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
    text ? text : "");
  fclose(f);
}

void LogLine(const char* tag, const char* text) {
  char line[2048] = {};
  sprintf_s(line, sizeof(line), "[%s] %s", tag ? tag : "dx11-client", text ? text : "");
  AppendClientLog(line);
  OutputDebugStringA(line);
  OutputDebugStringA("\n");
  if (gAttached) {
    try { Logger::info(line); } catch (...) { }
  }
}

// DX11_V122_DEFINE_SERVER_MESSAGE_CHANNEL_FOR_DX9_ACK
// DX9 gets this helper from client/message_channels.h. The generated DX11
// bridge client is standalone, so it defines the same server-side message
// channel setup locally before using the DX9 Bridge_Ack sequence.
static std::unique_ptr<MessageChannelClient> gpServerMessageChannel;

static void initServerMessageChannel(const uint32_t serverThreadId) {
  gpServerMessageChannel = std::make_unique<MessageChannelClient>(static_cast<uint32_t>(serverThreadId));
  char msg[256] = {};
  sprintf_s(msg, sizeof(msg), "Server message channel initialized from Bridge_Ack pHandle/thread=%u.", serverThreadId);
  LogLine("bridge", msg);
}

void SetModule(HMODULE moduleHandle) {
  gModule = moduleHandle;
  if (gRemixFolder.empty() && moduleHandle) gRemixFolder = GetFolderFromModule(moduleHandle);
}

HMODULE LoadSystemDll(const char* dllName) {
  char sys[MAX_PATH] = {};
  GetSystemDirectoryA(sys, MAX_PATH);
  strcat_s(sys, "\\");
  strcat_s(sys, dllName);
  HMODULE mod = LoadLibraryA(sys);
  if (!mod) {
    char msg[512] = {};
    sprintf_s(msg, sizeof(msg), "LoadLibraryA('%s') failed err=%lu", sys, GetLastError());
    LogLine("loader", msg);
  }
  return mod;
}

static void OnServerExited(Process const*) {
  BridgeState::setServerState(BridgeState::ProcessState::Exited);
  gbBridgeRunning = false;
  LogLine("bridge", "x64 NvRemixBridge.exe exited; DX11 bridge disabled for this process.");
}

bool Attach() {
  std::lock_guard<std::mutex> lock(gAttachMutex);
  if (gAttached) return true;
  if (!gModule) GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
    reinterpret_cast<LPCSTR>(&Attach), &gModule);
  gRemixFolder = GetFolderFromModule(gModule);
  gTimeStart = std::chrono::high_resolution_clock::now();

  try {
    Config::init(Config::App::Client, gModule);
    GlobalOptions::init();
    Logger::init();
    dxvk::util::RtxFileSys::init(gRemixFolder);
    Logger::info("==================\nDX11 RTX Remix Bridge Client\n==================");
    Logger::info(std::string("Version: ") + std::string(BRIDGE_VERSION));
    Logger::info(std::string("Loaded DX11 bridge client from ") + gRemixFolder);
    initModuleBridge();
    initDeviceBridge();
    gpPresent = new NamedSemaphore("Present", 0, GlobalOptions::getPresentSemaphoreMaxFrames());
    BridgeState::setClientState(BridgeState::ProcessState::Init);
  } catch (...) {
    LogLine("bridge", "Attach failed while initializing bridge config/logger/IPC.");
    return false;
  }

  gAttached = true;
  LogLine("bridge", "DX11 bridge client attached and IPC queues initialized.");
  return true;
}

bool EnsureServer() {
  std::lock_guard<std::mutex> lock(gServerMutex);
  if (gpServer) return true;
  if (!Attach()) return false;

  const std::string serverPath = gRemixFolder + ".trex\\NvRemixBridge.exe";
  DWORD attrs = GetFileAttributesA(serverPath.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES) {
    LogLine("bridge", "Missing .trex\\NvRemixBridge.exe; DX11 bridge server cannot start.");
    return false;
  }

  SetEnvironmentVariableA("DX11_BRIDGE_MODE", "1");
  SetEnvironmentVariableA("DX11_BRIDGE_GUID", gUniqueIdentifier.toString().c_str());
  SetEnvironmentVariableA("DX11_BRIDGE_VERSION", BRIDGE_VERSION);

  std::stringstream cmdSS;
  cmdSS << '"' << serverPath << '"';
  cmdSS << " " << gUniqueIdentifier.toString();
  cmdSS << " " << BRIDGE_VERSION;
  cmdSS << " " << std::string(GetCommandLineA());
  const std::string command = cmdSS.str();

  char launchLine[4096] = {};
  sprintf_s(launchLine, sizeof(launchLine), "Launching x64 DX11 bridge server: %s", command.c_str());
  LogLine("bridge", launchLine);

  try {
    gpServer = new Process(command.c_str(), OnServerExited);
  } catch (...) {
    LogLine("bridge", "Process() failed to create NvRemixBridge.exe.");
    gpServer = nullptr;
    return false;
  }

  BridgeState::setServerState(BridgeState::ProcessState::Init);
  LogLine("bridge", "Sending Bridge_Syn and waiting for Bridge_Ack from x64 server on DeviceBridge using DX9 ACK sequence. DX11_V122_USE_DX9_BRIDGE_ACK_SEQUENCE");
  {
    ClientMessage syn(Commands::Bridge_Syn, reinterpret_cast<uintptr_t>(gpServer->GetCurrentProcessHandle()));
  }
  BridgeState::setClientState(BridgeState::ProcessState::Handshaking);

  // DX11_V122_USE_DX9_BRIDGE_ACK_SEQUENCE
  // Match NVIDIA's working DX9 bridge handshake:
  //   Bridge_Syn -> DeviceBridge::waitForCommand(Bridge_Ack)
  //   DeviceBridge::pop_front()
  //   initServerMessageChannel(ackResponse.pHandle)
  //   Bridge_Continue
  const auto waitForAck = DeviceBridge::waitForCommand(Commands::Bridge_Ack, GlobalOptions::getStartupTimeout());
  if (waitForAck != Result::Success) {
    LogLine("bridge", "Timed out or failed waiting for Bridge_Ack from x64 server on DeviceBridge.");
    BridgeState::setServerState(BridgeState::ProcessState::DoneProcessing);
    gbBridgeRunning = false;
    return false;
  }

  const auto ackResponse = DeviceBridge::pop_front();
  initServerMessageChannel(ackResponse.pHandle);
  BridgeState::setServerState(BridgeState::ProcessState::Handshaking);
  LogLine("bridge", "Bridge_Ack received through DX9 DeviceBridge ACK path; server message channel initialized; sending Bridge_Continue.");
  {
    ClientMessage cont(Commands::Bridge_Continue);
  }

  BridgeState::setClientState(BridgeState::ProcessState::Running);
  BridgeState::setServerState(BridgeState::ProcessState::Running);
  LogLine("bridge", "DX11 bridge client/server handshake completed.");
  return true;
}

void Detach() {
  std::lock_guard<std::mutex> lock(gServerMutex);
  if (gAttached) {
    BridgeState::setClientState(BridgeState::ProcessState::DoneProcessing);
    if (gpServer) {
      LogLine("bridge", "Sending Bridge_Terminate to x64 server.");
      gpServer->UnregisterExitCallback();
      { ClientMessage term(Commands::Bridge_Terminate); }
      DeviceBridge::waitForCommandAndDiscard(Commands::Bridge_Ack, GlobalOptions::getCommandTimeout());
      delete gpServer;
      gpServer = nullptr;
    }
    gpServerMessageChannel.reset();
    delete gpPresent;
    gpPresent = nullptr;
    BridgeState::setClientState(BridgeState::ProcessState::Exited);
    gAttached = false;
  }
}

}