// Loads a GTAVive.asi (with the stub ScriptHookV.dll/OVRInject.dll beside it)
// and runs its registered ScriptMain. The scriptWait stub feeds an 'L' key
// press on frame 3 and terminates the process after 40 frames, leaving
// native_trace.log behind.
#include <windows.h>
#include <cstdio>

int main(int argc, char** argv)
{
	const char* asi = argc > 1 ? argv[1] : "GTAVive.asi";

	HMODULE lib = LoadLibraryA(asi);
	if (!lib)
	{
		printf("LoadLibrary(%s) failed: %lu\n", asi, GetLastError());
		return 1;
	}

	HMODULE shv = GetModuleHandleA("ScriptHookV.dll");
	if (!shv)
	{
		printf("stub ScriptHookV.dll not loaded\n");
		return 2;
	}

	typedef void* (*GetScriptMainFn)();
	GetScriptMainFn getMain = (GetScriptMainFn)GetProcAddress(shv, "GetScriptMain");
	if (!getMain || !getMain())
	{
		printf("no ScriptMain registered\n");
		return 3;
	}

	printf("running ScriptMain (%s)...\n", asi);
	((void (*)())getMain())();
	return 0;
}
