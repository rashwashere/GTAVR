// Stub ScriptHookV.dll for offline tracing of GTAVive.asi.
// Exports the same mangled symbols the .asi imports; every native call is
// logged to native_trace.log. Scenario behavior (native return values,
// simulated key presses) is selected via the GTAVIVE_SCENARIO env var:
//   idle (default), vehicle, fire, teleport, teleport_veh, melee, fly
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#pragma comment(lib, "user32.lib") // keybd_event

static FILE* trace = nullptr;
static void (*g_scriptMain)() = nullptr;
static void (*g_keyHandler)(DWORD, WORD, BYTE, BOOL, BOOL, BOOL, BOOL) = nullptr;
static int g_frames = 0;
static unsigned __int64 g_retbuf[32];
static char g_scenario[64] = "idle";
static bool g_scenarioRead = false;

static unsigned __int64 g_lastHash = 0;
static int g_pushIndex = 0;
static unsigned __int64 g_pushes[32];
static int g_shapeTestCalls = 0;

static const unsigned __int64 H_GET_PLAYER_PED = 0x43A66C31C68491C0;
static const unsigned __int64 H_GET_ENTITY_COORDS = 0x3FEF770D40960D5A;
static const unsigned __int64 H_GET_ENTITY_ROTATION = 0xAFBD61CC738D9EB9;
static const unsigned __int64 H_GET_ENTITY_VELOCITY = 0x4805D2B1D8CF94A9;
static const unsigned __int64 H_GET_ENTITY_FORWARD = 0x0A794A5A57F8DF91;
static const unsigned __int64 H_GET_ENTITY_TYPE = 0x8ACD366038D14505;
static const unsigned __int64 H_IS_ENTITY_A_VEHICLE = 0x6AC7003FA6E5575E;
static const unsigned __int64 H_IS_PED_IN_ANY_VEHICLE = 0x997ABD671D25CA0B;
static const unsigned __int64 H_GET_VEHICLE_PED_IS_USING = 0x6094AD011A2EA87D;
static const unsigned __int64 H_GET_PED_IN_VEHICLE_SEAT = 0xBB40DD2270B65366;
static const unsigned __int64 H_GET_PED_BONE_COORDS = 0x17C07FC640E86B4E;
static const unsigned __int64 H_CREATE_OBJECT = 0x509D5878EB39E842;
static const unsigned __int64 H_DELETE_OBJECT = 0x539E0AE3E6634B9F;
static const unsigned __int64 H_CREATE_CAM = 0xB51194800B257161;
static const unsigned __int64 H_GET_WEAPONTYPE_MODEL = 0xF46CDC33180FDA94;
static const unsigned __int64 H_GET_HASH_KEY = 0xD24D37CC275948CC;
static const unsigned __int64 H_HAS_WEAPON_ASSET_LOADED = 0x36E353271F0E90EE;
static const unsigned __int64 H_GET_SHAPE_TEST_RESULT = 0x3D87450E15D98694;

static void OpenTrace()
{
	if (!trace)
		trace = fopen("native_trace.log", "w");
	if (!g_scenarioRead)
	{
		g_scenarioRead = true;
		DWORD n = GetEnvironmentVariableA("GTAVIVE_SCENARIO", g_scenario, sizeof(g_scenario));
		if (n == 0)
			strcpy(g_scenario, "idle");
	}
}

static bool Scenario(const char* name)
{
	OpenTrace();
	return strcmp(g_scenario, name) == 0;
}

extern "C" __declspec(dllexport) int GetFrameCount()
{
	return g_frames;
}

extern "C" __declspec(dllexport) const char* GetScenario()
{
	OpenTrace();
	return g_scenario;
}

void scriptWait(DWORD time)
{
	OpenTrace();
	fprintf(trace, "WAIT\n");
	fflush(trace);
	g_frames++;
	if (g_frames == 3 && g_keyHandler)
	{
		// Press a REAL Ctrl (the .asi checks GetAsyncKeyState only), then
		// press+release L through the handler. Released again below.
		keybd_event(VK_CONTROL, 0, 0, 0);
		g_keyHandler(VK_CONTROL, 1, 0, 0, 0, 0, 0);
		g_keyHandler('L', 1, 0, 0, 0, 0, 0);
		g_keyHandler('L', 1, 0, 0, 0, 1, 1);
	}
	if (g_frames == 8 && g_keyHandler)
	{
		keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
		g_keyHandler(VK_CONTROL, 1, 0, 0, 0, 1, 1);
	}
	if (g_frames == 5 && Scenario("fly") && g_keyHandler)
	{
		g_keyHandler('1', 1, 0, 0, 0, 0, 0);
		g_keyHandler('1', 1, 0, 0, 0, 1, 1);
	}
	if (g_frames > 40)
	{
		fprintf(trace, "END\n");
		fflush(trace);
		fclose(trace);
		ExitProcess(0);
	}
}

void scriptRegister(HINSTANCE module, void (*scriptMain)())
{
	g_scriptMain = scriptMain;
}

void scriptUnregister(HINSTANCE module)
{
}

void nativeInit(unsigned __int64 hash)
{
	OpenTrace();
	g_lastHash = hash;
	g_pushIndex = 0;
	fprintf(trace, "INIT %016llx\n", hash);
}

void nativePush64(unsigned __int64 val)
{
	OpenTrace();
	if (g_pushIndex < 32)
		g_pushes[g_pushIndex] = val;

	// args that are pointers (addresses differ between binaries): normalize
	bool isPtr = false;
	if (g_lastHash == H_CREATE_CAM && g_pushIndex == 0) isPtr = true;
	if (g_lastHash == H_GET_HASH_KEY && g_pushIndex == 0) isPtr = true;
	if (g_lastHash == H_DELETE_OBJECT && g_pushIndex == 0) isPtr = true;
	if (g_lastHash == H_GET_SHAPE_TEST_RESULT && g_pushIndex >= 1 && g_pushIndex <= 4) isPtr = true;

	g_pushIndex++;
	if (isPtr)
		fprintf(trace, "PUSH PTR\n");
	else
		fprintf(trace, "PUSH %llx\n", val);
}

static void SetFloat3(float x, float y, float z)
{
	float* f = reinterpret_cast<float*>(g_retbuf);
	f[0] = x;
	f[2] = y;
	f[4] = z;
}

unsigned __int64* nativeCall()
{
	OpenTrace();
	fprintf(trace, "CALL\n");
	fflush(trace);
	memset(g_retbuf, 0, sizeof(g_retbuf));

	switch (g_lastHash)
	{
	case H_GET_PLAYER_PED:
		g_retbuf[0] = 3;
		break;
	case H_GET_ENTITY_COORDS:
		SetFloat3(100.0f, 200.0f, 3.0f);
		break;
	case H_GET_ENTITY_ROTATION:
		SetFloat3(0.0f, 0.0f, 90.0f);
		break;
	case H_GET_ENTITY_VELOCITY:
		SetFloat3(1.0f, 2.0f, 3.0f);
		break;
	case H_GET_ENTITY_FORWARD:
		SetFloat3(0.6f, 0.8f, 0.0f);
		break;
	case H_GET_PED_BONE_COORDS:
		SetFloat3(100.5f, 200.5f, 4.0f);
		break;
	case H_IS_PED_IN_ANY_VEHICLE:
		g_retbuf[0] = Scenario("vehicle") ? 1 : 0;
		break;
	case H_GET_VEHICLE_PED_IS_USING:
		g_retbuf[0] = 5;
		break;
	case H_GET_PED_IN_VEHICLE_SEAT:
		g_retbuf[0] = 8;
		break;
	case H_IS_ENTITY_A_VEHICLE:
		g_retbuf[0] = (g_pushes[0] == 7) ? 1 : 0;
		break;
	case H_GET_ENTITY_TYPE:
		g_retbuf[0] = (g_pushes[0] == 9) ? 1 : 0;
		break;
	case H_CREATE_OBJECT:
		g_retbuf[0] = 42;
		break;
	case H_CREATE_CAM:
		g_retbuf[0] = 77;
		break;
	case H_GET_WEAPONTYPE_MODEL:
		g_retbuf[0] = (g_pushes[0] & 0xffffffff) ^ 0x55;
		break;
	case H_GET_HASH_KEY:
		g_retbuf[0] = 0xDEADBEEF;
		break;
	case H_HAS_WEAPON_ASSET_LOADED:
		g_retbuf[0] = 1;
		break;
	case H_GET_SHAPE_TEST_RESULT:
	{
		g_shapeTestCalls++;
		bool hit;
		int entity;
		if (Scenario("melee"))
		{
			hit = true;
			entity = 9;
		}
		else if (Scenario("teleport_veh"))
		{
			hit = (g_shapeTestCalls % 3) == 0;
			entity = 7;
		}
		else
		{
			hit = (g_shapeTestCalls % 3) == 0;
			entity = 0;
		}
		BOOL* pHit = reinterpret_cast<BOOL*>(g_pushes[1]);
		float* pEnd = reinterpret_cast<float*>(g_pushes[2]);
		float* pNormal = reinterpret_cast<float*>(g_pushes[3]);
		int* pEntity = reinterpret_cast<int*>(g_pushes[4]);
		if (pHit) *pHit = hit ? 1 : 0;
		if (pEnd) { pEnd[0] = 10.0f; pEnd[2] = 20.0f; pEnd[4] = 30.0f; }
		if (pNormal) { pNormal[0] = 0.0f; pNormal[2] = 0.0f; pNormal[4] = 1.0f; }
		if (pEntity) *pEntity = entity;
		g_retbuf[0] = 0;
		break;
	}
	default:
		break;
	}
	return g_retbuf;
}

typedef void (*TKeyboardFn)(DWORD, WORD, BYTE, BOOL, BOOL, BOOL, BOOL);

void keyboardHandlerRegister(TKeyboardFn function)
{
	g_keyHandler = function;
}

void keyboardHandlerUnregister(TKeyboardFn function)
{
}

extern "C" __declspec(dllexport) void* GetScriptMain()
{
	return (void*)g_scriptMain;
}

#pragma comment(linker, "/export:?scriptWait@@YAXK@Z")
#pragma comment(linker, "/export:?scriptRegister@@YAXPEAUHINSTANCE__@@P6AXXZ@Z")
#pragma comment(linker, "/export:?scriptUnregister@@YAXPEAUHINSTANCE__@@@Z")
#pragma comment(linker, "/export:?nativeInit@@YAX_K@Z")
#pragma comment(linker, "/export:?nativePush64@@YAX_K@Z")
#pragma comment(linker, "/export:?nativeCall@@YAPEA_KXZ")
#pragma comment(linker, "/export:?keyboardHandlerRegister@@YAXP6AXKGEHHHH@Z@Z")
#pragma comment(linker, "/export:?keyboardHandlerUnregister@@YAXP6AXKGEHHHH@Z@Z")

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID)
{
	return TRUE;
}
