#include "targetver.h"

#include "D3D12Capture.hpp"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3d11_1.h>

#include <atomic>
#include <mutex>

#include "MinHook.h"
#include "Log.hpp"
#include "Vive/HMDSupport.hpp"

using namespace OVRInject;

namespace
{
  struct Slot {
    ID3D12Resource* texture = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    HANDLE shared_handle = nullptr;

    // 0 = never written, PENDING = being recorded, else the fence value
    // that is signaled once the GPU finished copying into this slot
    UINT64 fence_value = 0;
  };

  const UINT64 PENDING = UINT64_MAX;

  // Guards slots, generation and the reading slot. Shared between the
  // game's present thread and the VR render thread; only held briefly.
  std::mutex g_mutex;
  Slot g_slots[D3D12_CAPTURE_SLOTS];
  UINT g_generation = 0;
  int g_reading_slot = -1;

  std::atomic<ID3D12CommandQueue*> g_queue = nullptr;
  ID3D12Device* g_device = nullptr;
  ID3D12GraphicsCommandList* g_list = nullptr;
  ID3D12Fence* g_fence = nullptr;
  UINT64 g_fence_counter = 0;
  HANDLE g_fence_event = nullptr;

  UINT64 g_width = 0;
  UINT g_height = 0;
  DXGI_FORMAT g_format = DXGI_FORMAT_UNKNOWN;
  bool g_textures_failed = false;

  // The swapchain we mirror. Others (launcher, overlays) are ignored
  // unless the locked one goes quiet, e.g. after the game recreated it.
  IDXGISwapChain* g_locked_swapchain = nullptr;
  ULONGLONG g_locked_last_tick = 0;

  // Set by the first D3D12 present, tells the queue hook what to look for
  std::atomic_bool g_want_queue = false;
  LUID g_swapchain_luid = {};

  enum VRState { VR_NOT_TRIED, VR_READY, VR_FAILED };
  VRState g_vr_state = VR_NOT_TRIED;
  std::atomic_bool g_vr_retry = false;

  unsigned long long g_present_count = 0;
  unsigned long long g_copied_count = 0;

  typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDXGISwapChain*, UINT, UINT);
  typedef HRESULT(STDMETHODCALLTYPE* Present1Fn)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
  typedef void(STDMETHODCALLTYPE* ExecuteCommandListsFn)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

  PresentFn Original_Present = nullptr;
  Present1Fn Original_Present1 = nullptr;
  ExecuteCommandListsFn Original_ExecuteCommandLists = nullptr;

  void LogOwner(char* what, void* address) {
    char path[MAX_PATH] = "?";
    HMODULE module = nullptr;

    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)address, &module))
      GetModuleFileNameA(module, path, MAX_PATH);

    LOGSTRF("D3D12Capture: %s = 0x%p in %s\n", what, address, path);
  }

  bool SameLuid(const LUID& a, const LUID& b) {
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
  }

  void WaitForGpu() {
    ID3D12CommandQueue* queue = g_queue.load();

    if (queue == nullptr || g_fence == nullptr || g_fence_event == nullptr)
      return;

    UINT64 value = ++g_fence_counter;

    if (FAILED(queue->Signal(g_fence, value)))
      return;

    if (g_fence->GetCompletedValue() < value) {
      g_fence->SetEventOnCompletion(value, g_fence_event);
      WaitForSingleObject(g_fence_event, 2000);
    }
  }

  bool EnsureDevice(ID3D12CommandQueue* queue) {
    if (g_device != nullptr)
      return g_list != nullptr;

    // Everything is derived from the captured queue, never from the
    // swapchain: if a wrapper (ReShade, Streamline) sits on the queue, the
    // device and command list it hands out are wrapped to match.
    HRESULT result = queue->GetDevice(__uuidof(ID3D12Device), (void**)&g_device);
    if (FAILED(result)) {
      LOGSTRF("D3D12Capture: queue->GetDevice failed 0x%08X\n", result);
      return false;
    }

    result = g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&g_fence);
    if (FAILED(result)) {
      LOGSTRF("D3D12Capture: CreateFence failed 0x%08X\n", result);
      return false;
    }

    g_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    for (int i = 0; i < D3D12_CAPTURE_SLOTS; ++i) {
      result = g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                __uuidof(ID3D12CommandAllocator), (void**)&g_slots[i].allocator);
      if (FAILED(result)) {
        LOGSTRF("D3D12Capture: CreateCommandAllocator %d failed 0x%08X\n", i, result);
        return false;
      }
    }

    ID3D12GraphicsCommandList* list = nullptr;
    result = g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_slots[0].allocator, nullptr,
                                         __uuidof(ID3D12GraphicsCommandList), (void**)&list);
    if (FAILED(result)) {
      LOGSTRF("D3D12Capture: CreateCommandList failed 0x%08X\n", result);
      return false;
    }

    list->Close();
    g_list = list;

    LOGSTRF("D3D12Capture: capture device ready\n");

    return true;
  }

  bool EnsureTextures(const D3D12_RESOURCE_DESC& backbuffer_desc) {
    if (g_generation != 0 && g_width == backbuffer_desc.Width && g_height == backbuffer_desc.Height &&
        g_format == backbuffer_desc.Format)
      return true;

    // Don't hammer a failing allocation every frame, only on a new size
    if (g_textures_failed && g_width == backbuffer_desc.Width && g_height == backbuffer_desc.Height &&
        g_format == backbuffer_desc.Format)
      return false;

    WaitForGpu();

    std::lock_guard<std::mutex> lock(g_mutex);

    for (int i = 0; i < D3D12_CAPTURE_SLOTS; ++i) {
      if (g_slots[i].shared_handle != nullptr)
        CloseHandle(g_slots[i].shared_handle);
      if (g_slots[i].texture != nullptr)
        g_slots[i].texture->Release();

      g_slots[i].shared_handle = nullptr;
      g_slots[i].texture = nullptr;
      g_slots[i].fence_value = 0;
    }

    g_width = backbuffer_desc.Width;
    g_height = backbuffer_desc.Height;
    g_format = backbuffer_desc.Format;
    g_reading_slot = -1;
    g_textures_failed = true;

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = backbuffer_desc.Width;
    desc.Height = backbuffer_desc.Height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = backbuffer_desc.Format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // Simultaneous access lets D3D11 sample the texture while it stays in
    // COPY_DEST over here; render target keeps D3D11's open path happy.
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

    for (int i = 0; i < D3D12_CAPTURE_SLOTS; ++i) {
      HRESULT result = g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         __uuidof(ID3D12Resource), (void**)&g_slots[i].texture);
      if (FAILED(result)) {
        LOGSTRF("D3D12Capture: CreateCommittedResource %d failed 0x%08X (%llux%u format %d)\n", i, result,
                g_width, g_height, (int)g_format);
        return false;
      }

      result = g_device->CreateSharedHandle(g_slots[i].texture, nullptr, GENERIC_ALL, nullptr,
                                            &g_slots[i].shared_handle);
      if (FAILED(result)) {
        LOGSTRF("D3D12Capture: CreateSharedHandle %d failed 0x%08X\n", i, result);
        return false;
      }
    }

    g_textures_failed = false;
    ++g_generation;

    LOGSTRF("D3D12Capture: created %d shared textures %llux%u format %d (generation %u)\n",
            D3D12_CAPTURE_SLOTS, g_width, g_height, (int)g_format, g_generation);

    if (g_format != DXGI_FORMAT_B8G8R8A8_UNORM && g_format != DXGI_FORMAT_R8G8B8A8_UNORM &&
        g_format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB && g_format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
      LOGSTRF("D3D12Capture: WARNING backbuffer is not 8-bit SDR - HDR output will look wrong in the headset, "
              "turn HDR (and any swapchain upgrade addon) off\n");

    return true;
  }

  void Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;

    g_list->ResourceBarrier(1, &barrier);
  }

  void CopyFrame(ID3D12CommandQueue* queue, ID3D12Resource* backbuffer) {
    int slot_index = -1;

    {
      std::lock_guard<std::mutex> lock(g_mutex);

      // Oldest slot the render thread isn't looking at
      UINT64 oldest = PENDING;
      for (int i = 0; i < D3D12_CAPTURE_SLOTS; ++i) {
        if (i == g_reading_slot || g_slots[i].fence_value == PENDING)
          continue;

        if (slot_index == -1 || g_slots[i].fence_value < oldest) {
          slot_index = i;
          oldest = g_slots[i].fence_value;
        }
      }

      if (slot_index == -1)
        return;

      // The GPU is still busy with this slot's previous copy, so its
      // allocator can't be reset yet. Drop this frame instead of stalling.
      if (g_fence->GetCompletedValue() < oldest)
        return;

      g_slots[slot_index].fence_value = PENDING;
    }

    Slot& slot = g_slots[slot_index];
    UINT64 value = 0;

    if (SUCCEEDED(slot.allocator->Reset()) && SUCCEEDED(g_list->Reset(slot.allocator, nullptr))) {
      Transition(backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
      g_list->CopyResource(slot.texture, backbuffer);
      Transition(backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);

      if (SUCCEEDED(g_list->Close())) {
        ID3D12CommandList* lists[] = { g_list };
        queue->ExecuteCommandLists(1, lists);

        value = ++g_fence_counter;
        queue->Signal(g_fence, value);

        ++g_copied_count;
      }
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    slot.fence_value = value;
  }

  void EnsureVR() {
    if (g_vr_state == VR_READY)
      return;

    if (g_vr_state == VR_FAILED && !g_vr_retry.exchange(false))
      return;

    LOGSTRF("Initializing HMDSupport (D3D12)..\n");

    g_vr_state = HMDSupport::Singleton()->InitializeD3D12() ? VR_READY : VR_FAILED;
  }

  void OnPresent(IDXGISwapChain* swap_chain) {
    IDXGISwapChain3* swap_chain3 = nullptr;
    if (FAILED(swap_chain->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&swap_chain3)))
      return;

    UINT index = swap_chain3->GetCurrentBackBufferIndex();
    swap_chain3->Release();

    ID3D12Resource* backbuffer = nullptr;
    if (FAILED(swap_chain->GetBuffer(index, __uuidof(ID3D12Resource), (void**)&backbuffer)))
      return; // not a D3D12 swapchain

    ULONGLONG now = GetTickCount64();

    if (g_locked_swapchain != swap_chain) {
      if (g_locked_swapchain != nullptr && now - g_locked_last_tick < 2000) {
        backbuffer->Release();
        return;
      }

      D3D12_RESOURCE_DESC desc = backbuffer->GetDesc();
      LOGSTRF("D3D12Capture: mirroring swapchain 0x%p (%llux%u format %d)\n", swap_chain, desc.Width,
              desc.Height, (int)desc.Format);

      g_locked_swapchain = swap_chain;
    }

    g_locked_last_tick = now;
    ++g_present_count;

    ID3D12CommandQueue* queue = g_queue.load();

    if (queue == nullptr) {
      if (!g_want_queue) {
        ID3D12Device* device = nullptr;
        if (SUCCEEDED(backbuffer->GetDevice(__uuidof(ID3D12Device), (void**)&device))) {
          g_swapchain_luid = device->GetAdapterLuid();
          device->Release();

          g_want_queue = true;
        }
      }

      if (g_present_count % 600 == 0)
        LOGSTRF("D3D12Capture: %llu presents and still no direct command queue captured\n", g_present_count);
    } else if (EnsureDevice(queue)) {
      EnsureVR();

      if (g_vr_state == VR_READY && EnsureTextures(backbuffer->GetDesc()))
        CopyFrame(queue, backbuffer);

      if (g_copied_count == 1 || (g_copied_count != 0 && g_copied_count % 18000 == 0))
        LOGSTRF("D3D12Capture: %llu frames copied of %llu presents\n", g_copied_count, g_present_count);
    }

    backbuffer->Release();
  }

  thread_local bool t_inside_present = false;

  HRESULT STDMETHODCALLTYPE Hooked_Present(IDXGISwapChain* swap_chain, UINT sync_interval, UINT flags) {
    if (!t_inside_present && !(flags & DXGI_PRESENT_TEST)) {
      t_inside_present = true;
      OnPresent(swap_chain);
      t_inside_present = false;
    }

    // Unlike the D3D11 path the monitor always keeps presenting: a flip
    // model swapchain only advances its backbuffer through Present
    return Original_Present(swap_chain, sync_interval, flags);
  }

  HRESULT STDMETHODCALLTYPE Hooked_Present1(IDXGISwapChain1* swap_chain, UINT sync_interval, UINT flags,
                                            const DXGI_PRESENT_PARAMETERS* params) {
    if (!t_inside_present && !(flags & DXGI_PRESENT_TEST)) {
      t_inside_present = true;
      OnPresent(swap_chain);
      t_inside_present = false;
    }

    return Original_Present1(swap_chain, sync_interval, flags, params);
  }

  void STDMETHODCALLTYPE Hooked_ExecuteCommandLists(ID3D12CommandQueue* queue, UINT count,
                                                    ID3D12CommandList* const* lists) {
    if (g_queue.load(std::memory_order_relaxed) == nullptr && g_want_queue) {
      D3D12_COMMAND_QUEUE_DESC desc = queue->GetDesc();

      ID3D12Device* device = nullptr;
      if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT &&
          SUCCEEDED(queue->GetDevice(__uuidof(ID3D12Device), (void**)&device))) {
        LUID luid = device->GetAdapterLuid();
        device->Release();

        ID3D12CommandQueue* expected = nullptr;
        if (SameLuid(luid, g_swapchain_luid) && g_queue.compare_exchange_strong(expected, queue)) {
          queue->AddRef();
          LOGSTRF("D3D12Capture: captured direct command queue 0x%p\n", queue);
        }
      }
    }

    Original_ExecuteCommandLists(queue, count, lists);
  }

  bool Hook(char* what, void* from, void* to, void** original) {
    LogOwner(what, from);

    MH_STATUS status = MH_CreateHook(from, to, original);
    if (status == MH_OK)
      status = MH_EnableHook(from);

    if (status != MH_OK) {
      LOGSTRF("D3D12Capture: hooking %s failed: %s\n", what, MH_StatusToString(status));
      return false;
    }

    return true;
  }

  DWORD WINAPI HookThread(LPVOID) {
    // Let the game bring up its own device first
    for (int i = 0; i < 240 && GetModuleHandleW(L"D3D12Core.dll") == nullptr; ++i)
      Sleep(500);
    Sleep(3000);

    HMODULE d3d12_module = LoadLibraryW(L"d3d12.dll");
    HMODULE dxgi_module = LoadLibraryW(L"dxgi.dll");

    typedef HRESULT(WINAPI* CreateDeviceFn)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    typedef HRESULT(WINAPI* CreateFactoryFn)(REFIID, void**);

    CreateDeviceFn create_device = d3d12_module ? (CreateDeviceFn)GetProcAddress(d3d12_module, "D3D12CreateDevice") : nullptr;
    CreateFactoryFn create_factory = dxgi_module ? (CreateFactoryFn)GetProcAddress(dxgi_module, "CreateDXGIFactory1") : nullptr;

    if (create_device == nullptr || create_factory == nullptr) {
      LOGSTRF("D3D12Capture: d3d12.dll / dxgi.dll entry points not found\n");
      return 1;
    }

    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = DefWindowProcW;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"OVRInjectD3D12Probe";
    RegisterClassExW(&window_class);

    HWND window = CreateWindowExW(0, window_class.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 64, 64, nullptr,
                                  nullptr, window_class.hInstance, nullptr);

    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGIFactory2* factory = nullptr;
    IDXGISwapChain1* swap_chain = nullptr;

    HRESULT result = create_device(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&device);

    if (SUCCEEDED(result)) {
      D3D12_COMMAND_QUEUE_DESC queue_desc = {};
      queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

      result = device->CreateCommandQueue(&queue_desc, __uuidof(ID3D12CommandQueue), (void**)&queue);
    }

    if (SUCCEEDED(result))
      result = create_factory(__uuidof(IDXGIFactory2), (void**)&factory);

    if (SUCCEEDED(result)) {
      DXGI_SWAP_CHAIN_DESC1 desc = {};
      desc.Width = 64;
      desc.Height = 64;
      desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      desc.SampleDesc.Count = 1;
      desc.BufferCount = 2;
      desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

      // First without render target usage: post-processing injectors tend
      // to leave such a swapchain unwrapped, which yields the real DXGI
      // vtable and therefore a capture that includes their effects.
      desc.BufferUsage = DXGI_USAGE_BACK_BUFFER;
      result = factory->CreateSwapChainForHwnd(queue, window, &desc, nullptr, nullptr, &swap_chain);

      if (FAILED(result)) {
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        result = factory->CreateSwapChainForHwnd(queue, window, &desc, nullptr, nullptr, &swap_chain);
      }
    }

    if (FAILED(result)) {
      LOGSTRF("D3D12Capture: probe device / swapchain creation failed 0x%08X\n", result);
    } else {
      void** swap_chain_vtable = *(void***)swap_chain;
      void** queue_vtable = *(void***)queue;

      MH_STATUS status = MH_Initialize();
      if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        LOGSTRF("D3D12Capture: MH_Initialize failed: %s\n", MH_StatusToString(status));
      } else {
        // IDXGISwapChain::Present = 8, IDXGISwapChain1::Present1 = 22,
        // ID3D12CommandQueue::ExecuteCommandLists = 10
        Hook("ExecuteCommandLists", queue_vtable[10], Hooked_ExecuteCommandLists, (void**)&Original_ExecuteCommandLists);
        Hook("Present", swap_chain_vtable[8], Hooked_Present, (void**)&Original_Present);
        Hook("Present1", swap_chain_vtable[22], Hooked_Present1, (void**)&Original_Present1);

        LOGSTRF("D3D12Capture: hooks installed\n");
      }
    }

    if (swap_chain) swap_chain->Release();
    if (factory) factory->Release();
    if (queue) queue->Release();
    if (device) device->Release();

    if (window) DestroyWindow(window);
    UnregisterClassW(window_class.lpszClassName, window_class.hInstance);

    return 0;
  }
}

D3D12Capture*D3D12Capture::Singleton() {
  static D3D12Capture instance;

  return &instance;
}

bool D3D12Capture::IsEnhancedProcess() {
  wchar_t path[MAX_PATH] = L"";
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  _wcslwr_s(path);

  const wchar_t* name = wcsrchr(path, L'\\');
  name = name ? name + 1 : path;

  return wcsstr(name, L"enhanced") != nullptr;
}

void D3D12Capture::InstallHooksAsync() {
  HANDLE thread = CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr);

  if (thread != nullptr)
    CloseHandle(thread);
}

void D3D12Capture::RequestVRRetry() {
  g_vr_retry = true;
}

LUID D3D12Capture::GetAdapterLuid() const {
  return g_device != nullptr ? g_device->GetAdapterLuid() : LUID();
}

bool D3D12Capture::SyncSharedTextures(ID3D11Device* device11, ID3D11Texture2D** textures, UINT* generation) {
  std::lock_guard<std::mutex> lock(g_mutex);

  if (g_generation == 0)
    return false;

  if (*generation == g_generation)
    return textures[0] != nullptr;

  // One attempt per generation, success or not
  *generation = g_generation;

  for (int i = 0; i < D3D12_CAPTURE_SLOTS; ++i) {
    if (textures[i] != nullptr)
      textures[i]->Release();

    textures[i] = nullptr;
  }

  ID3D11Device1* device11_1 = nullptr;
  if (FAILED(device11->QueryInterface(__uuidof(ID3D11Device1), (void**)&device11_1))) {
    LOGSTRF("D3D12Capture: ID3D11Device1 not available, can't open shared textures\n");
    return false;
  }

  bool ok = true;

  for (int i = 0; i < D3D12_CAPTURE_SLOTS && ok; ++i) {
    HRESULT result = device11_1->OpenSharedResource1(g_slots[i].shared_handle, __uuidof(ID3D11Texture2D),
                                                     (void**)&textures[i]);
    if (FAILED(result)) {
      LOGSTRF("D3D12Capture: OpenSharedResource1 %d failed 0x%08X\n", i, result);
      ok = false;
    }
  }

  device11_1->Release();

  if (!ok) {
    for (int i = 0; i < D3D12_CAPTURE_SLOTS; ++i) {
      if (textures[i] != nullptr)
        textures[i]->Release();

      textures[i] = nullptr;
    }
  } else {
    LOGSTRF("D3D12Capture: render thread opened shared textures (generation %u)\n", g_generation);
  }

  return ok;
}

int D3D12Capture::AcquireLatestFrame(UINT generation) {
  std::lock_guard<std::mutex> lock(g_mutex);

  if (generation != g_generation || g_fence == nullptr)
    return -1;

  UINT64 completed = g_fence->GetCompletedValue();

  int best = -1;
  UINT64 best_value = 0;

  for (int i = 0; i < D3D12_CAPTURE_SLOTS; ++i) {
    UINT64 value = g_slots[i].fence_value;

    if (value != 0 && value != PENDING && value <= completed && value > best_value) {
      best = i;
      best_value = value;
    }
  }

  if (best != -1)
    g_reading_slot = best;

  return best;
}
