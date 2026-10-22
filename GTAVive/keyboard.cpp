// Keyboard state tracking, same scheme as the ScriptHookV SDK sample:
// the handler timestamps each key event; a key counts as "just up" if the
// up event arrived within the last 100ms.

#include "keyboard.hpp"

const int KEYS_SIZE = 255;
const DWORD NOW_PERIOD = 100;

struct KeyState
{
	DWORD time;
	BOOL isWithAlt;
	BOOL wasDownBefore;
	BOOL isUpNow;
};

static KeyState keyStates[KEYS_SIZE];

void OnKeyboardMessage(DWORD key, WORD repeats, BYTE scanCode, BOOL isExtended, BOOL isWithAlt, BOOL wasDownBefore, BOOL isUpNow)
{
	if (key < KEYS_SIZE)
	{
		keyStates[key].time = GetTickCount();
		keyStates[key].isWithAlt = isWithAlt;
		keyStates[key].wasDownBefore = wasDownBefore;
		keyStates[key].isUpNow = isUpNow;
	}
}

static void ResetKeyState(DWORD key)
{
	if (key < KEYS_SIZE)
		memset(&keyStates[key], 0, sizeof(KeyState));
}

bool IsKeyJustUp(DWORD key)
{
	bool b = key < KEYS_SIZE &&
		GetTickCount() < keyStates[key].time + NOW_PERIOD &&
		keyStates[key].isUpNow;
	if (b)
		ResetKeyState(key);
	return b;
}

// Per-tick release snapshot: every key's "just up" is read (and consumed) once per script tick, so several binds
// on the same key with different modifiers all see the same release.
static bool releasedThisTick[KEYS_SIZE];
// Keys held while the settings menu closes: their release is dropped so it cannot fire a bind afterwards.
static ULONGLONG ignoreReleaseUntil[KEYS_SIZE];

void UpdateKeyReleases()
{
	ULONGLONG now = GetTickCount64();
	for (DWORD k = 1; k < KEYS_SIZE; ++k)
	{
		bool released = IsKeyJustUp(k);
		if (ignoreReleaseUntil[k] != 0)
		{
			if (GetAsyncKeyState((int)k) & 0x8000)
				ignoreReleaseUntil[k] = now + 250;   // still held: keep ignoring until shortly after it goes up
			if (released || now >= ignoreReleaseUntil[k])
			{
				released = false;
				ignoreReleaseUntil[k] = 0;
			}
		}
		releasedThisTick[k] = released;
	}
}

bool KeyReleasedThisTick(DWORD key)
{
	return key < KEYS_SIZE && releasedThisTick[key];
}

void IgnoreReleaseOfHeldKeys()
{
	ULONGLONG now = GetTickCount64();
	for (DWORD k = 1; k < KEYS_SIZE; ++k)
	{
		releasedThisTick[k] = false;
		if (GetAsyncKeyState((int)k) & 0x8000)
			ignoreReleaseUntil[k] = now + 250;
	}
}

bool IsGameWindowFocused()
{
	HWND fg = GetForegroundWindow();
	DWORD pid = 0;
	if (fg != NULL)
		GetWindowThreadProcessId(fg, &pid);
	return pid == GetCurrentProcessId();
}

bool IsCtrlHeld()
{
	// Physical key state only, so a Ctrl-down whose release event never
	// reached the keyboard handler cannot leave Ctrl latched as held.
	return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
}
