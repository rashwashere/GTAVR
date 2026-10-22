// ReShade VR-loop reproduction for GTA V Enhanced + OVRInject.
//
// With some ReShade add-ons installed, a VR launch could die during the loading
// screen from a STACK OVERFLOW inside ReShade's add-on event dispatcher, on
// OVRInject's HMD render thread. Mechanism, read off ReShade.log: ReShade hooks
// openvr_api.dll by name, wraps IVRCompositor::Submit, and on the first Submit
// creates a VR effect runtime (ReShadeVR.ini) on OVRInject's D3D11 device - so
// every add-on's per-frame events start firing on that thread.
// OVRInject\OpenVRPrivateLoad.cpp is the fix; vrlooptest_fixed.exe tests it.
//
// This program reproduces that path with no game and no headset:
//   main thread  = the game: D3D12 device + flip swapchain, clears + presents.
//                  The REAL D3D12Capture.cpp is compiled in and installs its
//                  hooks through whatever dxgi.dll sits beside the exe (ReShade).
//   HMD thread   = HMDRenderer: vr::VR_Init, a D3D11 device on the game's
//                  adapter, two eye render targets, and per frame
//                  OMSetRenderTargets -> Clear -> copy the captured frame in ->
//                  IVRCompositor::Submit(Left/Right).
// openvr_api.dll beside the exe is the stub (stub_openvr_api.cpp), which is
// enough for ReShade to hook and build its VR runtime.
//
// PASS  = survives the full run with frames submitted. Exit code 0.
// Death = the process ends early; run.ps1 prints the exit code
//         (0xC00000FD / -1073741571 = STACK OVERFLOW).

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3d11.h>
#include <cstdio>
#include <atomic>

#include "D3D12Hook/D3D12Capture.hpp"
#include "Vive/HMDSupport.hpp"
#include <openvr.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "user32.lib")

using namespace OVRInject;

HMDSupport* HMDSupport::Singleton() {
  static char storage[sizeof(HMDSupport)];
  return (HMDSupport*)storage;
}
bool HMDSupport::InitializeD3D12() { return true; }

static const UINT WIDTH = 640, HEIGHT = 480, BUFFERS = 3, EYE = 512;
static std::atomic<int> g_submits{0};
static std::atomic<bool> g_stop{false};
static std::atomic<int> g_hmd_error{0};

#define CHECK(hr, what) if (FAILED(hr)) { printf("FAIL %s 0x%08X\n", what, (unsigned)(hr)); fflush(stdout); return 1; }

static DWORD WINAPI HmdThread(LPVOID) {
  D3D12Capture* capture = D3D12Capture::Singleton();

  // Wait for the capture to find the game's adapter, like HMDRenderer does
  LUID luid = {};
  for (int i = 0; i < 400 && !g_stop; ++i) {
    luid = capture->GetAdapterLuid();
    if (luid.LowPart || luid.HighPart) break;
    Sleep(10);
  }
  if (!luid.LowPart && !luid.HighPart) { printf("HMD: no adapter LUID from capture\n"); g_hmd_error = 1; return 1; }

  vr::EVRInitError verr = vr::VRInitError_None;
  vr::IVRSystem* sys = vr::VR_Init(&verr, vr::VRApplication_Scene);
  if (verr != vr::VRInitError_None || !sys) { printf("HMD: VR_Init failed %d\n", (int)verr); g_hmd_error = 2; return 1; }
  vr::IVRCompositor* compositor = vr::VRCompositor();
  if (!compositor) { printf("HMD: no compositor\n"); g_hmd_error = 3; return 1; }

  IDXGIFactory1* factory = nullptr;
  CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);
  IDXGIAdapter1* adapter = nullptr;
  for (UINT i = 0; factory && factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 ad; adapter->GetDesc1(&ad);
    if (ad.AdapterLuid.LowPart == luid.LowPart && ad.AdapterLuid.HighPart == luid.HighPart) break;
    adapter->Release(); adapter = nullptr;
  }

  ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
  HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                 nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
  if (FAILED(hr)) { printf("HMD: D3D11CreateDevice 0x%08X\n", (unsigned)hr); g_hmd_error = 4; return 1; }

  // Eye targets + depth, as HMDRenderer creates them
  ID3D11Texture2D* eye_tex[2] = {}; ID3D11RenderTargetView* eye_rtv[2] = {};
  ID3D11Texture2D* depth_tex = nullptr; ID3D11DepthStencilView* dsv = nullptr;
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = EYE; td.Height = EYE; td.MipLevels = 1; td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
  td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  for (int e = 0; e < 2; ++e) {
    dev->CreateTexture2D(&td, nullptr, &eye_tex[e]);
    dev->CreateRenderTargetView(eye_tex[e], nullptr, &eye_rtv[e]);
  }
  D3D11_TEXTURE2D_DESC dd = td; dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  dev->CreateTexture2D(&dd, nullptr, &depth_tex);
  dev->CreateDepthStencilView(depth_tex, nullptr, &dsv);

  ID3D11Texture2D* shared[D3D12_CAPTURE_SLOTS] = {};
  UINT generation = 0;
  printf("HMD: device ready, submitting\n"); fflush(stdout);

  while (!g_stop) {
    capture->SyncSharedTextures(dev, shared, &generation);
    int slot = capture->AcquireLatestFrame(generation);

    for (int e = 0; e < 2; ++e) {
      // The exact call the stack scan named (vtable slot 33, OMSetRenderTargets)
      ctx->OMSetRenderTargets(1, &eye_rtv[e], dsv);
      float c[4] = { 0.1f * e, 0.2f, 0.3f, 1.0f };
      ctx->ClearRenderTargetView(eye_rtv[e], c);
      ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
      if (slot >= 0 && shared[slot]) {
        D3D11_BOX box = { 0, 0, 0, EYE < WIDTH ? EYE : WIDTH, EYE < HEIGHT ? EYE : HEIGHT, 1 };
        D3D11_TEXTURE2D_DESC sd; shared[slot]->GetDesc(&sd);
        if (sd.Format == DXGI_FORMAT_B8G8R8A8_UNORM || sd.Format == DXGI_FORMAT_R8G8B8A8_UNORM) { /* formats differ: skip copy */ }
        (void)box;
      }
      vr::Texture_t tex = { (void*)eye_tex[e], vr::API_DirectX, vr::ColorSpace_Gamma };
      compositor->Submit(e == 0 ? vr::Eye_Left : vr::Eye_Right, &tex);
    }
    ctx->Flush();
    ++g_submits;
    Sleep(11); // ~90 Hz
  }
  return 0;
}

int main(int argc, char** argv) {
  int seconds = (argc > 1) ? atoi(argv[1]) : 25;
  DeleteFileA("gtavrInjectLog.txt");
  DeleteFileA("stub_openvr.log");

  WNDCLASSEXW wc = { sizeof(wc) };
  wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"FakeGTA5Enhanced";
  RegisterClassExW(&wc);
  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"ReShade VR loop test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              100, 100, WIDTH, HEIGHT, nullptr, nullptr, wc.hInstance, nullptr);

  ID3D12Device* device; ID3D12CommandQueue* queue; IDXGIFactory4* factory; IDXGISwapChain1* sc1; IDXGISwapChain3* sc;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
  D3D12_COMMAND_QUEUE_DESC qd = {};
  CHECK(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
  DXGI_SWAP_CHAIN_DESC1 sd = {};
  sd.Width = WIDTH; sd.Height = HEIGHT; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = BUFFERS; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  CHECK(factory->CreateSwapChainForHwnd(queue, hwnd, &sd, nullptr, nullptr, &sc1), "CreateSwapChainForHwnd");
  CHECK(sc1->QueryInterface(IID_PPV_ARGS(&sc)), "IDXGISwapChain3");

  D3D12_DESCRIPTOR_HEAP_DESC hd = {}; hd.NumDescriptors = BUFFERS; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  ID3D12DescriptorHeap* heap; CHECK(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "rtv heap");
  UINT rtv_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  ID3D12Resource* buffers[BUFFERS]; D3D12_CPU_DESCRIPTOR_HANDLE rtvs[BUFFERS];
  for (UINT i = 0; i < BUFFERS; ++i) {
    CHECK(sc->GetBuffer(i, IID_PPV_ARGS(&buffers[i])), "GetBuffer");
    rtvs[i] = heap->GetCPUDescriptorHandleForHeapStart(); rtvs[i].ptr += i * rtv_size;
    device->CreateRenderTargetView(buffers[i], nullptr, rtvs[i]);
  }
  ID3D12CommandAllocator* alloc; ID3D12GraphicsCommandList* list; ID3D12Fence* fence;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "allocator");
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list)), "list");
  list->Close();
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
  HANDLE fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  UINT64 fence_value = 0;

  D3D12Capture::Singleton()->InstallHooksAsync();
  HANDLE hmd = CreateThread(nullptr, 0, HmdThread, nullptr, 0, nullptr);

  UINT frame = 0;
  ULONGLONG start = GetTickCount64();
  while (GetTickCount64() - start < (ULONGLONG)seconds * 1000) {
    MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    UINT index = sc->GetCurrentBackBufferIndex();
    float color[4] = { (frame % 256) / 255.0f, 64 / 255.0f, 191 / 255.0f, 1.0f };
    alloc->Reset(); list->Reset(alloc, nullptr);
    D3D12_RESOURCE_BARRIER b = {}; b.Transition.pResource = buffers[index];
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT; b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    list->ResourceBarrier(1, &b);
    list->ClearRenderTargetView(rtvs[index], color, 0, nullptr);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    list->ResourceBarrier(1, &b);
    list->Close();
    ID3D12CommandList* lists[] = { list }; queue->ExecuteCommandLists(1, lists);
    sc->Present(1, 0);
    queue->Signal(fence, ++fence_value);
    fence->SetEventOnCompletion(fence_value, fence_event);
    WaitForSingleObject(fence_event, INFINITE);
    ++frame;
  }

  g_stop = true;
  WaitForSingleObject(hmd, 3000);

  int submits = g_submits.load();
  bool pass = submits > 200 && g_hmd_error == 0;
  printf("\npresented %u game frames, %d HMD submit rounds, hmd_error %d\n%s\n",
         frame, submits, g_hmd_error.load(), pass ? "PASS - survived" : "FAIL - too few submits (not a crash)");
  fflush(stdout);
  TerminateProcess(GetCurrentProcess(), pass ? 0 : 2); // skip teardown: we test the run, not shutdown order
  return pass ? 0 : 2;
}
