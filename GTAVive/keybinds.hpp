#pragma once

// Rebindable keyboard functions.
//
// GTAVive's own binds live in gtaovr\gtavive.ini [Keys]; the stabilizer and room scale binds live in
// gtaovr\stabilizer.ini [Hotkeys] and are handled by OVRInject (GTAVive only shows and edits them).
// Both files are re-read at most once a second, and at once after a change made in the settings menu.

#include "Vive/Hotkey.hpp"

enum BindId
{
	// gtavive.ini [Keys]
	BIND_ENTER_VR,
	BIND_VR_OFF,
	BIND_RECENTRE,
	BIND_FLY_MOVE,
	BIND_LEFT_HAND_STEERING,
	BIND_MELEE_CALIBRATION_DUMP,
	BIND_OPEN_MENU,
	BIND_MENU_UP,
	BIND_MENU_DOWN,
	BIND_MENU_LEFT,
	BIND_MENU_RIGHT,
	BIND_MENU_SELECT,
	BIND_MENU_BACK,
	BIND_GTAVIVE_COUNT,

	// stabilizer.ini [Hotkeys]
	BIND_STAB_TOGGLE = BIND_GTAVIVE_COUNT,
	BIND_STAB_RELOAD,
	BIND_STAB_PRESET,
	BIND_STAB_TARGET,
	BIND_STAB_UP,
	BIND_STAB_DOWN,
	BIND_STAB_SAVE,
	BIND_ROOMSCALE_REANCHOR,
	BIND_COUNT
};

struct BindInfo
{
	const char* iniKey;
	const char* defaultText;
	const char* label;   // menu row
	const char* help;    // menu help line
};

const BindInfo& GetBindInfo(BindId id);
bool IsStabilizerBind(BindId id);

// Re-reads both files when a second has passed or the menu changed a value; force = always.
void LoadKeyBinds(bool force = false);

const OVRInject::HotkeyBind& GetBind(BindId id);
void GetBindText(BindId id, char* out, size_t outSize);

// Writes a new bind to its file and applies it at once.
void SetBind(BindId id, const OVRInject::HotkeyBind& bind);
void ResetAllBindsToDefaults();

// The first other bind with the same key and modifiers, or BIND_COUNT when the bind is unique.
BindId FindDuplicateBind(BindId id);

// While suppressed (settings menu open) BindPressed always returns false.
void SetBindsSuppressed(bool suppressed);

// True on the tick the bind's key is released with exactly the bind's modifiers held.
// Needs UpdateKeyReleases() earlier in the same tick.
bool BindPressed(BindId id);
bool BindPressedIgnoringSuppression(BindId id);

// True while the bind's key is physically down with exactly its modifiers (menu navigation, hold to repeat).
bool BindHeld(BindId id);
