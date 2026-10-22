// GTA: Vive - ScriptHookV script half (GTAVive.asi).
//
// Reconstructed source for the original Alpha 0.1 binary (source was never
// published). Rebuilt from Ghidra decompilation + disassembly of the
// original GTAVive.asi; behavior, constants and native-call argument values
// were taken verbatim from the binary. See RECONSTRUCTION.md for the
// address-level mapping and verification notes.
//
// In-game keys, defaults shown; every one can be changed in gtaovr\gtavive.ini [Keys] or in the settings menu
// (keybinds.cpp, menu.cpp). VR mode starts once EnterVR is pressed with SteamVR connected:
//   Ctrl+L       EnterVR - enter VR mode; in VR, re-create the hand weapon prop
//                (plain L in the alpha; Ctrl is required so that typing 'L'
//                elsewhere does not trigger VR mode)
//   Ctrl+Shift+L VROff - switch VR off (and back on)
//   Ctrl+Shift+C RecentreView - re-centre the VR view on the character
//   0            DesktopMirror - toggle desktop mirroring
//   1            FlyMove - toggle touchpad fly-move
//   2            LeftHandSteering - toggle left-controller vehicle steering
//                (tilt = steer, trigger = accelerate, pad = brake)
//   U            MeleeCalibrationDump - write the prop rotation calibration
//                values (kx/ky/kz) to gravovrvalues.txt
//   Ctrl+F6      OpenMenu - open / close the settings menu (VR on or off)
//
// Right controller: trigger = fire, menu = cycle weapon (pistol / micro SMG /
// assault shotgun / RPG), grip = knuckle-duster melee (capsule swing test +
// ragdoll force), touchpad press = teleport arc (release to jump; aim at an
// occupied vehicle to pull the driver out and take the seat; release inside a
// vehicle to bail out sideways), touchpad touch = fly-move (when enabled).

#include "sdk/main.h"
#include "sdk/natives.h"
#include "keyboard.hpp"
#include "keybinds.hpp"
#include "menu.hpp"
#include "settings.hpp"
#include "joystick.hpp"
#include "log.hpp"

#include "Vive/HMDSupport.hpp"

#include <fstream>
#include <string>
#include <cstdlib>
#include <cmath>

// The game's character switch (character wheel, F5-F8) is disabled every tick, VR on or off: an accidental key press
// would swap the player character and move the view far from the current scene. Only the game's control actions are
// disabled; the key presses themselves are not swallowed.
static void BlockCharacterSwitch()
{
	CONTROLS::DISABLE_CONTROL_ACTION(0, 19, 1);   // INPUT_CHARACTER_WHEEL (hold Alt)
	CONTROLS::DISABLE_CONTROL_ACTION(0, 166, 1);  // INPUT_SELECT_CHARACTER_MICHAEL   (F5)
	CONTROLS::DISABLE_CONTROL_ACTION(0, 167, 1);  // INPUT_SELECT_CHARACTER_FRANKLIN  (F6)
	CONTROLS::DISABLE_CONTROL_ACTION(0, 168, 1);  // INPUT_SELECT_CHARACTER_TREVOR    (F7)
	CONTROLS::DISABLE_CONTROL_ACTION(0, 169, 1);  // INPUT_SELECT_CHARACTER_MULTIPLAYER (F8)
}

using namespace OVRInject;

// --- constants recovered from the alpha binary (.rdata) ---

static const float WORLD_SCALE = 1.075f;        // VR meters -> game units
static const float ORIGIN_HEIGHT = 1.1549990177154541f; // VR floor sits this far below the player position (exact alpha constant, 0x3F93D702)
static const float DEG_TO_RAD = 0.017453292f;

// Gun barrel calibration: controller matrix -> aim direction offsets.
static const float GUN_ROT_X = -70.0f;
static const float GUN_ROT_Y = -7.25f;
static const float GUN_ROT_Z = -0.75f;

// Weapon prop rotation calibration (TrackedController::GetRotation offsets).
static const float PROP_ROT_X = -81.25f;
static const float PROP_ROT_Y = 257.0f;
static const float PROP_ROT_Z = 264.25f;
static const float PROP_ROT_Y_EXTRA = 56.5f;    // added on top of GetRotation().y

// Melee (grip) prop rotation calibration; these are the kx/ky/kz values the
// MeleeCalibrationDump key (default U) writes to gravovrvalues.txt.
static const float MELEE_ROT_X = 127.0f;
static const float MELEE_ROT_Y = 1.25f;
static const float MELEE_ROT_Z = -0.75f;

static const Hash WEAPON_PISTOL = 0x1B06D571;
static const Hash WEAPON_MICROSMG = 0x13532244;
static const Hash WEAPON_ASSAULTSHOTGUN = 0xE284C527;
static const Hash WEAPON_RPG = 0xB1CA77B1;
static const Hash WEAPON_KNUCKLE = 0xD8DF3C3C;

static const int BONE_CAMERA_ANCHOR = 11816;    // GET_PED_BONE_COORDS bone for the in-vehicle camera

static const int CONTROL_VEH_ACCELERATE = 71;
static const int CONTROL_VEH_BRAKE = 72;

// The alpha stores -1.0f's bit pattern (0xBF800000) as an integer and
// int->float converts it, so non-RPG bullets get this absurd speed. Kept
// for behavioral fidelity (the RPG passes a true -1.0f).
static const float BULLET_SPEED_QUIRK = 3212836864.0f;

// --- state (original .data/.bss addresses noted in RECONSTRUCTION.md) ---

static NativeVector3 g_origin;                    // VR play-space origin in world coordinates
static float g_fireCooldown = 0.0f;         // SMG auto-fire timer
static bool g_blink = false;                // toggled every frame; never read (leftover)
static bool g_flyMoveEnabled = false;       // FlyMove key (default 1)
static bool g_leftHandSteering = false;     // LeftHandSteering key (default 2)
static bool g_joystickSteering = false;     // never enabled anywhere in Alpha 0.1
static Object g_weaponProp = 0;             // hand-held weapon prop object
static float g_recenterYaw = 0.0f;          // head yaw captured on grip press (in vehicle)
static XMFLOAT3 g_anchorHeadPos = { 0.0f, 0.0f, 0.0f }; // head position captured on grip press
static int g_weaponIndex = 0;               // 0 pistol, 1 micro SMG, 2 assault shotgun, 3 RPG
static bool g_vrPropActive = false;         // set by the EnterVR key
static bool g_disableCamRot = false;        // never set anywhere in Alpha 0.1

// --- beta movement options, re-read from gtaovr\gtavive.ini once a second so
// edits apply without a restart. Everything defaults to OFF (= alpha behaviour).
struct BetaSettings
{
	bool moveWhereYouLook = false;  // stick-forward = where the headset looks
	bool anchorToPlayer = false;    // attach the on-foot camera to the ped (no walk jitter)
	bool stickTurn = false;         // right stick left/right turns the play space
	float stickTurnSpeed = 90.0f;   // degrees per second at full deflection
	float followSmoothMs = 0.0f;    // with anchorToPlayer: 0 = rigid, >0 = soft follow (time constant)
	float vehAttitudeFollow = 0.0f;   // 0 = horizon locked (alpha) .. 100 = view tilts fully with the vehicle
	float vehAttitudeDeadzone = 0.0f; // degrees of vehicle tilt ignored before following starts
	float vehAttitudeSmoothMs = 150.0f;
	float pedFallFollow = 0.0f;       // on foot: 0 = view never goes down with a fallen ped .. 100 = fully
	float heightOffsetCm = 0.0f;      // HeightOffsetCm: the VR viewpoint moved down (-) or up (+), in cm (tall players)
	bool centreOnStart = true;        // CentreOnVrStart: where you stand when VR starts = inside the character
};
static BetaSettings g_beta;
static float g_vehPitchLp = 0.0f, g_vehRollLp = 0.0f;
static float g_pedPitchLp = 0.0f, g_pedRollLp = 0.0f, g_fallBlend = 0.0f;
static const int BONE_HEAD = 31086; // SKEL_Head
static float g_turnYawDeg = 0.0f;
static NativeVector3 g_follow;
static bool g_followValid = false;
// Re-centre VR View: the headset's x/z (VR metres) that counts as standing inside the character. Head and hand
// positions are taken relative to it, so a re-centre moves both into the character and keeps their spacing.
static float g_centreX = 0.0f, g_centreZ = 0.0f;
static bool g_centreWanted = false;
static const char* g_centreCause = "VR start";   // what asked for the pending centre (trace / notification)
static int g_centreWaitFrames = 0;               // tracked frames still to skip (VR start: the pose may be stale)

// A decimal [Beta] value ("-12.5"), parsed by hand so the process locale never matters; def when unreadable
static float BetaDecimal(const char* key, float def, float lo, float hi, const char* path)
{
	return ReadIniDecimal("Beta", key, def, lo, hi, path);
}

static void LoadBetaSettings()
{
	// Once a second, and at once after the settings menu wrote a value
	static ULONGLONG lastLoad = 0;
	static unsigned lastGeneration = 0;
	ULONGLONG now = GetTickCount64();
	if (lastLoad != 0 && now - lastLoad < 1000 && SettingsGeneration() == lastGeneration)
		return;
	lastLoad = now;
	lastGeneration = SettingsGeneration();

	char path[MAX_PATH] = "";
	GetFullPathNameA("gtaovr\\gtavive.ini", MAX_PATH, path, nullptr);

	BetaSettings b;
	b.moveWhereYouLook = GetPrivateProfileIntA("Beta", "MoveWhereYouLook", 0, path) != 0;
	b.anchorToPlayer = GetPrivateProfileIntA("Beta", "AnchorCameraToPlayer", 0, path) != 0;
	b.stickTurn = GetPrivateProfileIntA("Beta", "StickTurn", 0, path) != 0;
	int speed = GetPrivateProfileIntA("Beta", "StickTurnSpeed", 90, path);
	b.stickTurnSpeed = (float)(speed < 10 ? 10 : speed > 360 ? 360 : speed);
	int follow = GetPrivateProfileIntA("Beta", "FollowSmoothMs", 0, path);
	b.followSmoothMs = (float)(follow < 0 ? 0 : follow > 2000 ? 2000 : follow);
	int att = GetPrivateProfileIntA("Beta", "VehicleAttitudeFollow", 0, path);
	b.vehAttitudeFollow = (float)(att < 0 ? 0 : att > 100 ? 100 : att);
	int attDz = GetPrivateProfileIntA("Beta", "VehicleAttitudeDeadzone", 0, path);
	b.vehAttitudeDeadzone = (float)(attDz < 0 ? 0 : attDz > 45 ? 45 : attDz);
	int attMs = GetPrivateProfileIntA("Beta", "VehicleAttitudeSmoothMs", 150, path);
	b.vehAttitudeSmoothMs = (float)(attMs < 0 ? 0 : attMs > 2000 ? 2000 : attMs);
	int fall = GetPrivateProfileIntA("Beta", "PedFallFollow", 0, path);
	b.pedFallFollow = (float)(fall < 0 ? 0 : fall > 100 ? 100 : fall);
	b.heightOffsetCm = BetaDecimal("HeightOffsetCm", 0.0f, -100.0f, 50.0f, path);
	if (b.heightOffsetCm != g_beta.heightOffsetCm)
		Trace("BETA settings: HeightOffsetCm=%.2f\n", b.heightOffsetCm);
	b.centreOnStart = GetPrivateProfileIntA("Beta", "CentreOnVrStart", 1, path) != 0;
	if (b.centreOnStart != g_beta.centreOnStart)
		Trace("BETA settings: CentreOnVrStart=%d\n", b.centreOnStart);
	if (b.pedFallFollow != g_beta.pedFallFollow)
		Trace("BETA settings: PedFallFollow=%.0f\n", b.pedFallFollow);

	if (b.moveWhereYouLook != g_beta.moveWhereYouLook || b.anchorToPlayer != g_beta.anchorToPlayer ||
		b.stickTurn != g_beta.stickTurn || b.stickTurnSpeed != g_beta.stickTurnSpeed ||
		b.followSmoothMs != g_beta.followSmoothMs || b.vehAttitudeFollow != g_beta.vehAttitudeFollow ||
		b.vehAttitudeDeadzone != g_beta.vehAttitudeDeadzone || b.vehAttitudeSmoothMs != g_beta.vehAttitudeSmoothMs)
		Trace("BETA settings: MoveWhereYouLook=%d AnchorCameraToPlayer=%d StickTurn=%d StickTurnSpeed=%.0f FollowSmoothMs=%.0f VehicleAttitudeFollow=%.0f Deadzone=%.0f SmoothMs=%.0f\n",
			b.moveWhereYouLook, b.anchorToPlayer, b.stickTurn, b.stickTurnSpeed, b.followSmoothMs,
			b.vehAttitudeFollow, b.vehAttitudeDeadzone, b.vehAttitudeSmoothMs);

	if (!b.stickTurn)
		g_turnYawDeg = 0.0f;
	g_beta = b;
}

static const int MAX_JOYSTICKS = 16;
static Joystick* g_joysticks[MAX_JOYSTICKS];

// World mapping used throughout (VR x is mirrored):
//   game.x = origin.x - vr.x * WORLD_SCALE
//   game.y = origin.y + vr.z * WORLD_SCALE
//   game.z = origin.z + vr.y * WORLD_SCALE

// Aim direction of the gun: third column of (controller matrix * calibration
// rotation), negated (OpenGL-style forward).
static XMFLOAT3 GetGunDirection(Matrix4 matrix)
{
	Matrix4 rot;
	rot.rotateX(GUN_ROT_X);
	rot.rotateY(GUN_ROT_Y);
	rot.rotateZ(GUN_ROT_Z);

	Matrix4 result = matrix * rot;

	XMFLOAT3 direction;
	direction.x = -result[8];
	direction.y = -result[9];
	direction.z = -result[10];
	return direction;
}

// Draws 8 triangles (each double-sided) as translucent red polys.
// verts = 8 triangles x 9 floats.
static void DrawTriangles(const float* verts)
{
	for (int i = 0; i < 8; i++)
	{
		const float* t = verts + i * 9;
		GRAPHICS::DRAW_POLY(t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], 255, 0, 0, 128);
		GRAPHICS::DRAW_POLY(t[6], t[7], t[8], t[3], t[4], t[5], t[0], t[1], t[2], 255, 0, 0, 128);
	}
}

// Casts the ballistic teleport arc from `start` along `direction`, drawing
// the beam. On a world hit it draws the play-space outline at the landing
// spot and writes the hit position to outHit; a vehicle hit draws a green box
// around it instead and, if outVehicle is given, writes the vehicle handle.
// Returns true if anything was hit within 48 steps.
static bool CastBeam(const NativeVector3& start, const NativeVector3& direction, const vr::HmdQuad_t& playArea,
	const NativeVector3& headOffset, NativeVector3* outHit, Entity* outVehicle)
{
	Ped playerPed = PLAYER::GET_PLAYER_PED(0);

	float prevX = start.x, prevY = start.y, prevZ = start.z;
	float vertStep = direction.y + 0.1f;

	for (unsigned int step = 0; step < 48; step++)
	{
		float nextX = prevX - direction.x;
		float nextY = prevY + direction.z;
		float nextZ = prevZ + vertStep;
		vertStep += -0.04f;

		int ray = SHAPETEST::_START_SHAPE_TEST_LOS_PROBE(prevX, prevY, prevZ, nextX, nextY, nextZ, 11, playerPed, 7);

		BOOL hit = FALSE;
		NativeVector3 endCoords = {};
		NativeVector3 surfaceNormal = {};
		Entity entityHit = 0;
		SHAPETEST::GET_SHAPE_TEST_RESULT(ray, &hit, &endCoords, &surfaceNormal, &entityHit);

		if (hit)
		{
			if (!ENTITY::IS_ENTITY_A_VEHICLE(entityHit))
			{
				GRAPHICS::DRAW_LINE(prevX, prevY, prevZ, endCoords.x, endCoords.y, endCoords.z, 255, 0, 0, 255);

				// Play-space outline at the landing spot: 4 wall segments,
				// 0.2 units tall, offset so the player's spot in the play
				// space lands on the hit point.
				float verts[72];
				int v = 0;
				for (unsigned int i = 1; i <= 4; i++)
				{
					int cur = static_cast<int>(i) % 4;
					int prev = static_cast<int>(i) - 1;

					float prevVX = playArea.vCorners[prev].v[0] * WORLD_SCALE + headOffset.x + endCoords.x;
					float prevVY = playArea.vCorners[prev].v[2] * WORLD_SCALE + (endCoords.y - headOffset.z);
					float prevVZ = endCoords.z + playArea.vCorners[prev].v[1];
					float curVX = playArea.vCorners[cur].v[0] * WORLD_SCALE + headOffset.x + endCoords.x;
					float curVY = playArea.vCorners[cur].v[2] * WORLD_SCALE + (endCoords.y - headOffset.z);
					float curVZ = endCoords.z + playArea.vCorners[cur].v[1];

					verts[v++] = prevVX; verts[v++] = prevVY; verts[v++] = prevVZ;
					verts[v++] = curVX;  verts[v++] = curVY;  verts[v++] = curVZ;
					verts[v++] = curVX;  verts[v++] = curVY;  verts[v++] = curVZ + 0.2f;
					verts[v++] = prevVX; verts[v++] = prevVY; verts[v++] = prevVZ;
					verts[v++] = curVX;  verts[v++] = curVY;  verts[v++] = curVZ + 0.2f;
					verts[v++] = prevVX; verts[v++] = prevVY; verts[v++] = prevVZ + 0.2f;
				}
				DrawTriangles(verts);

				if (outHit != nullptr)
				{
					outHit->x = endCoords.x;
					outHit->y = endCoords.y;
					outHit->z = endCoords.z;
				}
			}
			else
			{
				GRAPHICS::DRAW_BOX(endCoords.x - 0.5f, endCoords.y - 0.5f, endCoords.z - 0.25f,
					endCoords.x + 0.5f, endCoords.y + 0.5f, endCoords.z + 0.25f, 0, 255, 0, 128);
			}

			if (outVehicle != nullptr)
			{
				if (ENTITY::IS_ENTITY_A_VEHICLE(entityHit))
					*outVehicle = entityHit;
			}

			return true;
		}

		GRAPHICS::DRAW_LINE(prevX, prevY, prevZ, nextX, nextY, nextZ, 255, 0, 0, 255);

		prevX = nextX;
		prevY = nextY;
		prevZ = nextZ;
	}

	return false;
}

// Replaces the hand prop with the model for the given weapon slot at the
// player's feet (it gets attached/positioned every frame afterwards).
static void SpawnWeaponProp(int index)
{
	Ped playerPed = PLAYER::GET_PLAYER_PED(0);
	NativeVector3 pos = ENTITY::GET_ENTITY_COORDS(playerPed, 0);

	OBJECT::DELETE_OBJECT(&g_weaponProp);

	Hash weaponHash;
	switch (index)
	{
	case 0: weaponHash = WEAPON_PISTOL; break;
	case 1: weaponHash = WEAPON_MICROSMG; break;
	case 2: weaponHash = WEAPON_ASSAULTSHOTGUN; break;
	case 3: weaponHash = WEAPON_RPG; break;
	case 4: weaponHash = WEAPON_KNUCKLE; break;
	default: return;
	}

	Hash model = WEAPON::GET_WEAPONTYPE_MODEL(weaponHash);
	g_weaponProp = OBJECT::CREATE_OBJECT(model, pos.x, pos.y, pos.z - ORIGIN_HEIGHT, 1, 1, 0);
}

// Auto-start VR (gtavive.ini [Beta] AutoStartVR=1): arm without the EnterVR key once VR is up and the game is
// really playing - no loading screen, not faded out, the player exists - for 3 s in a row. Off (default) = the
// EnterVR key only. It only LOOKS at VR readiness (IsVRReadyPassive): IsVRReady() would ask OVRInject to retry
// starting VR on every frame while SteamVR isn't up (a failing VR_Init per frame, log spam, SteamVR relaunching).
// If VR wasn't up when the game started, one press of EnterVR still does the retry.
static ULONGLONG g_autoReadySince = 0;
static void AutoStartReset() { g_autoReadySince = 0; }

static bool AutoStartDue()
{
	static ULONGLONG lastRead = 0;
	static unsigned lastGeneration = 0;
	static bool enabled = false;
	ULONGLONG now = GetTickCount64();
	if (lastRead == 0 || now - lastRead >= 1000 || SettingsGeneration() != lastGeneration)
	{
		lastRead = now;
		lastGeneration = SettingsGeneration();
		char path[MAX_PATH] = "";
		GetFullPathNameA("gtaovr\\gtavive.ini", MAX_PATH, path, nullptr);
		enabled = GetPrivateProfileIntA("Beta", "AutoStartVR", 0, path) != 0;
	}
	if (!enabled || !HMDSupport::Singleton()->IsVRReadyPassive() || DLC::GET_IS_LOADING_SCREEN_ACTIVE() ||
		CAM::IS_SCREEN_FADED_OUT() || !ENTITY::DOES_ENTITY_EXIST(PLAYER::GET_PLAYER_PED(0)))
	{
		g_autoReadySince = 0;
		return false;
	}
	if (g_autoReadySince == 0)
		g_autoReadySince = now;
	return now - g_autoReadySince >= 3000;
}

// VR On / Off in game (VROff, default Ctrl+Shift+L): off hands the view back to the game camera and waits for on
// again, like before EnterVR. After a manual off, Auto-start VR stays out of it until VR is switched on again.
static bool g_vrManualOff = false;

static void VrNotify(const char* text)
{
	UI::_SET_NOTIFICATION_TEXT_ENTRY("STRING");
	UI::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME((char*)text);
	UI::_DRAW_NOTIFICATION(0, 1);
}

// Start of every script tick, VR on or off: read the key releases once, then let the settings menu run first so
// that, while it is open, no bind reaches the game code below it.
static MenuAction TickInput(bool vrActive)
{
	UpdateKeyReleases();
	LoadKeyBinds();
	Menu_Tick(vrActive);
	return Menu_TakeAction();
}

// One VR run: wait for on (EnterVR, VROff, the menu or Auto-start), run until switched off, clean up.
static void RunVrMode()
{
	// VR mode arms once EnterVR is pressed (or by itself with Auto-start VR). The pose accessors dereference
	// OpenVR interfaces, so never go past this point without a live HMD.
	while (true)
	{
		bool autoStart = false;
		AutoStartReset();   // every wait starts its own 3 s, also after a script restart
		while (true)
		{
			BlockCharacterSwitch();  // every tick, VR off too
			MenuAction action = TickInput(false);
			// VROff turns VR back on as well, so the same key switches both ways
			if (action == MENU_ACTION_VR_TOGGLE || BindPressed(BIND_ENTER_VR) || BindPressed(BIND_VR_OFF))
				break;
			if (!g_vrManualOff && !Menu_IsOpen() && AutoStartDue())
			{
				autoStart = true;
				break;
			}
			WAIT(0);
		}
		g_vrManualOff = false;
		if (autoStart)
			Trace("Auto-start VR: headset ready, arming without the EnterVR key\n");

		if (HMDSupport::Singleton()->IsVRReady())
			break;

		Trace("EnterVR: VR not ready\n");

		Log("EnterVR pressed but VR is not initialized\n");

		char enterKey[64], message[200];
		GetBindText(BIND_ENTER_VR, enterKey, sizeof(enterKey));
		_snprintf_s(message, sizeof(message), _TRUNCATE,
			"GTA: Vive - VR is not ready. Start SteamVR, then press %s again (see gtavrInjectLog.txt).", enterKey);
		VrNotify(message);

		WAIT(0);
	}

	Cam cam = CAM::CREATE_CAM_WITH_PARAMS("DEFAULT_SCRIPTED_CAMERA",
		0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 100.0f, 0, 0);
	Trace("VR ARMED: cam=%d player=%d\n", cam, PLAYER::GET_PLAYER_PED(0));
	// Each VR run starts as fresh as the first one (otherwise a run after VR Off would keep these: the view creeps
	// in from the old origin, the follow camera glides from where it was, the hand prop index is stale)
	g_origin.x = g_origin.y = g_origin.z = 0.0f;
	g_followValid = false;
	g_weaponIndex = 0;
	g_fallBlend = 0.0f;
	LoadBetaSettings();
	g_centreX = g_centreZ = 0.0f;
	g_centreWanted = g_beta.centreOnStart;   // taken on the 3rd tracked frame (the first can still hold
	g_centreWaitFrames = 3;                  // the previous run's pose after VR Off / On)
	g_centreCause = "VR start";

	{
		Ped playerPed = PLAYER::GET_PLAYER_PED(0);
		NativeVector3 pos = ENTITY::GET_ENTITY_COORDS(playerPed, 0);
		Hash model = WEAPON::GET_WEAPONTYPE_MODEL(WEAPON_PISTOL);
		g_weaponProp = OBJECT::CREATE_OBJECT(model, pos.x, pos.y, pos.z - ORIGIN_HEIGHT, 1, 1, 0);
		g_vrPropActive = true;
	}

	int joystickCount = GetConnectedJoystickCount();
	Log("Number of joysticks connected: %d\n", joystickCount);
	for (int i = 0; i < joystickCount && i < MAX_JOYSTICKS; i++)
	{
		if (g_joysticks[i] != nullptr) continue;   // a later VR run keeps the first one's
		g_joysticks[i] = new Joystick(i);
		g_joysticks[i]->Initialize();

		char productName[MAX_PATH] = "";
		if (g_joysticks[i]->device_ != nullptr)
		{
			DIDEVICEINSTANCEA info = {};
			info.dwSize = sizeof(DIDEVICEINSTANCEA);
			if (SUCCEEDED(g_joysticks[i]->device_->GetDeviceInfo(&info)))
				strncpy_s(productName, info.tszProductName, _TRUNCATE);
		}
		Log("Initializing Joystick %d: %s\n", i, productName);
	}

	DIJOYSTATE joyState = {};
	float prevGunX = 0.0f, prevGunY = 0.0f, prevGunZ = 0.0f;

	while (true)
	{
		MenuAction menuAction = TickInput(true);

		// --- joystick poll (dead in Alpha 0.1: g_joystickSteering never becomes true) ---
		if (g_joystickSteering && g_joysticks[0] != nullptr && g_joysticks[0]->device_ != nullptr)
		{
			IDirectInputDevice8A* device = g_joysticks[0]->device_;
			bool acquired = true;
			HRESULT hr = device->Poll();
			if (FAILED(hr))
			{
				hr = device->Acquire();
				while (hr == DIERR_INPUTLOST)
					hr = device->Acquire();
				if (hr == DIERR_OTHERAPPHASPRIO || hr == DIERR_NOTINITIALIZED || hr == DIERR_INVALIDPARAM)
					acquired = false;
			}
			if (acquired)
				device->GetDeviceState(sizeof(DIJOYSTATE), &joyState);
		}

		Ped playerPed = PLAYER::GET_PLAYER_PED(0);

		HMDSupport* hmd = HMDSupport::Singleton();
		hmd->SyncOnPoses();
		TrackedController* rightHand = hmd->GetRightHand();

		// State captured at the end of the previous frame (UpdateButtonState
		// runs mid-loop, after the camera is placed).
		TrackedController::ButtonState buttons = rightHand->GetButtonState();

		XMFLOAT3 headPos = hmd->GetHeadPosition(0.0f);
		// Re-centre VR View: RecentreView key or the menu, and at VR start with CentreOnVrStart. Taken only from a
		// tracked pose (0,0,0 = not tracking yet).
		LoadBetaSettings();   // read before use (it re-reads at most once a second), so the first VR frame has it too
		bool recentreKey = BindPressed(BIND_RECENTRE);
		if (recentreKey || menuAction == MENU_ACTION_RECENTRE)
		{
			g_centreWanted = true;
			// kept until the centre is taken, for the trace and the notification
			g_centreCause = recentreKey ? "RecentreView key" : "menu";
		}
		bool tracked = headPos.x != 0.0f || headPos.y != 0.0f || headPos.z != 0.0f;
		if (g_centreWanted && tracked && --g_centreWaitFrames <= 0)
		{
			g_centreX = headPos.x;
			g_centreZ = headPos.z;
			g_centreWanted = false;
			g_centreWaitFrames = 0;
			Trace("VR RECENTRE (%s): centre=(%.2f,%.2f) m\n", g_centreCause, g_centreX, g_centreZ);
			if (strcmp(g_centreCause, "VR start") != 0)
				VrNotify(PED::IS_PED_IN_ANY_VEHICLE(playerPed, 0)
					? "GTA: Vive - VR view re-centred (on foot: in a vehicle the seat places you)"
					: "GTA: Vive - VR view re-centred on your character");
		}
		float headX = (headPos.x - g_centreX) * WORLD_SCALE;
		float headY = headPos.y * WORLD_SCALE;
		float headZ = (headPos.z - g_centreZ) * WORLD_SCALE;
		// VR Height (HeightOffsetCm): the viewpoint moved down / up in game, for players taller or shorter than the
		// character. Applies to every camera path that builds its height from headY (and to the hand prop on foot,
		// below, so head and hands move together like SteamVR's own height setting)
		headY += g_beta.heightOffsetCm / 100.0f * WORLD_SCALE;

		// --- play-space bounds in world coordinates (walk redirection) ---
		vr::HmdQuad_t playArea = hmd->GetPlaySpaceSize();
		float minX = 0.0f, maxX = 0.0f, minY = 0.0f, maxY = 0.0f;
		for (int i = 0; i < 4; i++)
		{
			float wx = playArea.vCorners[i].v[0] * WORLD_SCALE + g_origin.x;
			float wy = playArea.vCorners[i].v[2] * WORLD_SCALE + g_origin.y;
			if (i == 0)
			{
				minX = maxX = wx;
				minY = maxY = wy;
			}
			else
			{
				if (wx < minX) minX = wx;
				if (wx > maxX) maxX = wx;
				if (wy < minY) minY = wy;
				if (wy > maxY) maxY = wy;
			}
		}

		NativeVector3 playerPos = ENTITY::GET_ENTITY_COORDS(playerPed, 0);

		// Walking in the real play space drags the world origin along; if the
		// player got moved far away (respawn etc.) the origin snaps to them.
		float newOriginX = playerPos.x;
		float newOriginY = playerPos.y;
		if (fabsf(playerPos.y - maxY) < 10.0f && fabsf(playerPos.y - minY) < 10.0f &&
			fabsf(playerPos.x - maxX) < 10.0f && fabsf(playerPos.x - minX) < 10.0f)
		{
			if (playerPos.x < minX)
				g_origin.x += -0.05f;
			if (maxX < playerPos.x)
				g_origin.x += 0.05f;
			if (playerPos.y < minY)
				g_origin.y += -0.05f;
			if (maxY < playerPos.y)
				g_origin.y += 0.05f;
			newOriginX = g_origin.x;
			newOriginY = g_origin.y;
		}
		g_origin.x = newOriginX;
		g_origin.y = newOriginY;
		g_origin.z = playerPos.z - ORIGIN_HEIGHT;
		g_blink = !g_blink;

		// --- on-foot camera position ---
		float camX = g_origin.x - headX;
		float camY = g_origin.y + headZ;
		float camZ = (playerPos.z - ORIGIN_HEIGHT) + headY;

		XMFLOAT3 handPos = rightHand->GetPosition();
		XMFLOAT3 propRot = rightHand->GetRotation(PROP_ROT_X, PROP_ROT_Y, PROP_ROT_Z);

		float gunX = g_origin.x - (handPos.x - g_centreX) * WORLD_SCALE;   // relative to the re-centred spot
		float gunY = (handPos.z - g_centreZ) * WORLD_SCALE + g_origin.y;
		float gunZ = handPos.y * WORLD_SCALE + g_origin.z;
		gunZ += g_beta.heightOffsetCm / 100.0f * WORLD_SCALE;   // the hand moves with the shifted view (on foot)

		XMFLOAT3 headRot = hmd->GetHeadRotation();

		float yawDeltaDeg = 0.0f;   // vehicle heading offset added to yaws
		float yawDeltaRad = 0.0f;   // same, in radians, applied to aim directions

		// --- beta movement options (gtaovr\gtavive.ini [Beta], all off by default) ---
		LoadBetaSettings();
		bool anchoredOnFoot = false;
		float anchorOffX = 0.0f, anchorOffY = 0.0f, anchorOffZ = 0.0f;
		float turnYawDeg = 0.0f;
		float attPitchDeg = 0.0f, attRollDeg = 0.0f; // vehicle attitude carried into the view

		if (!PED::IS_PED_IN_ANY_VEHICLE(playerPed, 0))
		{
			ENTITY::DETACH_ENTITY(g_weaponProp, 0, 0);

			// StickTurn: the right stick's left/right rotates the whole play space
			// around the player (smooth turn), since a seated player can't turn
			// their body. The stick no longer swings the hidden gameplay camera.
			if (g_beta.stickTurn)
			{
				CONTROLS::DISABLE_CONTROL_ACTION(0, 1, 1); // INPUT_LOOK_LR
				float x = CONTROLS::GET_DISABLED_CONTROL_NORMAL(0, 1);
				if (fabsf(x) > 0.15f && !CONTROLS::_IS_USING_KEYBOARD(2))
					g_turnYawDeg -= x * g_beta.stickTurnSpeed * GAMEPLAY::GET_FRAME_TIME();
				if (g_turnYawDeg > 180.0f) g_turnYawDeg -= 360.0f;
				if (g_turnYawDeg < -180.0f) g_turnYawDeg += 360.0f;
				turnYawDeg = g_turnYawDeg;
			}

			// head offset from the player in game axes, rotated by the stick turn
			float offX = -headX, offY = headZ;
			if (turnYawDeg != 0.0f)
			{
				float c = cosf(turnYawDeg * DEG_TO_RAD), s = sinf(turnYawDeg * DEG_TO_RAD);
				float rx = offX * c - offY * s;
				float ry = offX * s + offY * c;
				offX = rx; offY = ry;
				camX = g_origin.x + offX;
				camY = g_origin.y + offY;
			}

			// AnchorCameraToPlayer: instead of re-placing the camera from the ped's
			// position once per script tick (and dragging the origin after a
			// stick-walking ped in 5 cm steps - the jitter), attach it to the ped.
			// The game then resolves the position at render time, in step with the ped.
			if (g_beta.anchorToPlayer)
			{
				g_origin.x = playerPos.x;
				g_origin.y = playerPos.y;

				if (g_beta.followSmoothMs <= 0.0f)
				{
					// rigid: glued to the ped
					anchoredOnFoot = true;
					anchorOffX = offX;
					anchorOffY = offY;
					anchorOffZ = headY - ORIGIN_HEIGHT;
					camX = playerPos.x + offX;
					camY = playerPos.y + offY;
					g_followValid = false;
				}
				else
				{
					// FollowSmoothMs: the camera trails the ped through a low-pass
					// with that time constant - a soft, steadicam-like follow that
					// takes the ped's footstep bob and sudden starts/stops out, and
					// (unlike the alpha's chase) never falls behind and never snaps.
					float dx = playerPos.x - g_follow.x, dy = playerPos.y - g_follow.y, dz = playerPos.z - g_follow.z;
					if (!g_followValid || dx * dx + dy * dy + dz * dz > 25.0f * 25.0f)
					{
						g_follow = playerPos; // first frame, or a teleport / respawn
						g_followValid = true;
					}
					else
					{
						float a = 1.0f - expf(-GAMEPLAY::GET_FRAME_TIME() / (g_beta.followSmoothMs / 1000.0f));
						g_follow.x += dx * a;
						g_follow.y += dy * a;
						g_follow.z += dz * a;
					}

					CAM::DETACH_CAM(cam);
					camX = g_follow.x + offX;
					camY = g_follow.y + offY;
					camZ = (g_follow.z - ORIGIN_HEIGHT) + headY;
				}
			}
			else
			{
				CAM::DETACH_CAM(cam);
				g_followValid = false;
			}
		}
		else
		{
			Vehicle vehicle = PED::GET_VEHICLE_PED_IS_USING(playerPed);

			// --- driving input ---
			if (g_joystickSteering)
			{
				VEHICLE::SET_VEHICLE_STEER_BIAS(vehicle,
					-((float)(joyState.lX - 0x7fff) * 0.000030518044f));
				CONTROLS::_SET_CONTROL_NORMAL(0, CONTROL_VEH_ACCELERATE,
					(float)(0xffff - joyState.lRz) * 0.000015259022f + 0.5f);
				CONTROLS::_SET_CONTROL_NORMAL(0, CONTROL_VEH_BRAKE,
					(float)(0xffff - joyState.lY) * 0.000015259022f + 0.5f);
			}
			else if (g_leftHandSteering)
			{
				TrackedController* leftHand = hmd->GetLeftHand();
				leftHand->UpdateButtonState();
				TrackedController::ButtonState leftButtons = leftHand->GetButtonState();
				XMFLOAT3 leftRot = leftHand->GetRotation(0.0f, 0.0f, 0.0f);

				VEHICLE::SET_VEHICLE_STEER_BIAS(vehicle, leftRot.x * -0.0055555557f * 0.5f);
				CONTROLS::_SET_CONTROL_NORMAL(0, CONTROL_VEH_ACCELERATE, leftButtons.triggerMargin);
				CONTROLS::_SET_CONTROL_NORMAL(0, CONTROL_VEH_BRAKE, leftButtons.padPressed ? 1.0f : 0.0f);
			}
			else
			{
				TrackedController::ButtonState padButtons = rightHand->GetButtonState();
				if (padButtons.touchContact)
				{
					VEHICLE::SET_VEHICLE_STEER_BIAS(vehicle, padButtons.touchX * -0.1f);
					CONTROLS::_SET_CONTROL_NORMAL(0, CONTROL_VEH_ACCELERATE, padButtons.touchY);
				}
			}

			// --- vehicle camera: seat-anchored, recentered by grip ---
			NativeVector3 vehicleRot = ENTITY::GET_ENTITY_ROTATION(vehicle, 0);
			yawDeltaDeg = vehicleRot.z - g_recenterYaw;

			if (buttons.gripPressed)
			{
				g_anchorHeadPos = hmd->GetHeadPosition(0.0f);
				g_recenterYaw = headRot.z;
				vehicleRot = ENTITY::GET_ENTITY_ROTATION(vehicle, 0);
				yawDeltaDeg = vehicleRot.z - g_recenterYaw;
			}

			XMFLOAT3 headNow = hmd->GetHeadPosition(0.0f);
			float c = cosf(-(yawDeltaDeg * DEG_TO_RAD));
			float s = sinf(-(yawDeltaDeg * DEG_TO_RAD));

			// head position rotated around the grip anchor
			float relX = headNow.x - g_anchorHeadPos.x;
			float relZ = headNow.z - g_anchorHeadPos.z;
			float rotatedX = (relX * c + g_anchorHeadPos.x) - relZ * s;
			float rotatedZ = relX * s + g_anchorHeadPos.z + relZ * c;

			NativeVector3 bone = PED::GET_PED_BONE_COORDS(playerPed, BONE_CAMERA_ANCHOR, 0.0f, 0.0f, 0.0f);

			float camOffX = g_anchorHeadPos.x - rotatedX;
			float camOffY = rotatedZ - g_anchorHeadPos.z;
			float camOffZ = (headNow.y - g_anchorHeadPos.y) + 0.675f;

			// hand position relative to the head, same rotation
			XMFLOAT3 handNow = rightHand->GetPosition();
			float handRelX = handNow.x - headNow.x;
			float handRelY = handNow.y - headNow.y;
			float handRelZ = handNow.z - headNow.z;
			float handRotX = handRelX * c - handRelZ * s;
			float handRotZ = handRelZ * c + handRelX * s;

			CAM::ATTACH_CAM_TO_ENTITY(cam, playerPed, camOffX, camOffY, camOffZ, 0);
			ENTITY::ATTACH_ENTITY_TO_ENTITY(g_weaponProp, playerPed, 0,
				camOffX - handRotX, handRotZ + camOffY, handRelY + camOffZ,
				propRot.x, propRot.y + PROP_ROT_Y_EXTRA, propRot.z + yawDeltaDeg,
				2, 1, 0, 1, 0, 0);

			camX = (bone.x - rotatedX) + g_anchorHeadPos.x;
			camY = (bone.y + rotatedZ) - g_anchorHeadPos.z;
			camZ = ((bone.z + headNow.y) - g_anchorHeadPos.y) + 0.675f;

			gunX = camX - handRotX;
			gunY = handRotZ + camY;
			gunZ = handRelY + camZ;

			yawDeltaRad = yawDeltaDeg * DEG_TO_RAD;

			// VehicleAttitudeFollow (beta): the alpha takes the vehicle's heading but
			// keeps the view locked to the horizon - climb a hill or roll over and the
			// camera stays level. Blend in the vehicle's pitch and roll:
			//   amount   0 = horizon locked (alpha) ... 100 = rigidly bolted to the car
			//   deadzone degrees of tilt ignored before it starts to follow (small
			//            bumps stay level, big slopes carry over)
			//   smoothMs low-pass, so suspension chatter doesn't shake the view
			if (g_beta.vehAttitudeFollow > 0.0f)
			{
				NativeVector3 vr2 = ENTITY::GET_ENTITY_ROTATION(vehicle, 2);
				float a = g_beta.vehAttitudeSmoothMs > 0.0f
					? 1.0f - expf(-GAMEPLAY::GET_FRAME_TIME() / (g_beta.vehAttitudeSmoothMs / 1000.0f)) : 1.0f;
				g_vehPitchLp += (vr2.x - g_vehPitchLp) * a;
				g_vehRollLp += (vr2.y - g_vehRollLp) * a;

				float dz = g_beta.vehAttitudeDeadzone;
				float vp = fabsf(g_vehPitchLp) <= dz ? 0.0f : (g_vehPitchLp > 0.0f ? g_vehPitchLp - dz : g_vehPitchLp + dz);
				float vroll = fabsf(g_vehRollLp) <= dz ? 0.0f : (g_vehRollLp > 0.0f ? g_vehRollLp - dz : g_vehRollLp + dz);
				float k = g_beta.vehAttitudeFollow / 100.0f;

				// The car's tilt as seen from where the head is looking: facing
				// forward its pitch is pitch; looking out of the side window the
				// same nose-up tilt shows up as roll (and vice versa).
				float psi = ((headRot.z + yawDeltaDeg) - vr2.z) * DEG_TO_RAD;
				attPitchDeg = k * (vp * cosf(psi) + vroll * sinf(psi));
				attRollDeg = k * (vroll * cosf(psi) - vp * sinf(psi));
			}
			else
			{
				g_vehPitchLp = 0.0f;
				g_vehRollLp = 0.0f;
			}
		}

		// PedFallFollow (beta): on foot the view floats upright at standing height no
		// matter what happens to the body - get hit by a car and the camera just hangs
		// there. When the ped ragdolls or falls, carry the view down WITH the body:
		// toward the ped's head, tilting with it. The amount is how firmly the head is
		// "held up" - 0 = never goes down (alpha), 100 = goes all the way with the body.
		// Shares the tilt deadzone / smoothing with the vehicle option.
		if (g_beta.pedFallFollow > 0.0f && !PED::IS_PED_IN_ANY_VEHICLE(playerPed, 0))
		{
			bool down = PED::IS_PED_RAGDOLL(playerPed) || PED::IS_PED_FALLING(playerPed);
			float a = g_beta.vehAttitudeSmoothMs > 0.0f
				? 1.0f - expf(-GAMEPLAY::GET_FRAME_TIME() / (g_beta.vehAttitudeSmoothMs / 1000.0f)) : 1.0f;
			g_fallBlend += ((down ? 1.0f : 0.0f) - g_fallBlend) * a; // eases in on the fall, out on getting up
			float k = (g_beta.pedFallFollow / 100.0f) * g_fallBlend;

			if (k > 0.001f)
			{
				NativeVector3 pr = ENTITY::GET_ENTITY_ROTATION(playerPed, 2);
				g_pedPitchLp += (pr.x - g_pedPitchLp) * a;
				g_pedRollLp += (pr.y - g_pedRollLp) * a;

				float dz = g_beta.vehAttitudeDeadzone;
				float pp = fabsf(g_pedPitchLp) <= dz ? 0.0f : (g_pedPitchLp > 0.0f ? g_pedPitchLp - dz : g_pedPitchLp + dz);
				float prl = fabsf(g_pedRollLp) <= dz ? 0.0f : (g_pedRollLp > 0.0f ? g_pedRollLp - dz : g_pedRollLp + dz);
				float psi = ((headRot.z + turnYawDeg) - pr.z) * DEG_TO_RAD;
				attPitchDeg = k * (pp * cosf(psi) + prl * sinf(psi));
				attRollDeg = k * (prl * cosf(psi) - pp * sinf(psi));

				// position: from the floating standing-height spot toward the actual head
				NativeVector3 head = PED::GET_PED_BONE_COORDS(playerPed, BONE_HEAD, 0.0f, 0.0f, 0.0f);
				camX += (head.x - camX) * k;
				camY += (head.y - camY) * k;
				camZ += (head.z - camZ) * k;

				if (anchoredOnFoot)
				{
					anchorOffX = camX - playerPos.x;
					anchorOffY = camY - playerPos.y;
					anchorOffZ = camZ - playerPos.z;
				}
			}
			else
			{
				g_pedPitchLp = 0.0f;
				g_pedRollLp = 0.0f;
			}
		}
		else
			g_fallBlend = 0.0f;

		float camYawDeg = headRot.z + yawDeltaDeg + turnYawDeg;

		if (anchoredOnFoot)
			CAM::ATTACH_CAM_TO_ENTITY(cam, playerPed, anchorOffX, anchorOffY, anchorOffZ, 0); // world-axis offset
		else
			CAM::SET_CAM_COORD(cam, camX, camY, camZ);
		CAM::SET_CAM_ACTIVE(cam, 1);
		CAM::SET_CAM_NEAR_CLIP(cam, 0.075f);
		if (!g_disableCamRot)
			CAM::SET_CAM_ROT(cam, headRot.x + attPitchDeg, headRot.y + attRollDeg, camYawDeg, 2);
		CAM::RENDER_SCRIPT_CAMS(1, 0, cam, 1, 0);
		BlockCharacterSwitch();

		// MoveWhereYouLook: the left stick moves the ped relative to the hidden
		// GAMEPLAY camera, not the VR one - so "forward" pointed somewhere else and
		// kept changing as that camera swung behind the turning ped (the circles).
		// Pin the gameplay camera's heading to where the headset looks; in first
		// person this also makes the ped face where you look.
		if (g_beta.moveWhereYouLook && !PED::IS_PED_IN_ANY_VEHICLE(playerPed, 0))
		{
			float rel = camYawDeg - ENTITY::GET_ENTITY_HEADING(playerPed);
			while (rel > 180.0f) rel -= 360.0f;
			while (rel < -180.0f) rel += 360.0f;
			CAM::SET_GAMEPLAY_CAM_RELATIVE_HEADING(rel);
			CAM::SET_GAMEPLAY_CAM_RELATIVE_PITCH(0.0f, 1.0f);
		}

		{
			static ULONGLONG lastTrace = 0;
			static Ped lastPlayer = 0;
			ULONGLONG now = GetTickCount64();
			if (playerPed != lastPlayer)
			{
				Trace("PLAYER PED CHANGED %d -> %d\n", lastPlayer, playerPed);
				lastPlayer = playerPed;
			}
			if (now - lastTrace >= 100)
			{
				lastTrace = now;
				NativeVector3 pp = ENTITY::GET_ENTITY_COORDS(playerPed, 0);
				Trace("SAMPLE player=%d pos=(%.2f,%.2f,%.2f) inVeh=%d | cam=(%.2f,%.2f,%.2f) rot=(%.1f,%.1f,%.1f) yawDelta=%.1f | origin=(%.2f,%.2f,%.2f) head=(%.2f,%.2f,%.2f)\n",
					playerPed, pp.x, pp.y, pp.z, PED::IS_PED_IN_ANY_VEHICLE(playerPed, 0),
					camX, camY, camZ, headRot.x, headRot.y, headRot.z + yawDeltaDeg, yawDeltaDeg,
					g_origin.x, g_origin.y, g_origin.z, headX, headY, headZ);
			}
		}

		rightHand->UpdateButtonState();

		NativeVector3 playerPosNow = ENTITY::GET_ENTITY_COORDS(playerPed, 0);

		// --- hotkeys (gtavive.ini [Keys]; the release snapshot was taken by TickInput at the top of the tick) ---
		if (BindPressed(BIND_FLY_MOVE))
			g_flyMoveEnabled = !g_flyMoveEnabled;
		if (BindPressed(BIND_LEFT_HAND_STEERING))
			g_leftHandSteering = !g_leftHandSteering;
		if (BindPressed(BIND_DESKTOP_MIRROR))
			hmd->SetDesktopMirroring(!hmd->GetDesktopMirroring());
		// VR off wins when EnterVR and VROff share a key, so that key toggles VR
		bool vrOffKey = BindPressed(BIND_VR_OFF);
		if (vrOffKey || menuAction == MENU_ACTION_VR_TOGGLE)
		{
			// --- VR off: give the view back to the game camera, show the player, drop GTAVive's props ---
			Trace("VR OFF by %s\n", vrOffKey ? "VROff key" : "menu");
			CAM::RENDER_SCRIPT_CAMS(0, 0, 0, 1, 0);
			CAM::DESTROY_CAM(cam, 0);
			if (g_weaponProp != 0 && ENTITY::DOES_ENTITY_EXIST(g_weaponProp))
				OBJECT::DELETE_OBJECT(&g_weaponProp);
			g_weaponProp = 0;
			g_vrPropActive = false;
			Ped offPed = PLAYER::GET_PLAYER_PED(0);
			ENTITY::SET_ENTITY_VISIBLE(offPed, 1, 0);
			ENTITY::RESET_ENTITY_ALPHA(offPed);
			hmd->SetDesktopMirroring(true);   // the monitor shows the game again even if mirroring was off (0 key)
			g_vrManualOff = true;
			char offKey[64], message[160];
			GetBindText(BIND_VR_OFF, offKey, sizeof(offKey));
			_snprintf_s(message, sizeof(message), _TRUNCATE, "GTA: Vive - VR off. %s turns it back on.", offKey);
			VrNotify(message);
			return;
		}
		if (BindPressed(BIND_ENTER_VR))
		{
			Hash model = WEAPON::GET_WEAPONTYPE_MODEL(WEAPON_PISTOL);
			g_weaponProp = OBJECT::CREATE_OBJECT(model,
				playerPosNow.x, playerPosNow.y, playerPosNow.z - ORIGIN_HEIGHT, 1, 1, 0);
			g_vrPropActive = true;
		}
		if (BindPressed(BIND_MELEE_CALIBRATION_DUMP))
		{
			std::ofstream file("gravovrvalues.txt");
			file << "kx\n" << std::to_string(MELEE_ROT_X) << "\n";
			file << "ky\n" << std::to_string(MELEE_ROT_Y) << "\n";
			file << "kz\n" << std::to_string(MELEE_ROT_Z) << "\n";
		}

		// --- VR hands: weapons, melee, locomotion ---
		if (g_vrPropActive && buttons.valid_)
		{
			if (buttons.gripJustPressed)
			{
				Ped ped = PLAYER::GET_PLAYER_PED(0);
				NativeVector3 pos = ENTITY::GET_ENTITY_COORDS(ped, 0);
				OBJECT::DELETE_OBJECT(&g_weaponProp);
				Hash model = WEAPON::GET_WEAPONTYPE_MODEL(WEAPON_KNUCKLE);
				g_weaponProp = OBJECT::CREATE_OBJECT(model, pos.x, pos.y, pos.z - ORIGIN_HEIGHT, 1, 1, 0);
			}

			// touchpad fly-move (FlyMove key): touch position steers, gun points the way
			if (g_flyMoveEnabled)
			{
				XMFLOAT3 dir = GetGunDirection(rightHand->GetMatrix());

				XMVECTOR forward = DirectX::XMVector3Normalize(DirectX::XMVectorSet(-dir.x, -dir.z, 0.0f, 1.0f));
				XMVECTOR side = DirectX::XMVector3Normalize(
					DirectX::XMVector3Cross(DirectX::XMVectorSet(-dir.x, -dir.z, 0.0f, 1.0f),
						DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 1.0f)));

				NativeVector3 velocity = ENTITY::GET_ENTITY_VELOCITY(playerPed);

				float moveX = DirectX::XMVectorGetX(forward) * buttons.touchY
					- DirectX::XMVectorGetX(side) * buttons.touchX;
				float moveY = DirectX::XMVectorGetY(side) * buttons.touchX
					- DirectX::XMVectorGetY(forward) * buttons.touchY;

				if (buttons.touchContact)
					ENTITY::SET_ENTITY_VELOCITY(playerPed, moveX * 5.0f, moveY * 5.0f, velocity.z);
			}

			// on foot the prop follows the hand directly
			if (!PED::IS_PED_IN_ANY_VEHICLE(playerPed, 0))
			{
				ENTITY::SET_ENTITY_COORDS(g_weaponProp, gunX, gunY, gunZ, 0, 0, 0, 0);
				ENTITY::SET_ENTITY_ROTATION(g_weaponProp,
					propRot.x, propRot.y + PROP_ROT_Y_EXTRA, propRot.z + yawDeltaDeg, 2, 0);
			}

			if (buttons.gripJustReleased)
				SpawnWeaponProp(g_weaponIndex);

			// melee: swing the knuckles - capsule test along the extrapolated hand path
			if (buttons.gripPressed)
			{
				XMFLOAT3 meleeRot = rightHand->GetRotation(MELEE_ROT_X, MELEE_ROT_Y, MELEE_ROT_Z);
				ENTITY::SET_ENTITY_ROTATION(g_weaponProp, meleeRot.x, meleeRot.y, meleeRot.z, 2, 0);

				// original quirk: X extrapolates from the previous position,
				// Y/Z from the current one
				float swingX = (gunX - prevGunX) * 10.0f + prevGunX;
				float swingY = (gunY - prevGunY) * 10.0f + gunY;
				float swingZ = (gunZ - prevGunZ) * 10.0f + gunZ;

				int ray = SHAPETEST::START_SHAPE_TEST_CAPSULE(prevGunX, prevGunY, prevGunZ,
					swingX, swingY, swingZ, 0.01f, 12, playerPed, 7);

				BOOL hit = FALSE;
				NativeVector3 endCoords = {};
				NativeVector3 surfaceNormal = {};
				Entity entityHit = 0;
				SHAPETEST::GET_SHAPE_TEST_RESULT(ray, &hit, &endCoords, &surfaceNormal, &entityHit);

				if (hit && ENTITY::GET_ENTITY_TYPE(entityHit) == 1)
				{
					PED::APPLY_DAMAGE_TO_PED(entityHit, 10, 1);
					PED::SET_PED_CAN_RAGDOLL(entityHit, 1);
					PED::SET_PED_TO_RAGDOLL(entityHit, 750, 7500000, 1, 1, 1, 1);
					ENTITY::APPLY_FORCE_TO_ENTITY(entityHit, 3,
						(swingX - prevGunX) * 2.0f, (swingY - prevGunY) * 2.0f, (swingZ - prevGunZ) * 2.0f,
						0.0f, 0.0f, 0.0f, 0, 0, 1, 1, 0, 1);
					ENTITY::CLEAR_ENTITY_LAST_DAMAGE_ENTITY(entityHit);
				}
			}

			prevGunX = gunX;
			prevGunY = gunY;
			prevGunZ = gunZ;

			// --- firing ---
			if (g_weaponIndex == 0)
			{
				if (buttons.triggerJustPressed)
				{
					XMFLOAT3 dir = GetGunDirection(rightHand->GetMatrix());
					float c = cosf(yawDeltaRad);
					float s = sinf(yawDeltaRad);
					float targetX = (float)(rand() % 500 - 250) * 0.02f + (-dir.x * c - dir.z * s) * 1000.0f + gunX;
					float targetY = (float)(rand() % 500 - 250) * 0.02f + (dir.z * c - dir.x * s) * 1000.0f + gunY;
					float targetZ = (float)(rand() % 500 - 250) * 0.02f + dir.y * 1000.0f + gunZ;
					GAMEPLAY::SHOOT_SINGLE_BULLET_BETWEEN_COORDS(gunX, gunY, gunZ,
						targetX, targetY, targetZ, 10, 1, WEAPON_PISTOL, playerPed, 1, 1, BULLET_SPEED_QUIRK);
				}
			}
			else if (g_weaponIndex == 1)
			{
				if (buttons.triggerPressed)
				{
					if (g_fireCooldown <= 0.0f)
					{
						XMFLOAT3 dir = GetGunDirection(rightHand->GetMatrix());
						float c = cosf(yawDeltaRad);
						float s = sinf(yawDeltaRad);
						float targetX = (float)(rand() % 1000 - 500) * 0.02f + (-dir.x * c - dir.z * s) * 1000.0f + gunX;
						float targetY = (float)(rand() % 1000 - 500) * 0.02f + (dir.z * c - dir.x * s) * 1000.0f + gunY;
						float targetZ = (float)(rand() % 1000 - 500) * 0.02f + dir.y * 1000.0f + gunZ;
						GAMEPLAY::SHOOT_SINGLE_BULLET_BETWEEN_COORDS(gunX, gunY, gunZ,
							targetX, targetY, targetZ, 4, 1, WEAPON_PISTOL, playerPed, 1, 1, BULLET_SPEED_QUIRK);
						g_fireCooldown = 0.25f;
					}
					else
					{
						g_fireCooldown += -0.1f;
					}
				}
			}
			else if (g_weaponIndex == 2)
			{
				if (buttons.triggerJustPressed)
				{
					float c = cosf(yawDeltaRad);
					float s = sinf(yawDeltaRad);
					for (int pellet = 0; pellet < 5; pellet++)
					{
						XMFLOAT3 dir = GetGunDirection(rightHand->GetMatrix());
						float targetX = (float)(rand() % 4000 - 2000) * 0.02f + (-dir.x * c - dir.z * s) * 1000.0f + gunX;
						float targetY = (float)(rand() % 4000 - 2000) * 0.02f + (dir.z * c - dir.x * s) * 1000.0f + gunY;
						float targetZ = (float)(rand() % 4000 - 2000) * 0.02f + dir.y * 1000.0f + gunZ;
						GAMEPLAY::SHOOT_SINGLE_BULLET_BETWEEN_COORDS(gunX, gunY, gunZ,
							targetX, targetY, targetZ, 15, 1, WEAPON_PISTOL, playerPed, 1, 1, BULLET_SPEED_QUIRK);
					}
				}
			}
			else if (g_weaponIndex == 3)
			{
				if (buttons.triggerJustPressed)
				{
					XMFLOAT3 dir = GetGunDirection(rightHand->GetMatrix());
					float c = cosf(yawDeltaRad);
					float s = sinf(yawDeltaRad);
					rand();
					float targetX = (-dir.x * c - dir.z * s) * 1000.0f + gunX;
					float targetY = (dir.z * c - dir.x * s) * 1000.0f + gunY;
					float targetZ = dir.y * 1000.0f + gunZ;

					Hash rocketHash = GAMEPLAY::GET_HASH_KEY("WEAPON_VEHICLE_ROCKET");
					if (!WEAPON::HAS_WEAPON_ASSET_LOADED(rocketHash))
					{
						WEAPON::REQUEST_WEAPON_ASSET(rocketHash, 31, 0);
						while (!WEAPON::HAS_WEAPON_ASSET_LOADED(rocketHash))
							WAIT(0);
					}
					GAMEPLAY::SHOOT_SINGLE_BULLET_BETWEEN_COORDS(gunX, gunY, gunZ,
						targetX, targetY, targetZ, 10, 1, rocketHash, playerPed, 1, 0, -1.0f);
				}
			}

			if (buttons.menuJustPressed)
			{
				g_weaponIndex = (g_weaponIndex + 1) % 4;
				SpawnWeaponProp(g_weaponIndex);
			}

			// --- teleport arc ---
			if (!PED::IS_PED_IN_ANY_VEHICLE(playerPed, 0))
			{
				if (buttons.padPressed)
				{
					// preview only
					NativeVector3 start = {};
					start.x = gunX;
					start.y = gunY;
					start.z = gunZ;

					XMFLOAT3 gunDir = GetGunDirection(rightHand->GetMatrix());
					NativeVector3 direction = {};
					direction.x = gunDir.x;
					direction.y = gunDir.y;
					direction.z = gunDir.z;

					NativeVector3 headOffset = {};
					headOffset.x = headX;
					headOffset.y = headY;
					headOffset.z = headZ;

					vr::HmdQuad_t area = hmd->GetPlaySpaceSize();
					CastBeam(start, direction, area, headOffset, nullptr, nullptr);
				}
				else if (buttons.padJustReleased)
				{
					NativeVector3 start = {};
					start.x = gunX;
					start.y = gunY;
					start.z = gunZ;

					XMFLOAT3 gunDir = GetGunDirection(rightHand->GetMatrix());
					NativeVector3 direction = {};
					direction.x = gunDir.x;
					direction.y = gunDir.y;
					direction.z = gunDir.z;

					NativeVector3 headOffset = {};
					headOffset.x = headX;
					headOffset.y = headY;
					headOffset.z = headZ;

					vr::HmdQuad_t area = hmd->GetPlaySpaceSize();

					// The alpha reused one stack buffer for the play-area quad
					// and the beam hit, so on a vehicle hit (where CastBeam
					// leaves outHit untouched) the "hit position" is leftover
					// quad data. Reproduced bit-for-bit.
					NativeVector3 hitPos = {};
					hitPos.x = area.vCorners[0].v[0];
					hitPos.y = area.vCorners[0].v[2];
					hitPos.z = area.vCorners[1].v[1];
					Entity hitVehicle = playerPed;
					if (CastBeam(start, direction, area, headOffset, &hitPos, &hitVehicle))
					{
						g_origin = hitPos;
						ENTITY::SET_ENTITY_COORDS(playerPed, hitPos.x, hitPos.y, hitPos.z, 0, 0, 0, 0);
					}

					// aim at an occupied vehicle: pull the driver out sideways
					// and take the seat
					if (ENTITY::IS_ENTITY_A_VEHICLE(hitVehicle))
					{
						Ped driver = VEHICLE::GET_PED_IN_VEHICLE_SEAT(hitVehicle, -1);
						if (driver != 0)
						{
							NativeVector3 driverPos = ENTITY::GET_ENTITY_COORDS(driver, 0);
							NativeVector3 forward = ENTITY::GET_ENTITY_FORWARD_VECTOR(driver);
							ENTITY::SET_ENTITY_COORDS(driver,
								driverPos.x - forward.y * 0.5f,
								driverPos.y + forward.x * 0.5f,
								driverPos.z, 0, 0, 0, 0);
						}
						PED::SET_PED_INTO_VEHICLE(playerPed, hitVehicle, -1);
					}
				}
			}
			else if (buttons.padJustReleased)
			{
				// bail out of the vehicle sideways
				NativeVector3 pos = ENTITY::GET_ENTITY_COORDS(playerPed, 0);
				NativeVector3 forward = ENTITY::GET_ENTITY_FORWARD_VECTOR(playerPed);
				ENTITY::SET_ENTITY_COORDS(playerPed,
					pos.x - forward.y * 0.5f,
					pos.y + forward.x * 0.5f,
					pos.z, 0, 0, 0, 0);
			}
		}

		// --- hide the player body (alpha trick differs per situation) ---
		if (!PED::IS_PED_IN_ANY_VEHICLE(playerPed, 0))
		{
			ENTITY::SET_ENTITY_VISIBLE(playerPed, 0, 0);
			ENTITY::SET_ENTITY_ALPHA(playerPed, 255, 0);
		}
		else
		{
			ENTITY::SET_ENTITY_VISIBLE(playerPed, 1, 0);
			ENTITY::SET_ENTITY_ALPHA(playerPed, 0, 0);
		}

		WAIT(0);
	}
}

// VR runs back to back; each returns when VR is switched off in game (VROff key or the menu)
void ScriptMain()
{
	for (;;)
		RunVrMode();
}
