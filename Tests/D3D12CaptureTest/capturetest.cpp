// Offline harness for the GTA V Enhanced (D3D12) capture path.
//
// Plays the role of the game: a bare D3D12 window that clears its flip-model
// backbuffer to a per-frame color and presents. The real D3D12Capture.cpp is
// compiled in and has to find Present / the command queue on its own, copy
// every frame into its shared ring, and hand it to a D3D11 device the way the
// VR render thread does. OpenVR is stubbed out, so no headset is needed.

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3d11.h>
#include <cstdio>

#include "D3D12Hook/D3D12Capture.hpp"
#include "Vive/HMDSupport.hpp"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "user32.lib")

using namespace OVRInject;

// --- OpenVR-free stand-ins for the two HMDSupport entry points the capture uses
HMDSupport* HMDSupport::Singleton() {
  static char storage[sizeof(HMDSupport)];
  return (HMDSupport*)storage;
}

bool HMDSupport::InitializeD3D12() {
  printf("[stub] HMDSupport::InitializeD3D12 called\n");
  return true;
}

static const UINT WIDTH = 640, HEIGHT = 480, BUFFERS = 3;

#define CHECK(hr, what) if (FAILED(hr)) { printf("FAIL %s 0x%08X\n", what, (unsigned)(hr)); return 1; }

int main() {
  DeleteFileA("gtavrInjectLog.txt");

  WNDCLASSEXW wc = { sizeof(wc) };
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"FakeGTA5Enhanced";
  RegisterClassExW(&wc);
  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Fake GTA5 Enhanced", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              100, 100, WIDTH, HEIGHT, nullptr, nullptr, wc.hInstance, nullptr);

  ID3D12Device* device; ID3D12CommandQueue* queue; IDXGIFactory4* factory; IDXGISwapChain1* sc1; IDXGISwapChain3* sc;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");

  D3D12_COMMAND_QUEUE_DESC qd = {};
  CHECK(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");

  DXGI_SWAP_CHAIN_DESC1 sd = {};
  sd.Width = WIDTH; sd.Height = HEIGHT; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // same as the game
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

  D3D12Capture* capture = D3D12Capture::Singleton();
  capture->InstallHooksAsync();

  // "VR render thread" side
  ID3D11Device* device11 = nullptr; ID3D11DeviceContext* context11 = nullptr; ID3D11Texture2D* staging = nullptr;
  ID3D11Texture2D* shared[D3D12_CAPTURE_SLOTS] = {};
  UINT generation = 0;

  int frames_read = 0, frames_correct = 0, last_red = -1, distinct_reds = 0;
  UINT frame = 0;
  ULONGLONG start = GetTickCount64();

  while (GetTickCount64() - start < 9000) {
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

    // Reader, exactly like HMDRenderer's D3D12 branch
    LUID luid = capture->GetAdapterLuid();
    if (device11 == nullptr && (luid.LowPart != 0 || luid.HighPart != 0)) {
      IDXGIAdapter1* adapter = nullptr;
      for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 ad; adapter->GetDesc1(&ad);
        if (ad.AdapterLuid.LowPart == luid.LowPart && ad.AdapterLuid.HighPart == luid.HighPart) break;
        adapter->Release(); adapter = nullptr;
      }
      CHECK(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                              &device11, nullptr, &context11), "D3D11CreateDevice");
    }

    if (device11 == nullptr || !capture->SyncSharedTextures(device11, shared, &generation))
      continue;

    int slot = capture->AcquireLatestFrame(generation);
    if (slot < 0)
      continue;

    if (staging == nullptr) {
      D3D11_TEXTURE2D_DESC td; shared[slot]->GetDesc(&td);
      printf("shared texture as seen by D3D11: %ux%u format %d bind 0x%X\n", td.Width, td.Height, td.Format, td.BindFlags);
      td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.MiscFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      CHECK(device11->CreateTexture2D(&td, nullptr, &staging), "staging");

      // The VR scene samples it, so a view must be creatable
      ID3D11ShaderResourceView* srv = nullptr;
      CHECK(device11->CreateShaderResourceView(shared[slot], nullptr, &srv), "CreateShaderResourceView");
      srv->Release();
    }

    context11->CopyResource(staging, shared[slot]);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(context11->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
      const unsigned char* px = (const unsigned char*)mapped.pData + (HEIGHT / 2) * mapped.RowPitch + (WIDTH / 2) * 4;
      int blue = px[0], green = px[1], red = px[2]; // BGRA
      context11->Unmap(staging, 0);

      ++frames_read;
      // Newest finished copy: at most a few frames behind what was just presented
      int age = (int)((frame - 1 - red) & 0xFF);
      if (blue == 191 && green == 64 && age <= 8) ++frames_correct;
      else if (frames_read - frames_correct <= 5) printf("bad pixel b%d g%d r%d at frame %u\n", blue, green, red, frame);
      if (red != last_red) { ++distinct_reds; last_red = red; }
    }
  }

  printf("\npresented %u, read back %d, correct %d, distinct frames seen %d\n", frame, frames_read, frames_correct, distinct_reds);

  bool pass = frames_read > 100 && frames_correct == frames_read && distinct_reds > frames_read / 2;
  printf("%s\n\n--- gtavrInjectLog.txt ---\n", pass ? "PASS" : "FAIL");

  char line[512]; FILE* fp = fopen("gtavrInjectLog.txt", "r");
  if (fp) { while (fgets(line, sizeof(line), fp)) fputs(line, stdout); fclose(fp); }

  return pass ? 0 : 1;
}
