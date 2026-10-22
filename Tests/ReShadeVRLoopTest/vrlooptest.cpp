// ReShade VR-loop reproduction for GTA V + OVRInject.
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
//   main thread  = the game: D3D11 device + swapchain, created through
//                  whatever dxgi.dll sits beside the exe (ReShade), clears +
//                  presents.
//   HMD thread   = HMDRenderer: vr::VR_Init, its own D3D11 device, two eye
//                  render targets, and per frame
//                  OMSetRenderTargets -> Clear -> IVRCompositor::Submit(Left/Right).
// openvr_api.dll beside the exe is the stub (stub_openvr_api.cpp), which is
// enough for ReShade to hook and build its VR runtime.
//
// PASS  = survives the full run with frames submitted. Exit code 0.
// Death = the process ends early; run.ps1 prints the exit code
//         (0xC00000FD / -1073741571 = STACK OVERFLOW).

#include <Windows.h>
#include <dxgi.h>
#include <d3d11.h>
#include <cstdio>
#include <atomic>

#include <openvr.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "user32.lib")

static const UINT WIDTH = 640, HEIGHT = 480, EYE = 512;
static std::atomic<int> g_submits{0};
static std::atomic<bool> g_stop{false};
static std::atomic<int> g_hmd_error{0};

#define CHECK(hr, what) if (FAILED(hr)) { printf("FAIL %s 0x%08X\n", what, (unsigned)(hr)); fflush(stdout); return 1; }

static DWORD WINAPI HmdThread(LPVOID) {
  vr::EVRInitError verr = vr::VRInitError_None;
  vr::IVRSystem* sys = vr::VR_Init(&verr, vr::VRApplication_Scene);
  if (verr != vr::VRInitError_None || !sys) { printf("HMD: VR_Init failed %d\n", (int)verr); g_hmd_error = 2; return 1; }
  vr::IVRCompositor* compositor = vr::VRCompositor();
  if (!compositor) { printf("HMD: no compositor\n"); g_hmd_error = 3; return 1; }

  // The same device creation as HMDRenderer::RenderThread
  ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
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

  printf("HMD: device ready, submitting\n"); fflush(stdout);

  while (!g_stop) {
    for (int e = 0; e < 2; ++e) {
      // The exact call the stack scan named (vtable slot 33, OMSetRenderTargets)
      ctx->OMSetRenderTargets(1, &eye_rtv[e], dsv);
      float c[4] = { 0.1f * e, 0.2f, 0.3f, 1.0f };
      ctx->ClearRenderTargetView(eye_rtv[e], c);
      ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
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
  wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"FakeGTA5";
  RegisterClassExW(&wc);
  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"ReShade VR loop test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              100, 100, WIDTH, HEIGHT, nullptr, nullptr, wc.hInstance, nullptr);

  // The game's device and swapchain, as GTA V creates them through D3D11
  DXGI_SWAP_CHAIN_DESC sd = {};
  sd.BufferDesc.Width = WIDTH; sd.BufferDesc.Height = HEIGHT; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = 1; sd.OutputWindow = hwnd; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  ID3D11Device* device; ID3D11DeviceContext* context; IDXGISwapChain* sc;
  CHECK(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                      &sd, &sc, &device, nullptr, &context), "D3D11CreateDeviceAndSwapChain");
  ID3D11Texture2D* backbuffer; ID3D11RenderTargetView* rtv;
  CHECK(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backbuffer), "GetBuffer");
  CHECK(device->CreateRenderTargetView(backbuffer, nullptr, &rtv), "CreateRenderTargetView");

  HANDLE hmd = CreateThread(nullptr, 0, HmdThread, nullptr, 0, nullptr);

  UINT frame = 0;
  ULONGLONG start = GetTickCount64();
  while (GetTickCount64() - start < (ULONGLONG)seconds * 1000) {
    MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    float color[4] = { (frame % 256) / 255.0f, 64 / 255.0f, 191 / 255.0f, 1.0f };
    context->OMSetRenderTargets(1, &rtv, nullptr);
    context->ClearRenderTargetView(rtv, color);
    sc->Present(1, 0);
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
