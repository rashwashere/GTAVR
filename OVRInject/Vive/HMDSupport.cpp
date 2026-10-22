#include "targetver.h"

#include "HMDSupport.hpp"
#include "HMDRenderer.hpp"
#include "D3D12Hook/D3D12Capture.hpp"

#include "Log.hpp"
#include "PoseTiming.hpp"

#include <cstdio>
#include <dxgi.h>
#include <string>

using namespace OVRInject;
using namespace vr;

HMDSupport::HMDSupport() :
    device_(nullptr), renderer_(new HMDRenderer(this)),
    hmd_(nullptr), compositor_(nullptr), chaperone_(nullptr),
    desktop_mirroring_(true), vr_ready_(false) {
}

HMDSupport::~HMDSupport() {
}

bool HMDSupport::Initialize(IDXGISwapChain* swap_chain, ID3D11Device* device) {
	swap_chain_ = swap_chain;
	device_ = device;

	device_->GetImmediateContext(&device_context_);

  EVRInitError error = VRInitError_None;
  hmd_ = VR_Init(&error, VRApplication_Scene);
  compositor_ = VRCompositor();
  chaperone_ = VRChaperone();

	if (error) {
		hmd_ = NULL;

		LOGSTRF("HMDSupport Initialize error: %s", VR_GetVRInitErrorAsEnglishDescription(error));

		return false;
	}

  // Get the backbuffer so we can initialize frame textures
  // with the same parameters
  ID3D11Texture2D* pBuffer;
  swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBuffer);

  // Pass the GTA device context and backbuffer
  renderer_->Initialize(device_context_, pBuffer);

  pBuffer->Release();

	LOGSTRF("Successfully initialized HMDSupport\n");

  vr_ready_ = true;

	return true;
}

bool HMDSupport::InitializeD3D12() {
  EVRInitError error = VRInitError_None;
  hmd_ = VR_Init(&error, VRApplication_Scene);

  if (error) {
    hmd_ = NULL;

    LOGSTRF("HMDSupport Initialize error: %s\n", VR_GetVRInitErrorAsEnglishDescription(error));

    return false;
  }

  compositor_ = VRCompositor();
  chaperone_ = VRChaperone();

  renderer_->InitializeD3D12();

  LOGSTRF("Successfully initialized HMDSupport (D3D12)\n");

  vr_ready_ = true;

  return true;
}

bool HMDSupport::IsVRReady() {
  if (!vr_ready_)
    D3D12Capture::Singleton()->RequestVRRetry();

  return vr_ready_;
}

void HMDSupport::SubmitFrameTexture(int eye_index, ID3D11Texture2D* texture, const unsigned int& time)
{
  renderer_->SubmitFrameTexture(eye_index, texture, time);
}

HMDSupport* HMDSupport::Singleton() {
  static HMDSupport instance;
   
  return &instance;
}

void HMDSupport::SetFrameHueristic(int frameHuer) {
  renderer_->SetFrameHueristic(frameHuer);
}

void HMDSupport::SetSpinlock(bool enabled) {
  renderer_->SetSpinlock(enabled);
}

void HMDSupport::SetHS(float hs) {
  renderer_->SetHS(hs);
}

void HMDSupport::SetVS(float vs) {
  renderer_->SetVS(vs);
}

void HMDSupport::SetZS(float zs) {
  renderer_->SetZS(zs);
}

// Game thread, once per GTAVive script tick (see PoseTiming.hpp).
//   PoseTiming=0: wait for the next HMD pose. The HMD thread wrote hmd_matrix_.
//   PoseTiming=1/2: no wait; sample the pose ring at this frame's own time and
//   write hmd_matrix_ here, so motion follows real game frame time.
void HMDSupport::SyncOnPoses() {
  double t0 = PoseTiming::Now();
  int mode = PoseTiming::Mode();

  if (mode == 0)
    renderer_->WaitGetPosesSimple();

  double t1 = PoseTiming::Now();
  PoseTiming::Sample s;
  double age = 0.0;

  if (PoseTiming::SampleNow(mode, t1, &s, &age) && mode != 0) {
    float m[16];
    PoseTiming::MatrixFromPose(s.view, m);
    Matrix4 mat(m);
    mat.invert();
    hmd_matrix_ = mat;
  }

  PoseTiming::GameTick(t0, t1 - t0, age);
}

void HMDSupport::SetDesktopMirroring(bool enable) {
  desktop_mirroring_ = enable;
}

bool HMDSupport::GetDesktopMirroring() {
  return desktop_mirroring_;
}