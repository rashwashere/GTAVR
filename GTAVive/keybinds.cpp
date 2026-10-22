#include "keybinds.hpp"
#include "keyboard.hpp"
#include "settings.hpp"
#include "log.hpp"

using OVRInject::HotkeyBind;

static const BindInfo kBinds[BIND_COUNT] = {
	{ "EnterVR",              "Ctrl+L",          "Enter VR",                  "Starts VR. While in VR it re-creates the hand pistol." },
	{ "VROff",                "Ctrl+Shift+L",    "VR off / on",               "Hands the view back to the game camera; press again to return to VR." },
	{ "RecentreView",         "Ctrl+Shift+C",    "Re-centre view",            "Puts the VR view back inside the character." },
	{ "FlyMove",              "1",               "Touchpad fly-move",         "Toggles moving with the right touchpad." },
	{ "LeftHandSteering",     "2",               "Left-hand steering",        "Toggles steering vehicles by tilting the left controller." },
	{ "MeleeCalibrationDump", "U",               "Melee calibration dump",    "Writes the melee prop rotation values to gravovrvalues.txt." },
	{ "OpenMenu",             "Ctrl+F5",         "Open / close this menu",    "Opens and closes this menu. On the same key as VRCam, it opens VRCam's menu." },
	{ "MenuUp",               "Up",              "Menu: up",                  "Moves the menu selection up (hold to repeat)." },
	{ "MenuDown",             "Down",            "Menu: down",                "Moves the menu selection down (hold to repeat)." },
	{ "MenuLeft",             "Left",            "Menu: decrease",            "Lowers the selected value (hold to repeat)." },
	{ "MenuRight",            "Right",           "Menu: increase",            "Raises the selected value (hold to repeat)." },
	{ "MenuSelect",           "Enter",           "Menu: select",              "Opens a page, toggles a value or starts a key change." },
	{ "MenuBack",             "Backspace",       "Menu: back",                "Goes back one page (Esc always works too)." },
	{ "ToggleKey",            "Ctrl+Shift+F8",   "Stabilizer on / off",       "Turns the camera stabilizer on and off (handled by OVRInject)." },
	{ "ReloadKey",            "Ctrl+Shift+F9",   "Stabilizer: reload file",   "Re-reads stabilizer.ini, discarding live changes." },
	{ "PresetKey",            "Ctrl+Shift+P",    "Stabilizer: next preset",   "Cycles Jitter, Manual and Adaptive." },
	{ "TargetKey",            "Ctrl+Shift+A",    "Stabilizer: adjust target", "Picks which value the stabilizer up / down keys change." },
	{ "UpKey",                "Ctrl+Shift+Up",   "Stabilizer: more",          "More smoothing (hold to repeat)." },
	{ "DownKey",              "Ctrl+Shift+Down", "Stabilizer: less",          "Less smoothing (hold to repeat)." },
	{ "SaveKey",              "Ctrl+Shift+S",    "Stabilizer: save",          "Writes the live stabilizer values to stabilizer.ini." },
	{ "ReanchorKey",          "Ctrl+Shift+Home", "Room scale re-anchor",      "Makes the current spot the room scale centre." },
};

static HotkeyBind g_binds[BIND_COUNT];
static bool g_loaded = false;
static bool g_suppressed = false;

const BindInfo& GetBindInfo(BindId id)
{
	return kBinds[id];
}

bool IsStabilizerBind(BindId id)
{
	return id >= BIND_GTAVIVE_COUNT;
}

static bool IsMenuNavBind(BindId id)
{
	return id >= BIND_MENU_UP && id <= BIND_MENU_BACK;
}

bool BindCanBeNone(BindId id)
{
	return id != BIND_OPEN_MENU && !IsMenuNavBind(id);
}

static const char* BindFile(BindId id)
{
	return IsStabilizerBind(id) ? StabilizerIniPath() : GtaviveIniPath();
}

static const char* BindSection(BindId id)
{
	return IsStabilizerBind(id) ? "Hotkeys" : "Keys";
}

static HotkeyBind DefaultBind(BindId id)
{
	HotkeyBind b = { 0, false, false, false };
	OVRInject::HotkeyParse(kBinds[id].defaultText, b);
	return b;
}

void LoadKeyBinds(bool force)
{
	static ULONGLONG lastLoad = 0;
	static unsigned lastGeneration = 0;
	ULONGLONG now = GetTickCount64();
	unsigned generation = SettingsGeneration();
	if (!force && g_loaded && generation == lastGeneration && now - lastLoad < 1000)
		return;
	lastLoad = now;
	lastGeneration = generation;

	for (int i = 0; i < BIND_COUNT; ++i)
	{
		BindId id = (BindId)i;
		char text[64] = "";
		GetPrivateProfileStringA(BindSection(id), kBinds[i].iniKey, kBinds[i].defaultText, text, sizeof(text), BindFile(id));
		// "None" and an empty value parse to the unbound bind and stay None
		HotkeyBind b = DefaultBind(id);
		bool readable = OVRInject::HotkeyParse(text, b);
		if (!readable)
		{
			b = DefaultBind(id);
			static ULONGLONG lastWarn[BIND_COUNT];
			if (id != BIND_OPEN_MENU && now - lastWarn[i] > 30000)
			{
				lastWarn[i] = now;
				Trace("KEYS: could not read %s=%s, using %s\n", kBinds[i].iniKey, text, kBinds[i].defaultText);
			}
		}
		// The menu key is never None or unreadable: the menu must stay reachable
		if (id == BIND_OPEN_MENU && (!readable || OVRInject::HotkeyIsNone(b)))
		{
			b = DefaultBind(id);
			static bool warned = false;
			if (!warned)
			{
				warned = true;
				Trace("KEYS: %s can not be None or unreadable (%s=%s), using %s\n", kBinds[i].iniKey,
					kBinds[i].iniKey, text, kBinds[i].defaultText);
			}
		}
		if (g_loaded && !OVRInject::HotkeyEqual(b, g_binds[i]))
		{
			char shown[64];
			OVRInject::HotkeyFormat(b, shown, sizeof(shown));
			Trace("KEYS: %s is now %s\n", kBinds[i].iniKey, shown);
		}
		g_binds[i] = b;
	}
	g_loaded = true;
}

const HotkeyBind& GetBind(BindId id)
{
	if (!g_loaded)
		LoadKeyBinds(true);
	return g_binds[id];
}

void GetBindText(BindId id, char* out, size_t outSize)
{
	OVRInject::HotkeyFormat(GetBind(id), out, outSize);
}

HotkeyBind GetEffectiveBind(BindId id)
{
	const HotkeyBind& b = GetBind(id);
	if (IsMenuNavBind(id) && OVRInject::HotkeyIsNone(b))
		return DefaultBind(id);
	return b;
}

void GetEffectiveBindText(BindId id, char* out, size_t outSize)
{
	OVRInject::HotkeyFormat(GetEffectiveBind(id), out, outSize);
}

bool SetBind(BindId id, const HotkeyBind& bind)
{
	if (OVRInject::HotkeyIsNone(bind) && !BindCanBeNone(id))
		return false;
	HotkeyBind stored = bind;
	if (OVRInject::HotkeyIsNone(stored))
		stored.ctrl = stored.shift = stored.alt = false;
	char text[64];
	OVRInject::HotkeyFormat(stored, text, sizeof(text));
	WriteIniString(BindSection(id), kBinds[id].iniKey, text, BindFile(id));
	g_binds[id] = stored;
	return true;
}

void ResetAllBindsToDefaults()
{
	for (int i = 0; i < BIND_COUNT; ++i)
	{
		BindId id = (BindId)i;
		WriteIniString(BindSection(id), kBinds[i].iniKey, kBinds[i].defaultText, BindFile(id));
		g_binds[i] = DefaultBind(id);
	}
	Trace("KEYS: all binds reset to defaults\n");
}

BindId FindDuplicateBind(BindId id)
{
	// Compares the keys that act; an unbound (None) function duplicates nothing
	HotkeyBind b = GetEffectiveBind(id);
	if (OVRInject::HotkeyIsNone(b))
		return BIND_COUNT;
	for (int i = 0; i < BIND_COUNT; ++i)
	{
		if (i != id && OVRInject::HotkeyEqual(b, GetEffectiveBind((BindId)i)))
			return (BindId)i;
	}
	return BIND_COUNT;
}

void SetBindsSuppressed(bool suppressed)
{
	g_suppressed = suppressed;
}

bool BindPressedIgnoringSuppression(BindId id)
{
	HotkeyBind b = GetEffectiveBind(id);
	return b.vk > 0 && KeyReleasedThisTick((DWORD)b.vk) && OVRInject::HotkeyModifiersMatch(b);
}

bool BindPressed(BindId id)
{
	return !g_suppressed && BindPressedIgnoringSuppression(id);
}

bool BindHeld(BindId id)
{
	HotkeyBind b = GetEffectiveBind(id);
	return b.vk > 0 && (GetAsyncKeyState(b.vk) & 0x8000) != 0 && OVRInject::HotkeyModifiersMatch(b);
}
