// Loads OpenVR under a PRIVATE module name so overlay / post-processing tools
// such as ReShade do not hook OVRInject's own OpenVR calls.
//
// WHY: ReShade recognises OpenVR purely by module name ("Registering hooks for
// 'openvr_api.dll'"), wraps IVRCompositor::Submit, and on the first Submit
// builds a second effect runtime (ReShadeVR.ini) on OVRInject's own D3D11 HMD
// device. From then on every ReShade add-on's per-frame events run on the HMD
// render thread. With some add-ons installed that ends in unbounded event
// recursion, a stack overflow during the loading screen. Tests\ReShadeVRLoopTest
// reproduces it offline.
//
// It is also wrong even without a crash: the captured game frame has ALREADY
// been through ReShade on the game's swapchain, so a VR runtime re-applies the
// whole preset (fisheye, grain, ...) to the headset image a second time.
//
// So OVRInject's headset path must never be a hook target. openvr_api.dll is
// linked /DELAYLOAD; this hook swaps the file actually loaded for a private copy,
// "openvr_api_ovrinject.dll", which such tools do not know by name. All VR_*
// calls in the code are unchanged. If the private copy is missing it is created
// from openvr_api.dll beside this module; if that fails, the normal name is
// loaded (it can then be hooked) and the log says so plainly.

#include <Windows.h>
#include <delayimp.h>
#include <cstring>
#include <cstdio>

#include "Log.hpp"

#pragma comment(lib, "delayimp.lib")

namespace {
  const char* kPublicName  = "openvr_api.dll";
  const char* kPrivateName = "openvr_api_ovrinject.dll";

  // Directory of the module this code is linked into (OVRInject.dll in the
  // game, the test exe in the harness) - that is where openvr_api.dll lives.
  bool ModuleDir(char* out, size_t n) {
    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)&ModuleDir, &self))
      return false;
    DWORD len = GetModuleFileNameA(self, out, (DWORD)n);
    if (len == 0 || len >= n) return false;
    char* slash = strrchr(out, '\\');
    if (!slash) return false;
    slash[1] = '\0';
    return true;
  }

  // True when the private copy is missing or differs in size from the public
  // one (openvr_api.dll was updated), so a stale copy is never used.
  bool NeedsRefresh(const char* pub, const char* priv) {
    WIN32_FILE_ATTRIBUTE_DATA a, b;
    if (!GetFileAttributesExA(priv, GetFileExInfoStandard, &b)) return true;
    if (!GetFileAttributesExA(pub, GetFileExInfoStandard, &a)) return false; // nothing to refresh from
    return a.nFileSizeLow != b.nFileSizeLow || a.nFileSizeHigh != b.nFileSizeHigh;
  }

  FARPROC WINAPI OpenVRDelayHook(unsigned notify, PDelayLoadInfo info) {
    if (notify != dliNotePreLoadLibrary || info == nullptr || info->szDll == nullptr ||
        _stricmp(info->szDll, kPublicName) != 0)
      return nullptr;

    char dir[MAX_PATH], pub[MAX_PATH], priv[MAX_PATH];
    if (!ModuleDir(dir, sizeof(dir))) {
      LOGSTRF("OpenVRPrivateLoad: could not resolve module dir - loading %s normally (ReShade WILL hook it)\n", kPublicName);
      return nullptr;
    }
    _snprintf_s(pub, _TRUNCATE, "%s%s", dir, kPublicName);
    _snprintf_s(priv, _TRUNCATE, "%s%s", dir, kPrivateName);

    if (NeedsRefresh(pub, priv)) {
      if (CopyFileA(pub, priv, FALSE))
        LOGSTRF("OpenVRPrivateLoad: created %s from %s\n", kPrivateName, kPublicName);
      else
        LOGSTRF("OpenVRPrivateLoad: could not create %s (error %lu)\n", priv, GetLastError());
    }

    HMODULE h = LoadLibraryA(priv);
    if (h) {
      LOGSTRF("OpenVRPrivateLoad: OpenVR loaded as %s - hidden from ReShade, no VR effect runtime on the HMD device\n", kPrivateName);
      return (FARPROC)h;
    }
    LOGSTRF("OpenVRPrivateLoad: LoadLibrary(%s) failed (error %lu) - loading %s normally (ReShade WILL hook it)\n",
            priv, GetLastError(), kPublicName);
    return nullptr; // fall through to the default load of openvr_api.dll
  }
}

extern "C" const PfnDliHook __pfnDliNotifyHook2 = OpenVRDelayHook;
