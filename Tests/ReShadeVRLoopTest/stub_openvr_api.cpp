// Stand-in openvr_api.dll for the ReShade VR-loop reproduction.
//
// ReShade recognises OpenVR purely by MODULE NAME ("Registering hooks for
// 'openvr_api.dll'") and then wraps IVRCompositor::Submit. On the first Submit
// it creates a VR effect runtime on the submitting D3D11 device and starts
// firing add-on events there - which is how the add-on stack overflow got onto
// OVRInject's HMD render thread. To reproduce that without SteamVR or a
// headset, ReShade only needs *an* openvr_api.dll whose interfaces answer.
//
// Every interface this hands out is the same dummy object: a vtable of
// generic slots that return 0 (VRCompositorError_None / false / null). Under
// the x64 ABI a callee that ignores its arguments and returns 0 in rax is a
// valid implementation of any method that does not return a struct by value,
// which covers everything the harness and ReShade's Submit path touch.

#include <Windows.h>
#include <cstdint>
#include <cstdio>

static uint64_t Generic() { return 0; }

struct Dummy { void** vtbl; };
static void* g_slots[512];
static Dummy g_dummy;

static void Init() {
  static bool done = false;
  if (done) return;
  done = true;
  for (auto& s : g_slots) s = (void*)&Generic;
  g_dummy.vtbl = g_slots;
}

extern "C" {
__declspec(dllexport) intptr_t VR_InitInternal(int* peError, int /*eType*/) {
  Init();
  if (peError) *peError = 0;
  return 1;
}
__declspec(dllexport) intptr_t VR_InitInternal2(int* peError, int /*eType*/, const char*) {
  Init();
  if (peError) *peError = 0;
  return 1;
}
__declspec(dllexport) void VR_ShutdownInternal() {}
__declspec(dllexport) bool VR_IsHmdPresent() { return true; }
__declspec(dllexport) bool VR_IsRuntimeInstalled() { return true; }
__declspec(dllexport) bool VR_IsInterfaceVersionValid(const char*) { return true; }
__declspec(dllexport) uint32_t VR_GetInitToken() { return 1; }
__declspec(dllexport) const char* VR_GetVRInitErrorAsSymbol(int) { return "VRInitError_None"; }
__declspec(dllexport) const char* VR_GetVRInitErrorAsEnglishDescription(int) { return "No Error (stub)"; }
__declspec(dllexport) const char* VR_RuntimePath() { return "C:\\stub"; }
__declspec(dllexport) void* VR_GetGenericInterface(const char* name, int* peError) {
  Init();
  if (peError) *peError = 0;
  // Log what ReShade asks for; useful when reading a failed run.
  FILE* f = nullptr;
  if (fopen_s(&f, "stub_openvr.log", "a") == 0 && f) { fprintf(f, "GetGenericInterface(%s)\n", name ? name : "null"); fclose(f); }
  return &g_dummy;
}
}
