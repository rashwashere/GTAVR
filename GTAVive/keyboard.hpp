#pragma once

#include "sdk/main.h"

void OnKeyboardMessage(DWORD key, WORD repeats, BYTE scanCode, BOOL isExtended, BOOL isWithAlt, BOOL wasDownBefore, BOOL isUpNow);

bool IsKeyJustUp(DWORD key);

// Reads every key's release once; call at the start of each script tick, then query with KeyReleasedThisTick.
void UpdateKeyReleases();
bool KeyReleasedThisTick(DWORD key);

// Drops the coming release of every key held right now (used when the settings menu closes).
void IgnoreReleaseOfHeldKeys();

// True while the game window has the keyboard focus.
bool IsGameWindowFocused();

// True while a Ctrl key is physically held (GetAsyncKeyState).
bool IsCtrlHeld();
