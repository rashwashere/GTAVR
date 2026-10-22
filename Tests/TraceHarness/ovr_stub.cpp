// Stub OVRInject.dll for offline tracing of GTAVive.asi.
// Exports the 9 mangled symbols the .asi imports and hands out a fake
// HMDSupport block laid out like the real one (matrix @0x10, controllers
// @0x90/@0x138, chaperone* @0x1f0, mirroring @0x210) so the .asi's inlined
// accessors read sane values.
#include <windows.h>
#include <DirectXMath.h>
#include <cstring>

using namespace DirectX;

class Matrix4
{
public:
	float m[16];
	float tm[16];
};

namespace OVRInject
{
	class TrackedController
	{
	public:
		struct ButtonState
		{
			float touchX;
			float touchY;
			bool touchContact;
			bool touchJustContacted;
			bool touchJustReleased;
			bool padPressed;
			bool padJustPressed;
			bool padJustReleased;
			bool gripPressed;
			bool gripJustPressed;
			bool gripJustReleased;
			float triggerMargin;
			bool triggerPressed;
			bool triggerJustPressed;
			bool triggerJustReleased;
			bool menuPressed;
			bool menuJustPressed;
			bool menuJustReleased;
			bool valid_;
		};

		__declspec(dllexport) Matrix4 GetMatrix();
		__declspec(dllexport) XMFLOAT3 GetPosition();
		__declspec(dllexport) XMFLOAT3 GetRotation(float rx = 0.0f, float ry = 0.0f, float rz = 0.0f);
		__declspec(dllexport) void UpdateButtonState();
		__declspec(dllexport) ButtonState GetButtonState();
	};

	class HMDSupport
	{
	public:
		__declspec(dllexport) static HMDSupport* Singleton();
		__declspec(dllexport) void SyncOnPoses();
		__declspec(dllexport) void SetDesktopMirroring(bool enable);
		__declspec(dllexport) bool GetDesktopMirroring();
	};
}

using namespace OVRInject;

// ---- scenario plumbing (frame counter + scenario name live in the SHV stub) ----

static int Frame()
{
	static int (*getFrames)() = nullptr;
	if (!getFrames)
	{
		HMODULE shv = GetModuleHandleA("ScriptHookV.dll");
		if (shv)
			getFrames = (int (*)())GetProcAddress(shv, "GetFrameCount");
	}
	return getFrames ? getFrames() : 0;
}

static bool Scenario(const char* name)
{
	static const char* (*getScenario)() = nullptr;
	if (!getScenario)
	{
		HMODULE shv = GetModuleHandleA("ScriptHookV.dll");
		if (shv)
			getScenario = (const char* (*)())GetProcAddress(shv, "GetScenario");
	}
	return getScenario && strcmp(getScenario(), name) == 0;
}

// ---- fake chaperone: vtable slot [2] = GetPlayAreaRect(HmdQuad_t*) ----

struct HmdQuadStub
{
	float corners[4][3];
};

static bool FakeCalibrationState(void* self)
{
	return true;
}

static bool FakePlayAreaSize(void* self, float* x, float* z)
{
	if (x) *x = 2.0f;
	if (z) *z = 2.0f;
	return true;
}

static bool FakePlayAreaRect(void* self, HmdQuadStub* quad)
{
	static const float corners[4][3] = {
		{ 1.0f, 0.0f,  1.0f },
		{ 1.0f, 0.0f, -1.0f },
		{ -1.0f, 0.0f, -1.0f },
		{ -1.0f, 0.0f,  1.0f },
	};
	memcpy(quad->corners, corners, sizeof(corners));
	return true;
}

static void* g_chaperoneVtable[8] = {
	(void*)FakeCalibrationState,
	(void*)FakePlayAreaSize,
	(void*)FakePlayAreaRect,
	nullptr, nullptr, nullptr, nullptr, nullptr,
};

struct FakeChaperone
{
	void** vtable;
};

static FakeChaperone g_chaperone = { g_chaperoneVtable };

// ---- fake HMDSupport block ----

static char g_block[0x220];
static bool g_initialized = false;

static void InitBlock()
{
	if (g_initialized)
		return;
	g_initialized = true;
	memset(g_block, 0, sizeof(g_block));

	// hmd_matrix_ @0x10: identity m + identity tm
	float* m = reinterpret_cast<float*>(g_block + 0x10);
	for (int i = 0; i < 16; i++)
	{
		m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
		m[16 + i] = (i % 5 == 0) ? 1.0f : 0.0f;
	}

	// chaperone_ @0x1f0
	*reinterpret_cast<void**>(g_block + 0x1f0) = &g_chaperone;
}

HMDSupport* HMDSupport::Singleton()
{
	InitBlock();
	return reinterpret_cast<HMDSupport*>(g_block);
}

void HMDSupport::SyncOnPoses()
{
}

void HMDSupport::SetDesktopMirroring(bool enable)
{
	*reinterpret_cast<bool*>(g_block + 0x210) = enable;
}

bool HMDSupport::GetDesktopMirroring()
{
	return *reinterpret_cast<bool*>(g_block + 0x210);
}

Matrix4 TrackedController::GetMatrix()
{
	Matrix4 result;
	for (int i = 0; i < 16; i++)
	{
		result.m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
		result.tm[i] = (i % 5 == 0) ? 1.0f : 0.0f;
	}
	return result;
}

XMFLOAT3 TrackedController::GetPosition()
{
	// drifts a little every frame so swing/extrapolation math is exercised
	float t = (float)Frame() * 0.01f;
	return XMFLOAT3(0.25f + t, 0.5f + t * 0.5f, 0.75f - t * 0.25f);
}

XMFLOAT3 TrackedController::GetRotation(float rx, float ry, float rz)
{
	return XMFLOAT3(rx, ry, rz);
}

void TrackedController::UpdateButtonState()
{
}

TrackedController::ButtonState TrackedController::GetButtonState()
{
	ButtonState state = {};
	state.valid_ = true;

	int frame = Frame();
	if (Scenario("vehicle"))
	{
		state.touchContact = true;
		state.touchX = 0.3f;
		state.touchY = 0.7f;
		state.gripPressed = frame >= 10 && frame <= 12;
	}
	else if (Scenario("fire"))
	{
		state.triggerJustPressed = frame == 12 || frame == 22 || frame == 27;
		state.triggerPressed = frame >= 16 && frame <= 19;
		state.menuJustPressed = frame == 15 || frame == 20 || frame == 25;
	}
	else if (Scenario("teleport") || Scenario("teleport_veh"))
	{
		state.padPressed = frame >= 10 && frame <= 14;
		state.padJustReleased = frame == 15;
	}
	else if (Scenario("melee"))
	{
		state.gripJustPressed = frame == 10;
		state.gripPressed = frame >= 10 && frame <= 20;
		state.gripJustReleased = frame == 21;
	}
	else if (Scenario("fly"))
	{
		state.touchContact = frame >= 8;
		state.touchX = 0.5f;
		state.touchY = 1.0f;
	}
	return state;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID)
{
	return TRUE;
}
