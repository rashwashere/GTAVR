#include "targetver.h"

#include <Windows.h>
#include <Psapi.h>
#include <tchar.h>

#include "Detour/D3D11DetourManager.hpp"
#include "D3D12Hook/D3D12Capture.hpp"

#include "Log.hpp"

using namespace OVRInject;

DetourManager* detourManager;

void init();

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
					 )
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
	{
		LOGSTRF("Attached main DLL to GTA: V\n");

		init();

		break;
	}
	case DLL_THREAD_ATTACH:
		break;
	case DLL_THREAD_DETACH:
		break;
	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}

void init()
{
	// GTA V Enhanced is D3D12 only; the D3D11 creation detours would never
	// fire there (and would sit on top of whatever dxgi.dll proxy is loaded)
	if (D3D12Capture::IsEnhancedProcess())
	{
		LOGSTRF("OVRInject: GTA V Enhanced detected, using the D3D12 capture path\n");

		D3D12Capture::Singleton()->InstallHooksAsync();

		return;
	}

	LOGSTRF("OVRInject: GTA V Legacy, using the D3D11 path\n");
	detourManager = new D3D11DetourManager();
}