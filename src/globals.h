#pragma once
#include <string>
#include "config.h"

extern Config g_cfg;
extern std::wstring g_modDir;   // folder containing this DLL, with trailing backslash

bool InstallD3DHooks();          // hooks.cpp
struct IDXGISwapChain;
void VR_OnPresent(IDXGISwapChain* sc);   // vr.cpp
void VR_OnResize(IDXGISwapChain* sc);
struct ID3D11Device; struct ID3D11DeviceContext;
void Discovery_Init(ID3D11Device* dev, ID3D11DeviceContext* ctx);   // discovery.cpp
void Discovery_OnPresent();
void Discovery_SetScreen(unsigned w, unsigned h);
void Discovery_Trigger();
void Watchdog_Beat();
void RDoc_Load(); void RDoc_Trigger(); void RDoc_Poll();
void Trace_Install(); void Trace_Event(const char* what, uintptr_t a = 0, uintptr_t b = 0); void Trace_Start(); void Trace_Poll();
void Discovery_LogExecs(int n);
void Discovery_LogDrawSources();
uint64_t Discovery_ExecCount();
void Discovery_WatchAddresses(const uintptr_t* addrs, const char* const* descs, int n);
