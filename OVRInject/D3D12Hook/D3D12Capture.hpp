#pragma once
#include "targetver.h"

#include <Windows.h>
#include <d3d11.h>

namespace OVRInject
{
#define D3D12_CAPTURE_SLOTS 4

  /**
  D3D12 front-end for GTA V Enhanced.

  The Legacy path detours D3D11CreateDeviceAndSwapChain, which Enhanced
  never calls. This hooks IDXGISwapChain::Present(1) and
  ID3D12CommandQueue::ExecuteCommandLists through a throwaway swapchain's
  vtables instead, so it works no matter when the DLL gets loaded.

  Every presented backbuffer is copied on the game's own queue into a ring
  of shared textures; the (unchanged, D3D11) VR render thread opens those
  by NT handle and only ever reads slots whose copy the GPU has finished.
  */
  class D3D12Capture
  {
  public:
    static D3D12Capture* Singleton();

    /** True when running inside the D3D12-only Enhanced executable */
    static bool IsEnhancedProcess();

    /** Spawns the hook thread. Safe to call from DllMain. */
    void InstallHooksAsync();

    /** Adapter the game renders on; the D3D11 VR device must match it */
    LUID GetAdapterLuid() const;

    /** Lets the next present try VR_Init again after it failed */
    void RequestVRRetry();

    /**
    Render thread: (re)opens the shared textures on its D3D11 device if the
    capture side recreated them (resolution / format change). Returns false
    when nothing is available yet.
    */
    bool SyncSharedTextures(ID3D11Device* device11, ID3D11Texture2D** textures, UINT* generation);

    /**
    Render thread: newest slot whose GPU copy has completed, or -1. The
    returned slot is protected from being overwritten until the next call.
    */
    int AcquireLatestFrame(UINT generation);

  };
}
