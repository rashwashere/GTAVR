// In-game settings menu.
//
// One panel: title, page name, rows, a help line for the selected row and a key hint line. Pages:
//   Gameplay   - every gtavive.ini [Beta] key
//   Stabilizer - stabilizer.ini [Stabilization], the active preset's values, [RoomScale] and [Timing]
//   Keys       - every bind (gtavive.ini [Keys] and stabilizer.ini [Hotkeys])
//   Actions    - VR off / on, re-centre view
// Values are cached while the menu is open and re-read from the files once a second, so edits made in a text
// editor show up too. Each change is written at once; GTAVive picks up gtavive.ini on its next tick and OVRInject
// auto-reloads stabilizer.ini.

#include "menu.hpp"
#include "keybinds.hpp"
#include "keyboard.hpp"
#include "settings.hpp"
#include "log.hpp"
#include "sdk/natives.h"

#include <vector>
#include <cstdio>
#include <cstring>
#include <cmath>

using OVRInject::HotkeyBind;

namespace
{
	enum Page { PAGE_MAIN, PAGE_GAMEPLAY, PAGE_STABILIZER, PAGE_KEYS, PAGE_ACTIONS, PAGE_COUNT };
	enum RowKind { ROW_HEADER, ROW_PAGE, ROW_TOGGLE, ROW_NUMBER, ROW_CHOICE, ROW_KEY, ROW_ACTION };
	enum IniFile { INI_GTAVIVE, INI_STABILIZER };
	enum ActionId { ACT_VR_TOGGLE, ACT_RECENTRE, ACT_RESET_KEYS, ACT_CLOSE };

	typedef float (*StepFn)(float v, int dir);

	struct Row
	{
		RowKind kind = ROW_HEADER;
		const char* label = "";
		const char* help = "";
		IniFile file = INI_GTAVIVE;
		const char* section = nullptr;
		const char* key = nullptr;
		float def = 0.0f, lo = 0.0f, hi = 1.0f, step = 1.0f;
		int decimals = 0;
		const char* unit = "";
		StepFn stepFn = nullptr;
		const char* const* choices = nullptr;
		int choiceCount = 0;
		bool choiceByName = false;
		int presetOnly = -1;   // stabilizer preset rows: shown only while that preset is active
		int target = 0;        // ROW_PAGE: Page, ROW_KEY: BindId, ROW_ACTION: ActionId
		float value = 0.0f;    // cached ini value
	};

	const char* const kPresetNames[] = { "Jitter", "Manual", "Adaptive" };
	const char* const kRoomModeNames[] = { "Linear", "Walk", "Progressive" };
	const char* const kTimingNames[] = { "Newest pose", "Interpolate", "Extrapolate" };

	// Finer steps near zero for values that span several orders of magnitude
	float StepFine(float v, int dir)
	{
		float a = dir > 0 ? v : v - 0.0001f;
		if (a < 0.1f) return 0.01f;
		if (a < 1.0f) return 0.05f;
		if (a < 10.0f) return 0.5f;
		return 1.0f;
	}

	float StepGain(float v, int dir)
	{
		float a = dir > 0 ? v : v - 0.0001f;
		return a < 2.0f ? 0.1f : 0.5f;
	}

	Row Header(const char* label)
	{
		Row r;
		r.kind = ROW_HEADER;
		r.label = label;
		return r;
	}

	Row PageRow(const char* label, const char* help, Page page)
	{
		Row r;
		r.kind = ROW_PAGE;
		r.label = label;
		r.help = help;
		r.target = page;
		return r;
	}

	Row ActionRow(const char* label, const char* help, ActionId action)
	{
		Row r;
		r.kind = ROW_ACTION;
		r.label = label;
		r.help = help;
		r.target = action;
		return r;
	}

	Row KeyRow(BindId id)
	{
		Row r;
		r.kind = ROW_KEY;
		r.label = GetBindInfo(id).label;
		r.help = GetBindInfo(id).help;
		r.target = id;
		return r;
	}

	Row Toggle(const char* label, const char* help, IniFile file, const char* section, const char* key, bool def, int presetOnly = -1)
	{
		Row r;
		r.kind = ROW_TOGGLE;
		r.label = label;
		r.help = help;
		r.file = file;
		r.section = section;
		r.key = key;
		r.def = def ? 1.0f : 0.0f;
		r.presetOnly = presetOnly;
		return r;
	}

	Row Number(const char* label, const char* help, IniFile file, const char* section, const char* key,
		float def, float lo, float hi, float step, int decimals, const char* unit, int presetOnly = -1, StepFn stepFn = nullptr)
	{
		Row r;
		r.kind = ROW_NUMBER;
		r.label = label;
		r.help = help;
		r.file = file;
		r.section = section;
		r.key = key;
		r.def = def;
		r.lo = lo;
		r.hi = hi;
		r.step = step;
		r.decimals = decimals;
		r.unit = unit;
		r.presetOnly = presetOnly;
		r.stepFn = stepFn;
		return r;
	}

	Row Choice(const char* label, const char* help, IniFile file, const char* section, const char* key,
		const char* const* choices, int count, int def, bool byName)
	{
		Row r;
		r.kind = ROW_CHOICE;
		r.label = label;
		r.help = help;
		r.file = file;
		r.section = section;
		r.key = key;
		r.choices = choices;
		r.choiceCount = count;
		r.def = (float)def;
		r.lo = 0.0f;
		r.hi = (float)(count - 1);
		r.choiceByName = byName;
		return r;
	}

	std::vector<Row> g_pages[PAGE_COUNT];
	const char* const kPageTitles[PAGE_COUNT] = { "Settings", "Gameplay", "Stabilizer", "Keys", "Actions" };

	void BuildPages()
	{
		const IniFile G = INI_GTAVIVE, S = INI_STABILIZER;

		std::vector<Row>& m = g_pages[PAGE_MAIN];
		m.push_back(PageRow("Gameplay", "Movement, camera and VR start options (gtavive.ini [Beta]).", PAGE_GAMEPLAY));
		m.push_back(PageRow("Stabilizer", "Camera stabilizer, room scale and pose timing (stabilizer.ini).", PAGE_STABILIZER));
		m.push_back(PageRow("Keys", "Change any keyboard bind.", PAGE_KEYS));
		m.push_back(PageRow("Actions", "VR off / on and re-centre the view.", PAGE_ACTIONS));
		m.push_back(ActionRow("Close menu", "Closes this menu. Every change is already saved.", ACT_CLOSE));

		std::vector<Row>& g = g_pages[PAGE_GAMEPLAY];
		g.push_back(Toggle("Move where you look", "Pushing the stick forward walks where the headset looks.", G, "Beta", "MoveWhereYouLook", false));
		g.push_back(Toggle("Anchor camera to player", "Attaches the on-foot camera to the character (no walking jitter).", G, "Beta", "AnchorCameraToPlayer", false));
		g.push_back(Number("Follow smoothing", "With the anchored camera: 0 = rigid, higher = softer follow.", G, "Beta", "FollowSmoothMs", 0, 0, 2000, 25, 0, " ms"));
		g.push_back(Toggle("Stick turn", "The right stick's left / right turns the play space.", G, "Beta", "StickTurn", false));
		g.push_back(Number("Stick turn speed", "Turn speed at full stick deflection.", G, "Beta", "StickTurnSpeed", 90, 10, 360, 10, 0, " deg/s"));
		g.push_back(Number("Vehicle tilt follow", "How much the view tilts with a vehicle: 0 = horizon locked, 100 = fully.", G, "Beta", "VehicleAttitudeFollow", 0, 0, 100, 5, 0, ""));
		g.push_back(Number("Vehicle tilt deadzone", "Vehicle tilt ignored before the view starts to follow.", G, "Beta", "VehicleAttitudeDeadzone", 0, 0, 45, 1, 0, " deg"));
		g.push_back(Number("Tilt smoothing", "Smoothing of the followed tilt (also used by fall follow).", G, "Beta", "VehicleAttitudeSmoothMs", 150, 0, 2000, 25, 0, " ms"));
		g.push_back(Number("Fall follow", "How far the view goes down with a falling or ragdolled character, 0-100.", G, "Beta", "PedFallFollow", 0, 0, 100, 5, 0, ""));
		g.push_back(Number("Height offset", "Moves your viewpoint down (-) or up (+). Tall players: try -15 to -30.", G, "Beta", "HeightOffsetCm", 0, -100, 50, 1, 1, " cm"));
		g.push_back(Toggle("Centre on VR start", "Where you stand when VR starts counts as inside the character.", G, "Beta", "CentreOnVrStart", true));
		g.push_back(Toggle("Auto-start VR", "Enters VR without the Enter VR key once the headset is ready.", G, "Beta", "AutoStartVR", false));

		std::vector<Row>& s = g_pages[PAGE_STABILIZER];
		s.push_back(Toggle("Stabilizer", "Smooths the headset pose before it reaches the game camera.", S, "Stabilization", "Enabled", true));
		s.push_back(Choice("Preset", "Jitter = fixed smoothing, Manual = per axis, Adaptive = smooth when still.", S, "Stabilization", "Preset", kPresetNames, 3, 0, true));
		s.push_back(Toggle("Stabilize position", "Smooth the physical movement.", S, "Stabilization", "StabilizePosition", true));
		s.push_back(Toggle("Stabilize rotation", "Smooth the looking around.", S, "Stabilization", "StabilizeRotation", true));
		s.push_back(Number("Position smoothing", "Higher = smoother but laggier.", S, "Jitter", "PosSmoothMs", 180, 0, 600, 10, 0, " ms", 0));
		s.push_back(Number("Rotation smoothing", "Higher = smoother but laggier.", S, "Jitter", "RotSmoothMs", 140, 0, 600, 10, 0, " ms", 0));
		s.push_back(Number("Pitch strength", "Up / down look smoothing, 0 = raw, 100 = max smoothing.", S, "Manual", "PitchStrength", 35, 0, 100, 5, 0, "", 1));
		s.push_back(Number("Yaw strength", "Left / right look smoothing, 0 = raw, 100 = max smoothing.", S, "Manual", "YawStrength", 35, 0, 100, 5, 0, "", 1));
		s.push_back(Number("Roll strength", "Head tilt smoothing, 0 = raw, 100 = max smoothing.", S, "Manual", "RollStrength", 35, 0, 100, 5, 0, "", 1));
		s.push_back(Number("Vertical strength", "Up / down bob smoothing, 0 = raw, 100 = max smoothing.", S, "Manual", "VerticalStrength", 45, 0, 100, 5, 0, "", 1));
		s.push_back(Number("Lateral strength", "Left / right sway smoothing, 0 = raw, 100 = max smoothing.", S, "Manual", "LateralStrength", 45, 0, 100, 5, 0, "", 1));
		s.push_back(Number("Depth strength", "Forward / back smoothing, 0 = raw, 100 = max smoothing.", S, "Manual", "DepthStrength", 45, 0, 100, 5, 0, "", 1));
		s.push_back(Number("Max smoothing", "How much smoothing a strength of 100 means.", S, "Manual", "MaxSmoothMs", 400, 50, 600, 25, 0, " ms", 1));
		s.push_back(Number("Position cutoff", "Lower = smoother when still.", S, "Adaptive", "PosMinCutoff", 0.8f, 0.1f, 5.0f, 0.1f, 2, " Hz", 2));
		s.push_back(Number("Position speed response", "Higher = less lag on fast moves.", S, "Adaptive", "PosBeta", 0.02f, 0.0f, 50.0f, 0.0f, 2, "", 2, StepFine));
		s.push_back(Number("Rotation cutoff", "Lower = smoother when still.", S, "Adaptive", "RotMinCutoff", 0.8f, 0.1f, 5.0f, 0.1f, 2, " Hz", 2));
		s.push_back(Number("Rotation speed response", "Higher = less lag on fast turns.", S, "Adaptive", "RotBeta", 0.02f, 0.0f, 50.0f, 0.0f, 2, "", 2, StepFine));
		s.push_back(Header("Room scale"));
		s.push_back(Toggle("Room scale", "Amplifies real walking in a small room.", S, "RoomScale", "Enabled", true));
		s.push_back(Choice("Room scale mode", "Linear = all travel, Walk = only walking pace, Progressive = beyond a radius.", S, "RoomScale", "Mode", kRoomModeNames, 3, 1, false));
		s.push_back(Number("Walk gain", "How much real walking is amplified on the floor plane.", S, "RoomScale", "PlaneGain", 4.0f, 0.1f, 20.0f, 0.0f, 2, "x", -1, StepGain));
		s.push_back(Header("Timing"));
		s.push_back(Choice("Pose timing", "Newest pose, interpolate to the frame time (smoothest) or extrapolate.", S, "Timing", "PoseTiming", kTimingNames, 3, 0, false));

		std::vector<Row>& k = g_pages[PAGE_KEYS];
		k.push_back(Header("GTAVive (gtavive.ini [Keys])"));
		for (int i = 0; i < BIND_GTAVIVE_COUNT; ++i)
			k.push_back(KeyRow((BindId)i));
		k.push_back(Header("Stabilizer and room scale (stabilizer.ini [Hotkeys])"));
		for (int i = BIND_GTAVIVE_COUNT; i < BIND_COUNT; ++i)
			k.push_back(KeyRow((BindId)i));
		k.push_back(ActionRow("Reset keys to defaults", "Restores every bind on this page. Press twice to confirm.", ACT_RESET_KEYS));

		std::vector<Row>& a = g_pages[PAGE_ACTIONS];
		a.push_back(ActionRow("VR off / on", "Switches VR off, or on again.", ACT_VR_TOGGLE));
		a.push_back(ActionRow("Re-centre view", "Puts the VR view back inside the character.", ACT_RECENTRE));
	}

	const char* RowFile(const Row& r)
	{
		return r.file == INI_STABILIZER ? StabilizerIniPath() : GtaviveIniPath();
	}

	float RoundTo(float v, int decimals)
	{
		double scale = pow(10.0, decimals);
		return (float)(floor(v * scale + 0.5) / scale);
	}

	void ReadRow(Row& r)
	{
		const char* path = RowFile(r);
		switch (r.kind)
		{
		case ROW_TOGGLE:
			r.value = ReadIniDecimal(r.section, r.key, r.def, -1.0e9f, 1.0e9f, path) != 0.0f ? 1.0f : 0.0f;
			break;
		case ROW_NUMBER:
			r.value = ReadIniDecimal(r.section, r.key, r.def, r.lo, r.hi, path);
			break;
		case ROW_CHOICE:
			if (r.choiceByName)
			{
				char buf[32] = "";
				GetPrivateProfileStringA(r.section, r.key, "", buf, sizeof(buf), path);
				int found = -1;
				for (int i = 0; i < r.choiceCount; ++i)
					if (_stricmp(buf, r.choices[i]) == 0)
						found = i;
				if (found < 0 && strcmp(r.key, "Preset") == 0)
					// No preset name: OVRInject falls back to the older numeric Mode key (1 = Adaptive, else Jitter)
					found = GetPrivateProfileIntA(r.section, "Mode", 2, path) == 1 ? 2 : 0;
				r.value = (float)(found < 0 ? (int)r.def : found);
			}
			else
			{
				float v = ReadIniDecimal(r.section, r.key, r.def, -1.0e9f, 1.0e9f, path);
				int i = (int)v;
				r.value = (float)(i < 0 || i >= r.choiceCount ? (int)r.def : i);
			}
			break;
		default:
			break;
		}
	}

	void WriteRow(const Row& r)
	{
		const char* path = RowFile(r);
		switch (r.kind)
		{
		case ROW_TOGGLE:
			WriteIniString(r.section, r.key, r.value != 0.0f ? "1" : "0", path);
			break;
		case ROW_NUMBER:
			WriteIniDecimal(r.section, r.key, r.value, r.decimals, path);
			break;
		case ROW_CHOICE:
			if (r.choiceByName)
				WriteIniString(r.section, r.key, r.choices[(int)r.value], path);
			else
				WriteIniDecimal(r.section, r.key, r.value, 0, path);
			break;
		default:
			return;
		}
		Trace("MENU: [%s] %s = %.3f\n", r.section, r.key, r.value);
	}

	void ReadPage(Page page)
	{
		for (Row& r : g_pages[page])
			ReadRow(r);
	}

	// Active stabilizer preset, from the cached Preset row
	int StabilizerPreset()
	{
		for (const Row& r : g_pages[PAGE_STABILIZER])
			if (r.kind == ROW_CHOICE && r.choiceByName)
				return (int)r.value;
		return 0;
	}

	bool RowVisible(Page page, const Row& r)
	{
		return page != PAGE_STABILIZER || r.presetOnly < 0 || r.presetOnly == StabilizerPreset();
	}

	bool RowSelectable(const Row& r)
	{
		return r.kind != ROW_HEADER;
	}

	// --- menu state ---

	struct RepeatKey
	{
		bool wasDown = false;
		ULONGLONG next = 0;
	};

	const int MAX_VISIBLE_ROWS = 14;
	const ULONGLONG REPEAT_DELAY_MS = 400;
	const ULONGLONG REPEAT_INTERVAL_MS = 70;

	bool g_built = false;
	bool g_open = false;
	Page g_page = PAGE_MAIN;
	int g_sel[PAGE_COUNT] = {};     // index into the page's visible rows
	int g_scroll[PAGE_COUNT] = {};
	ULONGLONG g_lastRefresh = 0;
	bool g_capturing = false;
	bool g_captureArmed = false;
	BindId g_captureBind = BIND_COUNT;
	bool g_resetArmed = false;
	const char* g_notice = nullptr; // short message in the help line, shown until g_noticeUntil
	ULONGLONG g_noticeUntil = 0;
	MenuAction g_pendingAction = MENU_ACTION_NONE;
	ULONGLONG g_graceUntil = 0;     // keep blocking the menu's game controls briefly after closing
	RepeatKey g_keyUp, g_keyDown, g_keyLeft, g_keyRight, g_keySelect, g_keyBack, g_keyEsc;

	void ResetRepeatStates()
	{
		// Keys already held when the menu opens or a page changes do not act until pressed again
		RepeatKey* keys[] = { &g_keyUp, &g_keyDown, &g_keyLeft, &g_keyRight, &g_keySelect, &g_keyBack, &g_keyEsc };
		for (RepeatKey* k : keys)
		{
			k->wasDown = true;
			k->next = 0;
		}
	}

	// Edge (and optional hold-repeat) detection on a physically held state
	bool Fired(bool down, RepeatKey& st, bool repeat, ULONGLONG now)
	{
		bool result = false;
		if (down && !st.wasDown)
		{
			result = true;
			st.next = now + REPEAT_DELAY_MS;
		}
		else if (down && repeat && st.next != 0 && now >= st.next)
		{
			result = true;
			st.next = now + REPEAT_INTERVAL_MS;
		}
		if (!down)
			st.next = 0;
		st.wasDown = down;
		return result;
	}

	void VisibleRows(std::vector<int>& out)
	{
		out.clear();
		std::vector<Row>& rows = g_pages[g_page];
		for (int i = 0; i < (int)rows.size(); ++i)
			if (RowVisible(g_page, rows[i]))
				out.push_back(i);
	}

	// Keeps the selection on a selectable row and inside the scroll window
	void ClampSelection(const std::vector<int>& vis)
	{
		int n = (int)vis.size();
		int& sel = g_sel[g_page];
		int& scroll = g_scroll[g_page];
		if (n == 0)
		{
			sel = scroll = 0;
			return;
		}
		if (sel >= n) sel = n - 1;
		if (sel < 0) sel = 0;
		std::vector<Row>& rows = g_pages[g_page];
		if (!RowSelectable(rows[vis[sel]]))
		{
			int i = sel;
			while (i < n && !RowSelectable(rows[vis[i]])) ++i;
			if (i >= n)
			{
				i = sel;
				while (i >= 0 && !RowSelectable(rows[vis[i]])) --i;
			}
			sel = i < 0 ? 0 : i;
		}
		if (sel < scroll) scroll = sel;
		if (sel >= scroll + MAX_VISIBLE_ROWS) scroll = sel - MAX_VISIBLE_ROWS + 1;
		// show the header right above the first row of a section when scrolling up to it
		if (scroll > 0 && scroll == sel && !RowSelectable(rows[vis[scroll - 1]])) --scroll;
		if (scroll > n - MAX_VISIBLE_ROWS) scroll = n - MAX_VISIBLE_ROWS;
		if (scroll < 0) scroll = 0;
	}

	void MoveSelection(const std::vector<int>& vis, int dir)
	{
		int n = (int)vis.size();
		if (n == 0)
			return;
		std::vector<Row>& rows = g_pages[g_page];
		int i = g_sel[g_page];
		for (int tries = 0; tries < n; ++tries)
		{
			i = (i + dir + n) % n;
			if (RowSelectable(rows[vis[i]]))
				break;
		}
		g_sel[g_page] = i;
		g_resetArmed = false;
	}

	void OpenPage(Page page)
	{
		g_page = page;
		g_resetArmed = false;
		ReadPage(page);
		if (page == PAGE_KEYS)
			LoadKeyBinds(true);
		g_lastRefresh = GetTickCount64();
	}

	void CloseMenu()
	{
		g_open = false;
		g_capturing = false;
		g_resetArmed = false;
		g_graceUntil = GetTickCount64() + 300;
		IgnoreReleaseOfHeldKeys();
		Trace("MENU: closed\n");
	}

	void GoBack()
	{
		if (g_page == PAGE_MAIN)
			CloseMenu();
		else
		{
			Page from = g_page;
			OpenPage(PAGE_MAIN);
			for (int i = 0; i < (int)g_pages[PAGE_MAIN].size(); ++i)
				if (g_pages[PAGE_MAIN][i].kind == ROW_PAGE && g_pages[PAGE_MAIN][i].target == from)
					g_sel[PAGE_MAIN] = i;
		}
	}

	void ChangeValue(Row& r, int dir)
	{
		switch (r.kind)
		{
		case ROW_TOGGLE:
			r.value = r.value != 0.0f ? 0.0f : 1.0f;
			break;
		case ROW_NUMBER:
		{
			float step = r.stepFn ? r.stepFn(r.value, dir) : r.step;
			float v = RoundTo(r.value + dir * step, r.decimals);
			if (v < r.lo) v = r.lo;
			if (v > r.hi) v = r.hi;
			if (v == r.value)
				return;
			r.value = v;
			break;
		}
		case ROW_CHOICE:
		{
			int i = ((int)r.value + dir + r.choiceCount) % r.choiceCount;
			r.value = (float)i;
			break;
		}
		default:
			return;
		}
		WriteRow(r);
	}

	void Activate(Row& r, bool vrActive)
	{
		switch (r.kind)
		{
		case ROW_PAGE:
			g_sel[r.target] = 0;
			g_scroll[r.target] = 0;
			OpenPage((Page)r.target);
			break;
		case ROW_TOGGLE:
		case ROW_CHOICE:
			ChangeValue(r, 1);
			break;
		case ROW_KEY:
			g_capturing = true;
			g_captureArmed = false;
			g_captureBind = (BindId)r.target;
			break;
		case ROW_ACTION:
			switch (r.target)
			{
			case ACT_CLOSE:
				CloseMenu();
				break;
			case ACT_VR_TOGGLE:
				g_pendingAction = MENU_ACTION_VR_TOGGLE;
				break;
			case ACT_RECENTRE:
				if (vrActive)
					g_pendingAction = MENU_ACTION_RECENTRE;
				break;
			case ACT_RESET_KEYS:
				if (!g_resetArmed)
					g_resetArmed = true;
				else
				{
					ResetAllBindsToDefaults();
					g_resetArmed = false;
				}
				break;
			}
			break;
		default:
			break;
		}
	}

	bool IsModifierOrMouse(int vk)
	{
		switch (vk)
		{
		case VK_LBUTTON: case VK_RBUTTON: case VK_CANCEL: case VK_MBUTTON: case VK_XBUTTON1: case VK_XBUTTON2:
		case VK_SHIFT: case VK_CONTROL: case VK_MENU: case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL:
		case VK_RCONTROL: case VK_LMENU: case VK_RMENU: case VK_LWIN: case VK_RWIN: case VK_CAPITAL:
			return true;
		default:
			return false;
		}
	}

	// Key capture for the Keys page: waits until every key is up, then takes the first key pressed together with
	// the modifiers held at that moment. Esc cancels; Delete alone sets the bind to None where that is allowed.
	void CaptureTick()
	{
		bool anyDown = false;
		int pressed = 0;
		for (int vk = 0x08; vk < 0xFF; ++vk)
		{
			if (IsModifierOrMouse(vk))
				continue;
			if (GetAsyncKeyState(vk) & 0x8000)
			{
				anyDown = true;
				if (pressed == 0)
					pressed = vk;
			}
		}
		if (!g_captureArmed)
		{
			if (!anyDown)
				g_captureArmed = true;
			return;
		}
		if (pressed == 0)
			return;

		g_capturing = false;
		ResetRepeatStates();        // the captured key must not also move the selection
		IgnoreReleaseOfHeldKeys();  // nor fire a bind (for example OpenMenu) when it is released
		if (pressed == VK_ESCAPE)
			return;

		HotkeyBind b;
		b.vk = pressed;
		b.ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
		b.shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
		b.alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
		if (pressed == VK_DELETE && !b.ctrl && !b.shift && !b.alt)
			b.vk = 0;   // None
		if (!SetBind(g_captureBind, b))
		{
			g_notice = "The menu key and the menu navigation keys can not be None.";
			g_noticeUntil = GetTickCount64() + 3000;
			return;
		}
		char text[64];
		OVRInject::HotkeyFormat(b, text, sizeof(text));
		Trace("MENU: %s = %s\n", GetBindInfo(g_captureBind).iniKey, text);
	}

	// The game's own use of the menu keys (phone, frontend, weapon wheel) and of attack / aim
	void BlockMenuControls()
	{
		static const int controls[] = {
			27,                       // INPUT_PHONE (Up arrow opens the phone)
			172, 173, 174, 175, 176, 177, 178, 179, 180, 181,   // INPUT_CELLPHONE_*
			187, 188, 189, 190, 191, 194, 199, 200, 201, 202,   // INPUT_FRONTEND_* (incl. pause / Esc)
			18,                       // INPUT_SKIP_CUTSCENE (Enter)
			215,                      // INPUT_FRONTEND_ENDSCREEN_ACCEPT
			24, 25, 257, 69, 70, 92, 114, 331,                  // attack / aim, on foot and in vehicles
			140, 141, 142, 143, 263, 264,                       // melee
			37,                       // INPUT_SELECT_WEAPON (weapon wheel)
			81, 82, 83, 84, 85,       // vehicle radio / weapon select keys
			99, 100,                  // INPUT_VEH_SELECT_NEXT_WEAPON / PREV
			241, 242,                 // INPUT_CURSOR_SCROLL_UP / DOWN
		};
		for (int c : controls)
		{
			CONTROLS::DISABLE_CONTROL_ACTION(0, c, 1);
			CONTROLS::DISABLE_CONTROL_ACTION(2, c, 1);
		}
	}

	// --- drawing ---

	const float PANEL_X = 0.5f;        // panel centre
	const float PANEL_W = 0.36f;
	const float PANEL_TOP = 0.13f;
	const float TITLE_H = 0.048f;
	const float SUB_H = 0.032f;
	const float ROW_H = 0.032f;
	const float HELP_H = 0.064f;
	const float FOOT_H = 0.030f;
	const float PAD = 0.008f;

	void Rect(float left, float top, float w, float h, int r, int g, int b, int a)
	{
		GRAPHICS::DRAW_RECT(left + w * 0.5f, top + h * 0.5f, w, h, r, g, b, a);
	}

	// align: 0 left, 1 centre, 2 right (right edge = wrapRight)
	void Text(const char* s, float x, float y, float scale, int r, int g, int b, int a, int align, float wrapLeft, float wrapRight)
	{
		char buf[100];
		strncpy_s(buf, s, _TRUNCATE);   // one text component holds up to 99 characters
		for (char* p = buf; *p; ++p)
			if (*p == '~')
				*p = '-';               // '~' starts a game format code
		UI::SET_TEXT_FONT(0);
		UI::SET_TEXT_SCALE(0.0f, scale);
		UI::SET_TEXT_COLOUR(r, g, b, a);
		if (align == 1)
			UI::SET_TEXT_CENTRE(1);
		else if (align == 2)
			UI::SET_TEXT_RIGHT_JUSTIFY(1);
		UI::SET_TEXT_WRAP(wrapLeft, wrapRight);
		UI::_SET_TEXT_ENTRY("STRING");
		UI::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME(buf);
		UI::_DRAW_TEXT(x, y);
	}

	void RowValueText(const Row& r, bool selected, char* out, size_t outSize)
	{
		char v[64] = "";
		switch (r.kind)
		{
		case ROW_TOGGLE:
			strcpy_s(v, r.value != 0.0f ? "On" : "Off");
			break;
		case ROW_NUMBER:
		{
			char num[32];
			FormatDecimal(num, sizeof(num), r.value, r.decimals);
			_snprintf_s(v, sizeof(v), _TRUNCATE, "%s%s", num, r.unit);
			break;
		}
		case ROW_CHOICE:
			strcpy_s(v, r.choices[(int)r.value]);
			break;
		case ROW_KEY:
			if (g_capturing && g_captureBind == (BindId)r.target)
				strcpy_s(v, "Press keys...");
			else if (OVRInject::HotkeyIsNone(GetBind((BindId)r.target)) && !BindCanBeNone((BindId)r.target))
			{
				// A navigation bind set to None in the file navigates with its default key
				char def[32];
				GetEffectiveBindText((BindId)r.target, def, sizeof(def));
				_snprintf_s(v, sizeof(v), _TRUNCATE, "None (uses %s)", def);
			}
			else
				GetBindText((BindId)r.target, v, sizeof(v));
			break;
		case ROW_PAGE:
			strcpy_s(v, ">");
			break;
		default:
			break;
		}
		bool adjustable = r.kind == ROW_TOGGLE || r.kind == ROW_NUMBER || r.kind == ROW_CHOICE;
		if (selected && adjustable)
			_snprintf_s(out, outSize, _TRUNCATE, "<  %s  >", v);
		else
			_snprintf_s(out, outSize, _TRUNCATE, "%s", v);
	}

	void Draw(bool vrActive, const std::vector<int>& vis)
	{
		const float left = PANEL_X - PANEL_W * 0.5f;
		const float right = PANEL_X + PANEL_W * 0.5f;
		std::vector<Row>& rows = g_pages[g_page];
		int n = (int)vis.size();
		int shown = n < MAX_VISIBLE_ROWS ? n : MAX_VISIBLE_ROWS;
		float y = PANEL_TOP;

		// title
		Rect(left, y, PANEL_W, TITLE_H, 24, 92, 160, 235);
		Text("GTA: Vive", PANEL_X, y + 0.006f, 0.55f, 255, 255, 255, 255, 1, left, right);
		y += TITLE_H;

		// page name and position
		Rect(left, y, PANEL_W, SUB_H, 12, 12, 12, 235);
		Text(kPageTitles[g_page], left + PAD, y + 0.005f, 0.32f, 120, 190, 255, 255, 0, left, right);
		char pos[32];
		int selectable = 0, selIndex = 0;
		for (int i = 0; i < n; ++i)
			if (RowSelectable(rows[vis[i]]))
			{
				++selectable;
				if (i == g_sel[g_page]) selIndex = selectable;
			}
		_snprintf_s(pos, sizeof(pos), _TRUNCATE, "%d / %d", selIndex, selectable);
		Text(pos, left, y + 0.005f, 0.32f, 200, 200, 200, 255, 2, left, right - PAD);
		y += SUB_H;

		// rows
		const Row* selectedRow = nullptr;
		for (int i = g_scroll[g_page]; i < g_scroll[g_page] + shown && i < n; ++i)
		{
			const Row& r = rows[vis[i]];
			bool selected = i == g_sel[g_page];
			if (selected)
				selectedRow = &r;

			if (r.kind == ROW_HEADER)
			{
				Rect(left, y, PANEL_W, ROW_H, 0, 0, 0, 215);
				Text(r.label, left + PAD, y + 0.006f, 0.30f, 120, 190, 255, 255, 0, left, right);
				y += ROW_H;
				continue;
			}

			bool dup = r.kind == ROW_KEY && FindDuplicateBind((BindId)r.target) != BIND_COUNT;
			bool disabled = r.kind == ROW_ACTION && r.target == ACT_RECENTRE && !vrActive;
			if (selected)
				Rect(left, y, PANEL_W, ROW_H, 235, 235, 235, 240);
			else
				Rect(left, y, PANEL_W, ROW_H, 0, 0, 0, 185);

			int tr = selected ? 10 : 240, tg = selected ? 10 : 240, tb = selected ? 10 : 240;
			if (disabled) { tr = tg = tb = selected ? 110 : 140; }

			const char* label = r.label;
			if (r.kind == ROW_ACTION && r.target == ACT_VR_TOGGLE)
				label = vrActive ? "VR off" : "VR on";
			if (r.kind == ROW_ACTION && r.target == ACT_RESET_KEYS && g_resetArmed)
				label = "Reset keys to defaults - press again";
			Text(label, left + PAD, y + 0.005f, 0.32f, tr, tg, tb, 255, 0, left, right);

			char value[96];
			RowValueText(r, selected, value, sizeof(value));
			if (value[0] != '\0')
			{
				int vr = tr, vg = tg, vb = tb;
				if (dup) { vr = 230; vg = 120; vb = 20; }
				else if (!selected) { vr = 170; vg = 215; vb = 255; }
				Text(value, left, y + 0.005f, 0.32f, vr, vg, vb, 255, 2, left, right - PAD);
			}
			y += ROW_H;
		}

		// help line for the selected row
		Rect(left, y, PANEL_W, HELP_H, 12, 12, 12, 235);
		char help[200] = "";
		int hr = 220, hg = 220, hb = 220;
		if (g_capturing)
			_snprintf_s(help, sizeof(help), _TRUNCATE, "Press the new key combination for %s.%s Esc cancels.",
				GetBindInfo(g_captureBind).label, BindCanBeNone(g_captureBind) ? " Delete = None." : "");
		else if (g_notice != nullptr && GetTickCount64() < g_noticeUntil)
		{
			strcpy_s(help, g_notice);
			hr = 255; hg = 150; hb = 60;
		}
		else if (selectedRow != nullptr)
		{
			const Row& r = *selectedRow;
			BindId other = r.kind == ROW_KEY ? FindDuplicateBind((BindId)r.target) : BIND_COUNT;
			if (other != BIND_COUNT)
			{
				_snprintf_s(help, sizeof(help), _TRUNCATE, "Also bound to: %s. Enter to change.", GetBindInfo(other).label);
				hr = 255; hg = 150; hb = 60;
			}
			else if (r.kind == ROW_ACTION && r.target == ACT_RECENTRE && !vrActive)
				strcpy_s(help, "Available while VR is on.");
			else if (r.kind == ROW_KEY)
				_snprintf_s(help, sizeof(help), _TRUNCATE, "%s Enter to change.", r.help);
			else
				strcpy_s(help, r.help);
		}
		Text(help, left + PAD, y + 0.006f, 0.28f, hr, hg, hb, 255, 0, left + PAD, right - PAD);
		y += HELP_H;

		// key hints
		Rect(left, y, PANEL_W, FOOT_H, 24, 92, 160, 235);
		char up[32], down[32], lft[32], rgt[32], sel[32], back[32], hint[200];
		GetEffectiveBindText(BIND_MENU_UP, up, sizeof(up));
		GetEffectiveBindText(BIND_MENU_DOWN, down, sizeof(down));
		GetEffectiveBindText(BIND_MENU_LEFT, lft, sizeof(lft));
		GetEffectiveBindText(BIND_MENU_RIGHT, rgt, sizeof(rgt));
		GetEffectiveBindText(BIND_MENU_SELECT, sel, sizeof(sel));
		GetEffectiveBindText(BIND_MENU_BACK, back, sizeof(back));
		_snprintf_s(hint, sizeof(hint), _TRUNCATE, "%s/%s select   %s/%s change   %s ok   %s/Esc back", up, down, lft, rgt, sel, back);
		Text(hint, PANEL_X, y + 0.005f, 0.26f, 255, 255, 255, 255, 1, left, right);
	}
}

void Menu_Tick(bool vrActive)
{
	if (!g_built)
	{
		BuildPages();
		g_built = true;
	}
	LoadKeyBinds();

	ULONGLONG now = GetTickCount64();
	bool wasOpen = g_open;
	bool focused = IsGameWindowFocused();
	bool paused = UI::IS_PAUSE_MENU_ACTIVE() != 0;

	if (!g_open)
	{
		if (focused && !paused && BindPressedIgnoringSuppression(BIND_OPEN_MENU))
		{
			g_open = true;
			ResetRepeatStates();
			OpenPage(g_page);
			Trace("MENU: opened\n");
		}
	}
	else if (!g_capturing && BindPressedIgnoringSuppression(BIND_OPEN_MENU))
		CloseMenu();

	if (g_open)
	{
		if (now - g_lastRefresh >= 1000)
		{
			ReadPage(g_page);   // picks up edits made outside the menu
			g_lastRefresh = now;
		}

		BlockMenuControls();
		if (g_capturing)
		{
			CONTROLS::DISABLE_ALL_CONTROL_ACTIONS(0);   // the key being captured must not act in the game
			CONTROLS::DISABLE_ALL_CONTROL_ACTIONS(2);
		}

		std::vector<int> vis;
		VisibleRows(vis);
		ClampSelection(vis);

		if (focused && !paused)
		{
			if (g_capturing)
				CaptureTick();
			else
			{
				bool esc = Fired((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0, g_keyEsc, false, now);
				bool back = Fired(BindHeld(BIND_MENU_BACK), g_keyBack, false, now);
				bool up = Fired(BindHeld(BIND_MENU_UP), g_keyUp, true, now);
				bool down = Fired(BindHeld(BIND_MENU_DOWN), g_keyDown, true, now);
				bool lft = Fired(BindHeld(BIND_MENU_LEFT), g_keyLeft, true, now);
				bool rgt = Fired(BindHeld(BIND_MENU_RIGHT), g_keyRight, true, now);
				bool select = Fired(BindHeld(BIND_MENU_SELECT), g_keySelect, false, now);

				if (esc || back)
				{
					GoBack();
					if (g_open)
					{
						ResetRepeatStates();
						VisibleRows(vis);
						ClampSelection(vis);
					}
				}
				else if (!vis.empty())
				{
					if (up) MoveSelection(vis, -1);
					if (down) MoveSelection(vis, 1);
					ClampSelection(vis);
					Row& row = g_pages[g_page][vis[g_sel[g_page]]];
					if (lft || rgt)
					{
						ChangeValue(row, rgt ? 1 : -1);
						g_resetArmed = false;
					}
					else if (select)
					{
						Page before = g_page;
						Activate(row, vrActive);
						if (g_page != before)
							ResetRepeatStates();
					}
					// a preset change shows other rows; keep the selection valid
					VisibleRows(vis);
					ClampSelection(vis);
				}
			}
		}

		if (g_open && !paused)
			Draw(vrActive, vis);
	}
	else if (g_graceUntil != 0)
	{
		// The key that closed the menu may still be on its way up: keep it away from the game (pause menu, phone)
		if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) || (GetAsyncKeyState(VK_BACK) & 0x8000) ||
			(GetAsyncKeyState(VK_RETURN) & 0x8000) || BindHeld(BIND_MENU_BACK) || BindHeld(BIND_MENU_SELECT))
			g_graceUntil = now + 300;
		if (now < g_graceUntil)
			BlockMenuControls();
		else
			g_graceUntil = 0;
	}

	SetBindsSuppressed(g_open || wasOpen);
}

bool Menu_IsOpen()
{
	return g_open;
}

MenuAction Menu_TakeAction()
{
	MenuAction a = g_pendingAction;
	g_pendingAction = MENU_ACTION_NONE;
	return a;
}
