#include "targetver.h"

#include "HMDRenderer.hpp"

#include "Vive/Math/Matrices.h"
#include "Vive/PoseStabilizer.hpp"
#include "Scene/RenderScene.hpp"
#include "D3D12Hook/D3D12Capture.hpp"

#include "openvr.h"
#include "Log.hpp"

#include <mutex>
#include <condition_variable>
#include "RoomScale.hpp"
#include "PoseTiming.hpp"

#pragma comment(lib, "dxgi.lib")

using namespace vr;
using namespace OVRInject;

HMDRenderer::HMDRenderer(HMDSupport* hmd_support) :
    hmd_support_(hmd_support),
    rt_texture_left_(nullptr), rt_view_left_(nullptr),
    rt_texture_right_(nullptr), rt_view_right_(nullptr),
    last_frame_texture_(nullptr),
    frame_hueristic_(2),
    spinlock_(false),
    hs_(0.493500f),
    vs_(0.396747f),
    zs_(1.0f),
    gta_device_(nullptr), gta_context_(nullptr),
    d3d12_mode_(false), d3d12_generation_(0), d3d12_textures_(), d3d12_slot_(-1) {
}

static_assert(NUM_TEXTURES_TO_BUFFER == D3D12_CAPTURE_SLOTS, "render thread mirrors the capture ring 1:1");

static std::future<bool> posesReady;
static std::mutex poseMutex;
static std::condition_variable cv;

static std::mutex frameMutex;

bool jfawklejf = false;
bool hasNewFrame = false;
std::atomic_bool posesUpdated = false;
int waitedExtraFrame = 0;

HMDRenderer::~HMDRenderer() {
}

void HMDRenderer::Initialize(ID3D11DeviceContext* gta_context, ID3D11Texture2D* backbuffer_texture) {
	// Begin the renderer thread

  gta_context_ = gta_context;
  gta_context_->GetDevice(&gta_device_);

  InitializeFrameTextures(backbuffer_texture);

  last_frame_texture_ = &texture_buffer_[0];

  thread_handle_ = std::thread(RenderThreadEntry, this);

  // STInit();

  jfawklejf = true;
}

void HMDRenderer::InitializeD3D12() {
  d3d12_mode_ = true;

  // No frame yet, the render loop idles on poses until one arrives
  last_frame_texture_ = nullptr;

  thread_handle_ = std::thread(RenderThreadEntry, this);

  jfawklejf = true;
}

HRESULT HMDRenderer::CreateDeviceOnGameAdapter() {
  LUID luid = D3D12Capture::Singleton()->GetAdapterLuid();

  IDXGIFactory1* factory = nullptr;
  HRESULT result = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);
  if (FAILED(result))
    return result;

  IDXGIAdapter1* adapter = nullptr;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 desc;
    adapter->GetDesc1(&desc);

    if (desc.AdapterLuid.LowPart == luid.LowPart && desc.AdapterLuid.HighPart == luid.HighPart) {
      LOGSTRF("HMDRenderer using the game's adapter: %ls\n", desc.Description);
      break;
    }

    adapter->Release();
    adapter = nullptr;
  }

  if (adapter == nullptr)
    LOGSTRF("HMDRenderer could not find the game's adapter, falling back to the default one\n");

  result = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                             nullptr, 0, D3D11_SDK_VERSION, &device_, 0, &device_context_);

  if (adapter)
    adapter->Release();
  factory->Release();

  return result;
}

int lastTex = 0;
void HMDRenderer::SubmitFrameTexture(const int& eye_index, ID3D11Texture2D* texture, const unsigned int& time) {
  
  if (!jfawklejf)
    return;

  /*
  if (lastTex == 0)
    lastTex = 1;
  else
    lastTex = 0;

  texture_buffer_[lastTex].consumed = true;
  texture_buffer_[lastTex].time = time;

  gta_context_->CopyResource(texture_buffer_[lastTex].texture, texture);
  
  STPush(&texture_buffer_[lastTex]);

  return;
  */

  frameMutex.lock();

  if (waitedExtraFrame > 0) {
    frameMutex.unlock();
    return;
  }

  last_frame_texture_->consumed = true;
  last_frame_texture_->time = time;

  gta_context_->CopyResource(last_frame_texture_->texture, texture);

  hasNewFrame = true;

  frameMutex.unlock();

  /*
  for (int i = 0; i < NUM_TEXTURES_TO_BUFFER; ++i) {
    if (texture_buffer_[i].consumed)
      continue;

    texture_buffer_[i].consumed = true;
    texture_buffer_[i].time = time;

    gta_context_->CopyResource(texture_buffer_[i].texture, texture);

    texture_queue_.enqueue(&texture_buffer_[i]);

    break;
  }
  */
}

void HMDRenderer::Uninitialize() {
  // TODO:
  // Uninitialize everything
  // For now, leaky beaky memory :)
}

bool HMDRenderer::RenderThreadEntry(HMDRenderer* renderer) {
  renderer->RenderThread();

  //while (true)
  //  renderer->WaitGetPoses();

  return true;
}

void HMDRenderer::InitializeRenderTargets() {
  // Get the values that OpenVR wants us to use
  IVRSystem* hmd = hmd_support_->get_hmd();
  IVRCompositor* compositor = hmd_support_->get_compositor();

  ID3D11Texture2D** eye_textures[] = { &rt_texture_left_, &rt_texture_right_ };
  ID3D11RenderTargetView** eye_targets[] = { &rt_view_left_, &rt_view_right_ };

  ID3D11Texture2D** depth_textures[] = { &depth_texture_left_, &depth_texture_right_ };
  ID3D11DepthStencilState** depth_states[] = { &depth_state_left_, &depth_state_right_ };
  ID3D11DepthStencilView** depth_views[] = { &depth_view_left_, &depth_view_right_ };

  uint32_t texture_width, texture_height;
  hmd->GetRecommendedRenderTargetSize(&texture_width, &texture_height);

  viewport_.Width = texture_width;
  viewport_.Height = texture_height;
  viewport_.MinDepth = 0.0f;
  viewport_.MaxDepth = 1.0f;
  viewport_.TopLeftX = 0;
  viewport_.TopLeftY = 0;

  // Create texture and render target view for each eye
  for (uint32_t i = 0; i < sizeof(eye_textures) / sizeof(eye_textures[0]); ++i) {
    D3D11_TEXTURE2D_DESC texture_desc;
    memset(&texture_desc, 0, sizeof(D3D11_TEXTURE2D_DESC));

    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = 1;
    texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Width = texture_width;
    texture_desc.Height = texture_height;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    texture_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    HRESULT result = device_->CreateTexture2D(&texture_desc, nullptr,
                                              eye_textures[i]);

    LOGSTRF("HMDRenderer successfully created render texture %d\n", i);

    if (FAILED(result))
      LOGFATALF("HMDRenderer failed to create eye texture %d\nError: %d", i, result);

    D3D11_RENDER_TARGET_VIEW_DESC target_desc;
    target_desc.Format = texture_desc.Format;
    target_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    target_desc.Texture2D.MipSlice = 0;

    result = device_->CreateRenderTargetView(*eye_textures[i], &target_desc,
                                             eye_targets[i]);

    if (FAILED(result))
      LOGFATALF("HMDRenderer failed to create eye render target %d\nError: %d", i, result);

    LOGSTRF("HMDRenderer successfully created render target %d\n", i);

    LOGSTRF("HMDRenderer creating depth texture %d\n", i);

    D3D11_TEXTURE2D_DESC depth_desc;
    memset(&depth_desc, 0, sizeof(D3D11_TEXTURE2D_DESC));

    depth_desc.Width = texture_width;
    depth_desc.Height = texture_height;
    depth_desc.MipLevels = 1;
    depth_desc.ArraySize = 1;
    depth_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depth_desc.SampleDesc.Count = 1;
    depth_desc.SampleDesc.Quality = 0;
    depth_desc.Usage = D3D11_USAGE_DEFAULT;
    depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    depth_desc.CPUAccessFlags = 0;
    depth_desc.MiscFlags = 0;

    result = device_->CreateTexture2D(&depth_desc, NULL, depth_textures[i]);

    if (FAILED(result))
      LOGFATALF("HMDRenderer failed to create eye depth texture %d\nError: %d", i, result);

    D3D11_DEPTH_STENCIL_DESC depth_stencil_desc;
    memset(&depth_stencil_desc, 0, sizeof(D3D11_DEPTH_STENCIL_DESC));

    // Set up the description of the stencil state.
    depth_stencil_desc.DepthEnable = false;
    depth_stencil_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth_stencil_desc.DepthFunc = D3D11_COMPARISON_LESS;

    depth_stencil_desc.StencilEnable = false;
    depth_stencil_desc.StencilReadMask = 0xFF;
    depth_stencil_desc.StencilWriteMask = 0xFF;

    depth_stencil_desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
    depth_stencil_desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_INCR;
    depth_stencil_desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
    depth_stencil_desc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;

    depth_stencil_desc.BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
    depth_stencil_desc.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_DECR;
    depth_stencil_desc.BackFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
    depth_stencil_desc.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;

    result = device_->CreateDepthStencilState(&depth_stencil_desc, depth_states[i]);

    if (FAILED(result))
      LOGFATALF("HMDRenderer failed to create stencil state %d\nError: %d", i, result);

    D3D11_DEPTH_STENCIL_VIEW_DESC depth_view_desc;
    memset(&depth_view_desc, 0, sizeof(D3D11_DEPTH_STENCIL_VIEW_DESC));

    depth_view_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depth_view_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    depth_view_desc.Texture2D.MipSlice = 0;

    result = device_->CreateDepthStencilView(*depth_textures[i], &depth_view_desc, depth_views[i]);

    if (FAILED(result))
      LOGFATALF("HMDRenderer failed to create stencil view %d\nError: %d", i, result);
  }
}

void HMDRenderer::InitializeSharedResources() {
  for (uint32_t i = 0; i < NUM_TEXTURES_TO_BUFFER; ++i) {
    ID3D11Texture2D* tex = texture_buffer_[i].texture;

    IDXGIResource* tex_resource;

    tex->QueryInterface(__uuidof(IDXGIResource), (void**)&tex_resource);

    HANDLE shared_handle;
    tex_resource->GetSharedHandle(&shared_handle);

    HRESULT result = device_->OpenSharedResource(shared_handle, __uuidof(ID3D11Texture2D), (void**)&texture_buffer_[i].shared_texture);
    if (FAILED(result))
      LOGFATALF("Failed to create shared texture id %d", i);
  }
}

void HMDRenderer::InitializeFrameTextures(ID3D11Texture2D* base_texture) {
  // Create the frame textures using the GTA device
  // with the same parameters as the backbuffer texture
  for (uint32_t i = 0; i < NUM_TEXTURES_TO_BUFFER; ++i) {
    texture_buffer_[i].consumed = false;

    D3D11_TEXTURE2D_DESC desc;
    base_texture->GetDesc(&desc);

    // Mark as shareable so we can pass to the other context
    if (!(desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED))
      desc.MiscFlags |= D3D11_RESOURCE_MISC_SHARED;

    HRESULT result = gta_device_->CreateTexture2D(&desc, nullptr, &texture_buffer_[i].texture);
    if (FAILED(result))
      LOGFATALF("Failed to create texture %d in texture buffer", i);
  }
}

void HMDRenderer::InitializeFrameRenderModels() {

}

static inline void LogCompositorError(EVRCompositorError error) {
  if (error != VRCompositorError_None) {
    switch (error) {
    case VRCompositorError_DoNotHaveFocus:
      LOGSTRF("Error: VRCompositorError_DoNotHaveFocus\n");
      break;

    case VRCompositorError_IncompatibleVersion:
      LOGSTRF("Error: VRCompositorError_IncompatibleVersion\n");
      break;

    case VRCompositorError_InvalidTexture:
      LOGSTRF("Error: VRCompositorError_InvalidTexture\n");
      break;

    case VRCompositorError_IsNotSceneApplication:
      LOGSTRF("Error: VRCompositorError_IsNotSceneApplication\n");
      break;

    case VRCompositorError_SharedTexturesNotSupported:
      LOGSTRF("Error: VRCompositorError_SharedTexturesNotSupported\n");
      break;

    case VRCompositorError_TextureIsOnWrongDevice:
      LOGSTRF("Error: VRCompositorError_TextureIsOnWrongDevice\n");
      break;

    case VRCompositorError_TextureUsesUnsupportedFormat:
      LOGSTRF("Error: VRCompositorError_TextureUsesUnsupportedFormat\n");
      break;

    default:
      LOGSTRF("Error: Unknown VRCompositorError\n");
      break;
    }
  }
}

// "Screen" to render to

ObjectVertex panel_verts[]
{
  { XMFLOAT3(-0.493500f * 6.0f, -0.396747f * 6.0f, 0.0f), XMFLOAT2(1.0f, 1.0f) },
  { XMFLOAT3(0.493500f * 6.0f, -0.396747f * 6.0f, 0.0f), XMFLOAT2(0.0f, 1.0f) },
  { XMFLOAT3(0.493500f * 6.0f, 0.396747f * 6.0f, 0.0f), XMFLOAT2(0.0f, 0.0f) },
  { XMFLOAT3(-0.493500f * 6.0f, 0.396747f * 6.0f, 0.0f), XMFLOAT2(1.0f, 0.0f) }
};

unsigned long panel_indices[]
{
  0, 1, 2,
  0, 2, 3,
  2, 1, 0,
  3, 2, 0
};

Object* screen_obj;

void HMDRenderer::SetHS(float hs) {
  hs_ = hs;

  panel_verts[0] = { XMFLOAT3(-hs_ * 6.0f, -vs_ * 6.0f, 0.0f), XMFLOAT2(1.0f, 1.0f) };
  panel_verts[1] = { XMFLOAT3(hs_ * 6.0f, -vs_ * 6.0f, 0.0f), XMFLOAT2(0.0f, 1.0f) };
  panel_verts[2] = { XMFLOAT3(hs_ * 6.0f, vs_ * 6.0f, 0.0f), XMFLOAT2(0.0f, 0.0f) };
  panel_verts[3] = { XMFLOAT3(-hs_ * 6.0f, vs_ * 6.0f, 0.0f), XMFLOAT2(1.0f, 0.0f) };

  screen_obj->InitializeVertices(&panel_verts[0], sizeof(panel_verts) / sizeof(panel_verts[0]), &panel_indices[0], sizeof(panel_indices) / sizeof(panel_indices[0]));
}

void HMDRenderer::SetVS(float vs) {
  vs_ = vs;

  panel_verts[0] = { XMFLOAT3(-hs_ * 6.0f, -vs_ * 6.0f, 0.0f), XMFLOAT2(1.0f, 1.0f) };
  panel_verts[1] = { XMFLOAT3(hs_ * 6.0f, -vs_ * 6.0f, 0.0f), XMFLOAT2(0.0f, 1.0f) };
  panel_verts[2] = { XMFLOAT3(hs_ * 6.0f, vs_ * 6.0f, 0.0f), XMFLOAT2(0.0f, 0.0f) };
  panel_verts[3] = { XMFLOAT3(-hs_ * 6.0f, vs_ * 6.0f, 0.0f), XMFLOAT2(1.0f, 0.0f) };

  screen_obj->InitializeVertices(&panel_verts[0], sizeof(panel_verts) / sizeof(panel_verts[0]), &panel_indices[0], sizeof(panel_indices) / sizeof(panel_indices[0]));
}

void HMDRenderer::SetZS(float zs) {
  zs_ = zs;
}

void HMDRenderer::SetFS(float fs) {
  fs_ = fs;
}

void HMDRenderer::STInit() {
  // Initialize D3D11 state
  HRESULT result =
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr,
      0, D3D11_SDK_VERSION, &device_, 0, &device_context_);

  if (FAILED(result))
    LOGFATALF("HMDRenderer device creation error: %d\n", result);
  //device_ = gta_device_;
  //device_context_ = gta_context_;

  InitializeRenderTargets();

  InitializeSharedResources();

  LOGSTRF("Created render targets and shared resources\n");

  IVRSystem* hmd = hmd_support_->get_hmd();
  IVRCompositor* compositor = hmd_support_->get_compositor();

  scene_ = new RenderScene(device_);
  scene_->Initialize();

  LOGSTRF("Initialized scene\n");

  // Create the panel we render to
  screen_obj = new Object(device_);
  screen_obj->InitializeVertices(&panel_verts[0], sizeof(panel_verts) / sizeof(panel_verts[0]), &panel_indices[0], sizeof(panel_indices) / sizeof(panel_indices[0]));

  scene_->AddObject(screen_obj);

  // Create projection matrices
  auto mat = hmd->GetProjectionMatrix(Eye_Left, 0.1f, 100.0f, API_OpenGL);

  proj_mat_left_ = XMMATRIX(
    mat.m[0][0], mat.m[0][1], mat.m[0][2], mat.m[0][3],
    mat.m[1][0], mat.m[1][1], mat.m[1][2], mat.m[1][3],
    mat.m[2][0], mat.m[2][1], mat.m[2][2], mat.m[2][3],
    mat.m[3][0], mat.m[3][1], mat.m[3][2], mat.m[3][3]
  );

  auto matr = hmd->GetProjectionMatrix(Eye_Right, 0.1f, 100.0f, API_OpenGL);

  proj_mat_right_ = XMMATRIX(
    matr.m[0][0], matr.m[0][1], matr.m[0][2], matr.m[0][3],
    matr.m[1][0], matr.m[1][1], matr.m[1][2], matr.m[1][3],
    matr.m[2][0], matr.m[2][1], matr.m[2][2], matr.m[2][3],
    matr.m[3][0], matr.m[3][1], matr.m[3][2], matr.m[3][3]
  );

  //posesThread = std::thread(RenderThreadEntry, this);
}

void HMDRenderer::STPush(FrameTexture* tex) {
  IVRSystem* hmd = hmd_support_->get_hmd();
  IVRCompositor* compositor = hmd_support_->get_compositor();

  if (compositor == nullptr)
    return;

  // Since the texture was updated, relocate the screen
  auto cam = scene_->GetCamera();
  cam->SetViewMatrix(view_mat_);

  screen_obj->position.x = cam->position.x + cam->GetForwardVector().x * 2.0f;
  screen_obj->position.y = cam->position.y + cam->GetForwardVector().y * 2.0f;
  screen_obj->position.z = cam->position.z + cam->GetForwardVector().z * 2.0f;

  screen_obj->focus.x = cam->position.x;
  screen_obj->focus.y = cam->position.y;
  screen_obj->focus.z = cam->position.z;

  screen_obj->up = cam->up;

  device_context_->RSSetViewports(1, &viewport_);

  // Apply the texture to the screen
  if (!screen_obj->SetTexture(tex->shared_texture)) {
    InitializeSharedResources();
    screen_obj->SetTexture(tex->shared_texture);
  }


  // Pass data to the renderer for left eye pass
  {
    scene_->GetCamera()->SetProjectionMatrix(proj_mat_left_);

    auto eye_mat_l = ConvertSteamVRMatrixToMatrix4(hmd->GetEyeToHeadTransform(Eye_Left)).invert();

    XMMATRIX eye_matrix_left = eye_mat_l.getDXMatrix();

    scene_->GetCamera()->SetEyeMatrix(eye_matrix_left);
    scene_->GetCamera()->SetViewMatrix(view_mat_);

    scene_->RenderFrame(&rt_view_left_, &depth_view_left_, &depth_state_left_);
  }

  // Pass data to the renderer for right eye pass
  {
    scene_->GetCamera()->SetProjectionMatrix(proj_mat_right_);
    auto eye_mat_r = ConvertSteamVRMatrixToMatrix4(hmd->GetEyeToHeadTransform(Eye_Right)).invert();

    XMMATRIX eye_matrix_right = eye_mat_r.getDXMatrix();

    scene_->GetCamera()->SetEyeMatrix(eye_matrix_right);
    scene_->GetCamera()->SetViewMatrix(view_mat_);

    scene_->RenderFrame(&rt_view_right_, &depth_view_right_, &depth_state_right_);
  }

  device_context_->Flush();

  {
    // Pass left texture to vr compositor

    vr::Texture_t leftTex = { (void*)rt_texture_left_, vr::API_DirectX, vr::ColorSpace_Gamma };
    vr::EVRCompositorError err = compositor->Submit(Eye_Left, &leftTex);

    if (err && err != VRCompositorError_DoNotHaveFocus)
      LogCompositorError(err);
  }

  {
    // Pass right texture to vr compositor

    vr::Texture_t rightTex = { (void*)rt_texture_right_ , vr::API_DirectX, vr::ColorSpace_Gamma };
    vr::EVRCompositorError err = compositor->Submit(Eye_Right, &rightTex);

    if (err && err != VRCompositorError_DoNotHaveFocus)
      LogCompositorError(err);
  }

  //thread_handle_ = CreateThread(NULL, 0, &RenderThreadEntry, (void*)this, 0, &thread_id_);
}

void HMDRenderer::RenderThread() {
  g_pose_stabilizer.LoadConfig();

  // Initialize D3D11 state
  HRESULT result = d3d12_mode_ ?
      CreateDeviceOnGameAdapter() :
      D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr,
                        0, D3D11_SDK_VERSION, &device_, 0, &device_context_);

  if (FAILED(result))
    LOGFATALF("HMDRenderer device creation error: %d\n", result);

  InitializeRenderTargets();

  // The D3D12 ring is opened (and reopened on resize) inside the loop
  if (!d3d12_mode_)
    InitializeSharedResources();

  LOGSTRF("Created render targets and shared resources\n");

  IVRSystem* hmd = hmd_support_->get_hmd();
  IVRCompositor* compositor = hmd_support_->get_compositor();

  RenderScene scene(device_);
  scene.Initialize();

  LOGSTRF("Initialized scene\n");

  // Create the panel we render to
  screen_obj = new Object(device_);
  screen_obj->InitializeVertices(&panel_verts[0], sizeof(panel_verts) / sizeof(panel_verts[0]), &panel_indices[0], sizeof(panel_indices) / sizeof(panel_indices[0]));

  scene.AddObject(screen_obj);

  // Create projection matrices
  auto mat = hmd->GetProjectionMatrix(Eye_Left, 0.1f, 100.0f, API_OpenGL);
  
  XMMATRIX proj_mat_left = XMMATRIX(
    mat.m[0][0], mat.m[0][1], mat.m[0][2], mat.m[0][3],
    mat.m[1][0], mat.m[1][1], mat.m[1][2], mat.m[1][3],
    mat.m[2][0], mat.m[2][1], mat.m[2][2], mat.m[2][3],
    mat.m[3][0], mat.m[3][1], mat.m[3][2], mat.m[3][3]
  );

  auto matr = hmd->GetProjectionMatrix(Eye_Right, 0.1f, 100.0f, API_OpenGL);
  
  XMMATRIX proj_mat_right = XMMATRIX(
    matr.m[0][0], matr.m[0][1], matr.m[0][2], matr.m[0][3],
    matr.m[1][0], matr.m[1][1], matr.m[1][2], matr.m[1][3],
    matr.m[2][0], matr.m[2][1], matr.m[2][2], matr.m[2][3],
    matr.m[3][0], matr.m[3][1], matr.m[3][2], matr.m[3][3]
  );

  bool rendering_ = true;

  device_context_->RSSetViewports(1, &viewport_);
  while (rendering_) {
    // Main render loop

    // Consume textures from queue
    FrameTexture* tex = nullptr;
    //while (texture_queue_.try_dequeue(tex)) {
    //  // Empty the rest of the queue for now..
    //  sorted_frame_textures_.push_back(tex);
    //}

    // D3D12 game: show the newest frame the GPU finished copying
    if (d3d12_mode_) {
      frameMutex.lock();

      D3D12Capture* capture = D3D12Capture::Singleton();

      int slot = -1;
      if (capture->SyncSharedTextures(device_, d3d12_textures_, &d3d12_generation_))
        slot = capture->AcquireLatestFrame(d3d12_generation_);

      if (slot == -1) {
        // Resized or not started yet, the old textures are gone
        d3d12_slot_ = -1;

        frameMutex.unlock();
        WaitGetPoses();
        posesUpdated = true;

        continue;
      }

      if (slot != d3d12_slot_) {
        ID3D11ShaderResourceView* old_view = d3d12_slot_ != -1 ? screen_obj->GetTexture() : nullptr;

        if (screen_obj->SetTexture(d3d12_textures_[slot])) {
          if (old_view != nullptr)
            old_view->Release();
        } else if (d3d12_slot_ == -1) {
          LOGSTRF("HMDRenderer failed to create a view of shared texture %d\n", slot);

          frameMutex.unlock();
          WaitGetPoses();
          posesUpdated = true;

          continue;
        }

        d3d12_slot_ = slot;
      }

      // The screen stays locked in front of the head, like the D3D11 path
      auto cam = scene.GetCamera();
      cam->SetViewMatrix(view_mat_);

      screen_obj->position.x = cam->position.x + cam->GetForwardVector().x * 2.0f * zs_;
      screen_obj->position.y = cam->position.y + cam->GetForwardVector().y * 2.0f * zs_;
      screen_obj->position.z = cam->position.z + cam->GetForwardVector().z * 2.0f * zs_;

      screen_obj->focus.x = cam->position.x;
      screen_obj->focus.y = cam->position.y;
      screen_obj->focus.z = cam->position.z;

      screen_obj->up = cam->up;
    }
    // Render earliest frames, no skip
    else if (frame_hueristic_ == 0) {
      FrameTexture* early_tex = nullptr;
      unsigned int early_index;
      if (!sorted_frame_textures_.empty()) {
        // Make the old frame texture available again
        if (last_frame_texture_ != nullptr) {
          last_frame_texture_->consumed = false;
          last_frame_texture_->time = UINT_MAX;
        }

        // Find the earliest frame texture
        unsigned int earliest = UINT_MAX;

        for (unsigned int i = 0; i < sorted_frame_textures_.size(); ++i) {
          auto& ft = sorted_frame_textures_[i];

          if (ft->time < earliest) {
            early_tex = ft;
            early_index = i;

            earliest = ft->time;
          }
        }

        sorted_frame_textures_.erase(sorted_frame_textures_.begin() + early_index);

        // Free the rest of the frames
        //for (unsigned int i = 0; i < sorted_frame_textures_.size(); ++i) {
  //        auto& ft = sorted_frame_textures_[i];

  //        ft->consumed = false;
  //        ft->time = UINT_MAX;
  //      }

  //      sorted_frame_textures_.clear();

        // Apply the texture to the screen
        screen_obj->SetTexture(early_tex->shared_texture);

        // Keep a reference to the last frame texture used
        // so we can make it available again when we no longer need it
        last_frame_texture_ = early_tex;

        // Since the texture was updated, relocate the screen
        auto cam = scene.GetCamera();
        cam->SetViewMatrix(view_mat_);

        screen_obj->position.x = cam->position.x + cam->GetForwardVector().x * 2.0f;
        screen_obj->position.y = cam->position.y + cam->GetForwardVector().y * 2.0f;
        screen_obj->position.z = cam->position.z + cam->GetForwardVector().z * 2.0f;

        screen_obj->focus.x = cam->position.x;
        screen_obj->focus.y = cam->position.y;
        screen_obj->focus.z = cam->position.z;

        screen_obj->up = cam->up;
      }
      else {
        if (last_frame_texture_ == nullptr)
          continue;

        auto cam = scene.GetCamera();
        cam->SetViewMatrix(view_mat_);
      }
    } 
    // Render earliest frame, skip extra frames
    else if (frame_hueristic_ == 1) {
      FrameTexture* early_tex = nullptr;
      unsigned int early_index;
      if (!sorted_frame_textures_.empty()) {
        // Make the old frame texture available again
        if (last_frame_texture_ != nullptr) {
          last_frame_texture_->consumed = false;
          last_frame_texture_->time = UINT_MAX;
        }

        // Find the earliest frame texture
        unsigned int earliest = UINT_MAX;

        for (unsigned int i = 0; i < sorted_frame_textures_.size(); ++i) {
          auto& ft = sorted_frame_textures_[i];

          if (ft->time < earliest) {
            early_tex = ft;
            early_index = i;

            earliest = ft->time;
          }
        }

        sorted_frame_textures_.erase(sorted_frame_textures_.begin() + early_index);

        // Free the rest of the frames
        for (unsigned int i = 0; i < sorted_frame_textures_.size(); ++i) {
          auto& ft = sorted_frame_textures_[i];

          ft->consumed = false;
          ft->time = UINT_MAX;
        }

        sorted_frame_textures_.clear();

        // Apply the texture to the screen
        screen_obj->SetTexture(early_tex->shared_texture);

        // Keep a reference to the last frame texture used
        // so we can make it available again when we no longer need it
        last_frame_texture_ = early_tex;

        // Since the texture was updated, relocate the screen
        auto cam = scene.GetCamera();
        cam->SetViewMatrix(view_mat_);

        screen_obj->position.x = cam->position.x + cam->GetForwardVector().x * 2.0f;
        screen_obj->position.y = cam->position.y + cam->GetForwardVector().y * 2.0f;
        screen_obj->position.z = cam->position.z + cam->GetForwardVector().z * 2.0f;

        screen_obj->focus.x = cam->position.x;
        screen_obj->focus.y = cam->position.y;
        screen_obj->focus.z = cam->position.z;

        screen_obj->up = cam->up;
      }
      else {
        if (last_frame_texture_ == nullptr)
          continue;

        auto cam = scene.GetCamera();
        cam->SetViewMatrix(view_mat_);
      }
    }
    // Render last frame, skip extra frames
    else if (frame_hueristic_ == 2) {
      frameMutex.lock();
      if (hasNewFrame) {
        // Apply the texture to the screen
        screen_obj->SetTexture(last_frame_texture_->shared_texture);

        // Since the texture was updated, relocate the screen
        auto cam = scene.GetCamera();
        cam->SetViewMatrix(view_mat_);

        if (!spinlock_ && waitedExtraFrame <= 0) {
          screen_obj->position.x = cam->position.x + cam->GetForwardVector().x * 2.0f * zs_;
          screen_obj->position.y = cam->position.y + cam->GetForwardVector().y * 2.0f * zs_;
          screen_obj->position.z = cam->position.z + cam->GetForwardVector().z * 2.0f * zs_;

          screen_obj->focus.x = cam->position.x;
          screen_obj->focus.y = cam->position.y;
          screen_obj->focus.z = cam->position.z;

          screen_obj->up = cam->up;
        }

        if (last_frame_texture_ == &texture_buffer_[0]) {
          last_frame_texture_ = &texture_buffer_[0];
        }
        else {
          last_frame_texture_ = &texture_buffer_[1];
        }

        waitedExtraFrame--;
      }
      else {
        if (last_frame_texture_ == nullptr) {
          frameMutex.unlock();
          WaitGetPoses();
          posesUpdated = true;

          continue;
        }

        auto cam = scene.GetCamera();
        cam->SetViewMatrix(view_mat_);

        screen_obj->SetTexture(last_frame_texture_->shared_texture);

        waitedExtraFrame--;
      }
      /*
      
      FrameTexture* late_tex = nullptr;
      unsigned int late_index;
      if (!sorted_frame_textures_.empty()) {
        // Make the old frame texture available again
        if (last_frame_texture_ != nullptr) {
          last_frame_texture_->consumed = false;
          last_frame_texture_->time = 0;
        }

        // Find the earliest frame texture
        unsigned int latest = 0;

        for (unsigned int i = 0; i < sorted_frame_textures_.size(); ++i) {
          auto& ft = sorted_frame_textures_[i];

          if (ft->time > latest) {
            late_tex = ft;
            late_index = i;

            latest = ft->time;
          }
        }

        sorted_frame_textures_.erase(sorted_frame_textures_.begin() + late_index);

        // Free the rest of the frames
        for (unsigned int i = 0; i < sorted_frame_textures_.size(); ++i) {
          auto& ft = sorted_frame_textures_[i];

          ft->consumed = false;
          ft->time = 0;
        }

        sorted_frame_textures_.clear();

        // Apply the texture to the screen
        screen_obj->SetTexture(late_tex->shared_texture);

        // Keep a reference to the last frame texture used
        // so we can make it available again when we no longer need it
        last_frame_texture_ = late_tex;

        // Since the texture was updated, relocate the screen
        auto cam = scene.GetCamera();
        cam->SetViewMatrix(view_mat_);

        if (spinlock_ && waitedExtraFrame <= 0) {
          screen_obj->position.x = cam->position.x + cam->GetForwardVector().x * 2.0f * zs_;
          screen_obj->position.y = cam->position.y + cam->GetForwardVector().y * 2.0f * zs_;
          screen_obj->position.z = cam->position.z + cam->GetForwardVector().z * 2.0f * zs_;

          screen_obj->focus.x = cam->position.x;
          screen_obj->focus.y = cam->position.y;
          screen_obj->focus.z = cam->position.z;

          screen_obj->up = cam->up;
        }

        waitedExtraFrame--;
      }
      else {
        if (last_frame_texture_ == nullptr) {
          WaitGetPoses();
          posesUpdated = true;

          continue;
        }

        auto cam = scene.GetCamera();
        cam->SetViewMatrix(view_mat_);

        screen_obj->SetTexture(last_frame_texture_->shared_texture);

        waitedExtraFrame = 1;
      }
      */
    }
    // Render last frame, skip extra frames
    else if (frame_hueristic_ == 3) {
      FrameTexture* late_tex = nullptr;
      unsigned int late_index;
      if (!sorted_frame_textures_.empty()) {
        // Make the old frame texture available again
        if (last_frame_texture_ != nullptr) {
          last_frame_texture_->consumed = false;
          last_frame_texture_->time = 0;
        }

        // Find the earliest frame texture
        unsigned int latest = 0;

        for (unsigned int i = 0; i < sorted_frame_textures_.size(); ++i) {
          auto& ft = sorted_frame_textures_[i];

          if (ft->time > latest) {
            late_tex = ft;
            late_index = i;

            latest = ft->time;
          }
        }

        sorted_frame_textures_.erase(sorted_frame_textures_.begin() + late_index);
        
        //
        // Free the rest of the frames
        //for (unsigned int i = 0; i < sorted_frame_textures_.size(); ++i) {
        //  auto& ft = sorted_frame_textures_[i];

        //  ft->consumed = false;
        //  ft->time = 0;
        //}

        //sorted_frame_textures_.clear();
        //

        // Apply the texture to the screen
        screen_obj->SetTexture(late_tex->shared_texture);

        // Keep a reference to the last frame texture used
        // so we can make it available again when we no longer need it
        last_frame_texture_ = late_tex;

        // Since the texture was updated, relocate the screen
        auto cam = scene.GetCamera();
        cam->SetViewMatrix(view_mat_);

        screen_obj->position.x = cam->position.x + cam->GetForwardVector().x * 2.0f;
        screen_obj->position.y = cam->position.y + cam->GetForwardVector().y * 2.0f;
        screen_obj->position.z = cam->position.z + cam->GetForwardVector().z * 2.0f;

        screen_obj->focus.x = cam->position.x;
        screen_obj->focus.y = cam->position.y;
        screen_obj->focus.z = cam->position.z;

        screen_obj->up = cam->up;
      }
      else {
        if (last_frame_texture_ == nullptr)
          continue;

        auto cam = scene.GetCamera();
        cam->SetViewMatrix(view_mat_);
      }
    }

    // Pass data to the renderer for left eye pass
    {
      scene.GetCamera()->SetProjectionMatrix(proj_mat_left);

      auto eye_mat_l = ConvertSteamVRMatrixToMatrix4(hmd->GetEyeToHeadTransform(Eye_Left)).invert();

      XMMATRIX eye_matrix_left = eye_mat_l.getDXMatrix();

      scene.GetCamera()->SetEyeMatrix(eye_matrix_left);
      scene.GetCamera()->SetViewMatrix(view_mat_);

      scene.RenderFrame(&rt_view_left_, &depth_view_left_, &depth_state_left_);
    }

    // Pass data to the renderer for right eye pass
    {
      scene.GetCamera()->SetProjectionMatrix(proj_mat_right);
      auto eye_mat_r = ConvertSteamVRMatrixToMatrix4(hmd->GetEyeToHeadTransform(Eye_Right)).invert();

      XMMATRIX eye_matrix_right = eye_mat_r.getDXMatrix();

      scene.GetCamera()->SetEyeMatrix(eye_matrix_right);
      scene.GetCamera()->SetViewMatrix(view_mat_);

      scene.RenderFrame(&rt_view_right_, &depth_view_right_, &depth_state_right_);
    }

    device_context_->Flush();

    {
      // Pass left texture to vr compositor

      vr::Texture_t leftTex = { (void*)rt_texture_left_, vr::API_DirectX, vr::ColorSpace_Gamma };
      vr::EVRCompositorError err = compositor->Submit(Eye_Left, &leftTex);

      //if (err)
      //  LogCompositorError(err);
    }

    {
      // Pass right texture to vr compositor

      vr::Texture_t rightTex = { (void*)rt_texture_right_ , vr::API_DirectX, vr::ColorSpace_Gamma };
      vr::EVRCompositorError err = compositor->Submit(Eye_Right, &rightTex);

      //if (err)
      //  LogCompositorError(err);
    }

    if (spinlock_)
      waitedExtraFrame = 1;

    frameMutex.unlock();

    WaitGetPoses();
    posesUpdated = true;
  }

  scene.Uninitialize();

  device_->Release();
  device_context_->Release();
}

void HMDRenderer::WaitGetPoses() {
  IVRCompositor* compositor = hmd_support_->get_compositor();
  IVRSystem* hmd = hmd_support_->get_hmd();

  // Get device poses / sync with renderer
  compositor->WaitGetPoses(&poses_[0], k_unMaxTrackedDeviceCount, nullptr, 0);
  // The pose's timestamp is taken HERE, before any ini reads or log writes
  // below, which can take a few ms and would otherwise date the sample late.
  double tPose = PoseTiming::Now();

  g_pose_stabilizer.HandleHotkeys();
  PoseTiming::Reload();

  bool updatedRightHand = false;

  for (int nDevice = 0; nDevice < vr::k_unMaxTrackedDeviceCount; ++nDevice) {
    if (poses_[nDevice].bPoseIsValid) {
      auto pose_mat = poses_[nDevice].mDeviceToAbsoluteTracking;

      switch (hmd->GetTrackedDeviceClass(nDevice)) {
      case TrackedDeviceClass_HMD:
      {
        Matrix4 raw = ConvertSteamVRMatrixToMatrix4(pose_mat);

        // Pose guard: a tracking hiccup (common with phone-based headsets)
        // can deliver a pose that is NaN, kilometres away or jumps metres in
        // one frame. The camera math
        // multiplies that straight into the game camera (flung far away ->
        // unstreamed world, underwater fog, violent shake), so hold the last
        // good pose instead and log it. A head cannot move 0.5 m in one frame.
        static bool haveLast = false;
        static Matrix4 lastGood;
        static XMFLOAT3 lastPos = { 0.0f, 0.0f, 0.0f };
        static unsigned long total = 0, rejected = 0, consecutive = 0;
        static int lastResult = -1;
        static ULONGLONG lastLog = 0;
        static XMFLOAT3 hop = { 0.0f, 0.0f, 0.0f }; // absorbed tracking hops, subtracted from the pose
        static unsigned long hops = 0;

        ++total;
        XMFLOAT3 p = raw.GetPosition();
        int result = (int)poses_[nDevice].eTrackingResult;
        const char* reason = nullptr;
        bool rawRepeated = PoseTiming::NoteRaw(tPose, raw.get(), result == TrackingResult_Running_OK);

        if (result != lastResult) {
          // Tracking coming BACK is a re-baseline, so drop the offset
          // here too. The driver usually reports recovery before it delivers
          // the relocated position, so catching it on this edge clears the
          // offset a frame or two earlier than the jump test below would, and
          // covers a recentre that lands too close to the old pose to trip it.
          if (lastResult != -1 && result == TrackingResult_Running_OK &&
              (hop.x != 0.0f || hop.y != 0.0f || hop.z != 0.0f)) {
            LOGSTRF("PoseGuard: tracking recovered (%d -> %d) - dropping carried offset (%.2f,%.2f,%.2f); the new pose is the baseline now\n",
                    lastResult, result, hop.x, hop.y, hop.z);
            hop.x = hop.y = hop.z = 0.0f;
            RoomScale::RequestReanchor(); // new tracking frame: amplified travel restarts here
          }
          LOGSTRF("PoseGuard: HMD tracking result %d -> %d\n", lastResult, result);
          lastResult = result;
        }

        if (!(p.x == p.x && p.y == p.y && p.z == p.z) || fabsf(p.x) > 50.0f || fabsf(p.y) > 50.0f || fabsf(p.z) > 50.0f)
          reason = "position not finite / >50 m from origin";
        else if (result != TrackingResult_Running_OK)
          reason = "tracking result not Running_OK";
        else if (haveLast) {
          // A tracking "hop": the position estimate relocates by more than a
          // head can move in one frame. Holding the old pose and then snapping
          // to the new one is itself a visible cut, so absorb the hop instead:
          // carry it as an offset that keeps the view where it was, and bleed
          // it away. The offset is bounded three ways below (reset detection,
          // a cap, proportional bleed) so it can never grow into metres.
          float dx = p.x - lastPos.x, dy = p.y - lastPos.y, dz = p.z - lastPos.z;
          float jump = sqrtf(dx * dx + dy * dy + dz * dz);

          if (jump > 0.5f) {
            // A big jump is not a hop, it is a RESET. A driver re-establishing
            // tracking typically hands back a clean seated origin, and the
            // destination gives it away: x/z at zero, y at eye height.
            // Absorbing a recentre would be exactly backwards - the harder the
            // tracker recentres, the further off the view would sit. Instead
            // DROP the carried offset and re-baseline: the new origin is the
            // truth and the old offset describes a frame of reference that no
            // longer exists.
            bool nearOrigin = fabsf(p.x) < 0.30f && fabsf(p.z) < 0.30f;
            if (jump > 2.0f || nearOrigin) {
              LOGSTRF("PoseGuard: %.2f m jump to (%.2f,%.2f,%.2f) reads as a tracking RESET, not a hop%s - dropping carried offset (%.2f,%.2f,%.2f) and re-baselining\n",
                      jump, p.x, p.y, p.z, nearOrigin ? " (landed on origin)" : "",
                      hop.x, hop.y, hop.z);
              hop.x = hop.y = hop.z = 0.0f;
              RoomScale::RequestReanchor(); // new tracking frame: amplified travel restarts here
            } else {
              hop.x += dx; hop.y += dy; hop.z += dz;
              ++hops;

              // CLAMP: unbounded, many small hops would compound into tens
              // of metres. Past a rotation of the head the offset stops being
              // a correction and becomes the fault, so cap it and log when
              // the cap bites.
              const float MAX_CARRY = 0.30f;
              float carry = sqrtf(hop.x * hop.x + hop.y * hop.y + hop.z * hop.z);
              if (carry > MAX_CARRY) {
                float s = MAX_CARRY / carry;
                LOGSTRF("PoseGuard: carried offset hit the %.2f m cap (was %.2f m) - clamping; the view will not chase further corruption\n",
                        MAX_CARRY, carry);
                hop.x *= s; hop.y *= s; hop.z *= s;
                carry = MAX_CARRY;
              }

              LOGSTRF("PoseGuard: absorbed a %.2f m tracking hop (%.2f,%.2f,%.2f)->(%.2f,%.2f,%.2f), carried offset now (%.2f,%.2f,%.2f) = %.2f m - hop #%lu of %lu poses\n",
                      jump, lastPos.x, lastPos.y, lastPos.z, p.x, p.y, p.z,
                      hop.x, hop.y, hop.z, carry, hops, total);
            }
          }
        }

        // Never get stuck: if the new place persists for ~1 s, accept it
        if (reason != nullptr && consecutive >= 90 && p.x == p.x && p.y == p.y && p.z == p.z && fabsf(p.x) < 50.0f && fabsf(p.y) < 50.0f && fabsf(p.z) < 50.0f) {
          LOGSTRF("PoseGuard: new pose persisted for %lu frames, accepting it as the baseline\n", consecutive);
          reason = nullptr;
        }

        if (reason != nullptr) {
          ++rejected;
          ++consecutive;
          if (haveLast)
            raw = lastGood;

          ULONGLONG now = GetTickCount64();
          if (now - lastLog > 2000) {
            lastLog = now;
            LOGSTRF("PoseGuard: rejected HMD pose (%s) pos=(%.2f,%.2f,%.2f) last=(%.2f,%.2f,%.2f) result=%d - %lu of %lu poses rejected so far\n",
                    reason, p.x, p.y, p.z, lastPos.x, lastPos.y, lastPos.z, result, rejected, total);
          }

          if (!haveLast)
            break; // nothing trustworthy yet, keep the previous view matrix
        } else {
          lastPos = p; // raw, so the next hop is measured against real tracking

          // Bleed the offset PROPORTIONALLY, not at a fixed rate. Draining on
          // a time constant means recovery time is roughly constant regardless
          // of the offset's size - a big one clears as quickly as a small one.
          // A 2 cm/s FLOOR makes the last millimetres finish in finite time
          // instead of approaching zero asymptotically.
          static ULONGLONG lastBleed = 0;
          ULONGLONG nowBleed = GetTickCount64();
          float dtBleed = lastBleed ? (float)(nowBleed - lastBleed) / 1000.0f : 0.0f;
          lastBleed = nowBleed;
          float hopLen = sqrtf(hop.x * hop.x + hop.y * hop.y + hop.z * hop.z);
          if (hopLen > 0.0001f && dtBleed > 0.0f && dtBleed < 0.5f) {
            const float BLEED_TAU = 2.0f;                         // seconds
            float rate = hopLen / BLEED_TAU;                      // m/s
            if (rate < 0.02f) rate = 0.02f;                       // floor
            float keep = hopLen - rate * dtBleed;
            keep = keep < 0.0f ? 0.0f : keep / hopLen;
            hop.x *= keep; hop.y *= keep; hop.z *= keep;
          }

          raw[12] -= hop.x;
          raw[13] -= hop.y;
          raw[14] -= hop.z;

          lastGood = raw;
          haveLast = true;
          consecutive = 0;
        }

        Matrix4 mat = g_pose_stabilizer.Filter(raw);
        Matrix4 tracked = mat; // real metres, before RoomScale

        // Amplified floor-plane walking. LAST, after PoseGuard and the
        // stabilizer, so their thresholds stay in real metres. See RoomScale.hpp.
        RoomScale::Apply(mat);

        // Every processed pose goes into PoseTiming's ring, timestamped, so
        // the game thread can sample it at its own frame time (PoseTiming.hpp).
        // A repeat of the previous raw pose (the compositor handing back the same
        // driver pose) is skipped, or it would show as a brief stall; during a
        // real freeze one sample still goes in every 50 ms.
        if (!rawRepeated || PoseTiming::SinceLastPush(tPose) > 0.05)
          PoseTiming::Push(tPose, mat.get(), tracked.get());

        mat.invert();

        view_mat_ = mat.getDXMatrix();

        // PoseTiming 1/2: the game thread writes hmd_matrix_ in SyncOnPoses.
        if (PoseTiming::Mode() == 0)
          hmd_support_->hmd_matrix_ = mat;

        break;
      }

      case TrackedDeviceClass_Controller:
      {
        Matrix4 mat = ConvertSteamVRMatrixToMatrix4(pose_mat);

        if (!updatedRightHand) {
          hmd_support_->right_controller_.SetMatrix(mat);
          hmd_support_->right_controller_.SetDeviceIndex(nDevice);

          updatedRightHand = true;
        } else {
          hmd_support_->left_controller_.SetMatrix(mat);
          hmd_support_->left_controller_.SetDeviceIndex(nDevice);
        }

        break;
      }
      }
    }
  }
}

void HMDRenderer::WaitGetPosesSimple() {
  // If the poses were updated, we have a new pose to use
  if (posesUpdated) {
    posesUpdated = false;

    return;
  }

  // The render thread never started (no VR, or no frames hooked), there
  // is nothing to sync on. Spinning here would hang the whole game.
  if (!jfawklejf)
    return;

  // Otherwise, we should block until poses are ready
  ULONGLONG start = GetTickCount64();
  unsigned int spins = 0;

  while (!posesUpdated) {
    if ((++spins & 0xFFFF) == 0 && GetTickCount64() - start > 250)
      break;
  }

  posesUpdated = false;
}