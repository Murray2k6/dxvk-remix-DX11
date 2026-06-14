#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <strsafe.h>
#include "../shared/dx11_bridge_protocol.h"

namespace remix_dx11_bridge_client {
struct BridgeConnection {
  HANDLE pipe;
  uint32_t uid;
};

void Log(const wchar_t* fmt, ...);
bool StartDx11BridgeServer();
bool Connect(BridgeConnection& out);
HRESULT Send(BridgeConnection& c, remix_dx11_bridge::Cmd cmd, remix_dx11_bridge::Handle32 self,
             const void* payload, uint32_t payloadBytes, remix_dx11_bridge::Reply& reply,
             void* replyPayload = nullptr, uint32_t* replyPayloadBytes = nullptr);
HRESULT BridgeHello();
}
