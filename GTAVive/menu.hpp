#pragma once

// In-game settings menu, drawn with screen natives so it shows on the game image (and so in the headset).
// Opened with the OpenMenu bind (default Ctrl+F6). Every change is written to its ini file at once and applies live.

enum MenuAction
{
	MENU_ACTION_NONE,
	MENU_ACTION_VR_TOGGLE,   // VR off while in VR, VR on while waiting
	MENU_ACTION_RECENTRE,
};

// Call once per script tick, after UpdateKeyReleases() and before any BindPressed() check.
// vrActive selects the wording of the VR row and enables the re-centre action.
void Menu_Tick(bool vrActive);

bool Menu_IsOpen();

// The action chosen on the Actions page this tick (consumed by the call).
MenuAction Menu_TakeAction();
