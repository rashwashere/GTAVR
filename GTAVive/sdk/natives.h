#pragma once

// Natives used by GTAVive, with the original hashes recovered from the alpha
// binary (ScriptHookV maps them to the running game version). Only the natives the mod actually calls are declared.
// Names follow the ScriptHookV SDK natives.h conventions of the time.

#include "main.h"

namespace NativeInvoke
{
	template <typename T>
	static inline void Push(T val)
	{
		static_assert(sizeof(T) <= sizeof(unsigned __int64), "native argument too large");
		unsigned __int64 val64 = 0;
		*reinterpret_cast<T*>(&val64) = val;
		nativePush64(val64);
	}

	template <typename R, typename... Args>
	static inline R Invoke(unsigned __int64 hash, Args... args)
	{
		nativeInit(hash);
		(Push(args), ...);
		return *reinterpret_cast<R*>(nativeCall());
	}
}

namespace PLAYER
{
	static Ped GET_PLAYER_PED(Player player) { return NativeInvoke::Invoke<Ped>(0x43A66C31C68491C0, player); }
}

namespace ENTITY
{
	static NativeVector3 GET_ENTITY_COORDS(Entity entity, BOOL alive) { return NativeInvoke::Invoke<NativeVector3>(0x3FEF770D40960D5A, entity, alive); }
	static BOOL DOES_ENTITY_EXIST(Entity entity) { return NativeInvoke::Invoke<BOOL>(0x7239B21A38F536BA, entity); } // not in the alpha
	static NativeVector3 GET_ENTITY_ROTATION(Entity entity, int rotationOrder) { return NativeInvoke::Invoke<NativeVector3>(0xAFBD61CC738D9EB9, entity, rotationOrder); }
	static NativeVector3 GET_ENTITY_VELOCITY(Entity entity) { return NativeInvoke::Invoke<NativeVector3>(0x4805D2B1D8CF94A9, entity); }
	static void SET_ENTITY_VELOCITY(Entity entity, float x, float y, float z) { NativeInvoke::Invoke<Void>(0x1C99BB7B6E96D16F, entity, x, y, z); }
	static float GET_ENTITY_HEADING(Entity entity) { return NativeInvoke::Invoke<float>(0xE83D4F9BA2A38914, entity); } // not in the alpha
	static NativeVector3 GET_ENTITY_FORWARD_VECTOR(Entity entity) { return NativeInvoke::Invoke<NativeVector3>(0x0A794A5A57F8DF91, entity); }
	static int GET_ENTITY_TYPE(Entity entity) { return NativeInvoke::Invoke<int>(0x8ACD366038D14505, entity); }
	static BOOL IS_ENTITY_A_VEHICLE(Entity entity) { return NativeInvoke::Invoke<BOOL>(0x6AC7003FA6E5575E, entity); }
	static void SET_ENTITY_COORDS(Entity entity, float x, float y, float z, BOOL xAxis, BOOL yAxis, BOOL zAxis, BOOL clearArea) { NativeInvoke::Invoke<Void>(0x06843DA7060A026B, entity, x, y, z, xAxis, yAxis, zAxis, clearArea); }
	static void SET_ENTITY_ROTATION(Entity entity, float pitch, float roll, float yaw, int rotationOrder, BOOL flag) { NativeInvoke::Invoke<Void>(0x8524A8B0171D5E07, entity, pitch, roll, yaw, rotationOrder, flag); }
	static void SET_ENTITY_VISIBLE(Entity entity, BOOL toggle, BOOL flag) { NativeInvoke::Invoke<Void>(0xEA1C610A04DB6BBB, entity, toggle, flag); }
	static void SET_ENTITY_ALPHA(Entity entity, int alphaLevel, BOOL skin) { NativeInvoke::Invoke<Void>(0x44A0870B7E92D7C0, entity, alphaLevel, skin); }
	static void RESET_ENTITY_ALPHA(Entity entity) { NativeInvoke::Invoke<Void>(0x9B1E824FFBB7027A, entity); } // not in the alpha (VR off)
	static void DETACH_ENTITY(Entity entity, BOOL dynamic, BOOL collision) { NativeInvoke::Invoke<Void>(0x961AC54BF0613F5D, entity, dynamic, collision); }
	static void ATTACH_ENTITY_TO_ENTITY(Entity entity1, Entity entity2, int boneIndex, float xPos, float yPos, float zPos, float xRot, float yRot, float zRot, BOOL softPinning, BOOL collision, BOOL isPed, BOOL vertexIndex, int fixedRot, BOOL p14) { NativeInvoke::Invoke<Void>(0x6B9BBD38AB0796DF, entity1, entity2, boneIndex, xPos, yPos, zPos, xRot, yRot, zRot, softPinning, collision, isPed, vertexIndex, fixedRot, p14); }
	static void APPLY_FORCE_TO_ENTITY(Entity entity, int forceType, float x, float y, float z, float xRot, float yRot, float zRot, int boneIndex, BOOL isDirectionRel, BOOL ignoreUpVec, BOOL isForceRel, BOOL p12, BOOL p13) { NativeInvoke::Invoke<Void>(0xC5F68BE9613E2D18, entity, forceType, x, y, z, xRot, yRot, zRot, boneIndex, isDirectionRel, ignoreUpVec, isForceRel, p12, p13); }
	static void CLEAR_ENTITY_LAST_DAMAGE_ENTITY(Entity entity) { NativeInvoke::Invoke<Void>(0xA72CD9CA74A5ECBA, entity); }
}

namespace PED
{
	static BOOL IS_PED_RAGDOLL(Ped ped) { return NativeInvoke::Invoke<BOOL>(0x47E4E977581C5B55, ped); } // not in the alpha
	static BOOL IS_PED_FALLING(Ped ped) { return NativeInvoke::Invoke<BOOL>(0xFB92A102F1C4DFA3, ped); } // not in the alpha
	static BOOL IS_PED_IN_ANY_VEHICLE(Ped ped, BOOL atGetIn) { return NativeInvoke::Invoke<BOOL>(0x997ABD671D25CA0B, ped, atGetIn); }
	static Vehicle GET_VEHICLE_PED_IS_USING(Ped ped) { return NativeInvoke::Invoke<Vehicle>(0x6094AD011A2EA87D, ped); }
	static NativeVector3 GET_PED_BONE_COORDS(Ped ped, int boneId, float offsetX, float offsetY, float offsetZ) { return NativeInvoke::Invoke<NativeVector3>(0x17C07FC640E86B4E, ped, boneId, offsetX, offsetY, offsetZ); }
	static void APPLY_DAMAGE_TO_PED(Ped ped, int damageAmount, BOOL p2) { NativeInvoke::Invoke<Void>(0x697157CED63F18D4, ped, damageAmount, p2); }
	static void SET_PED_CAN_RAGDOLL(Ped ped, BOOL toggle) { NativeInvoke::Invoke<Void>(0xB128377056A54E2A, ped, toggle); }
	static BOOL SET_PED_TO_RAGDOLL(Ped ped, int time1, int time2, int ragdollType, BOOL p4, BOOL p5, BOOL p6) { return NativeInvoke::Invoke<BOOL>(0xAE99FB955581844A, ped, time1, time2, ragdollType, p4, p5, p6); }
	static void SET_PED_INTO_VEHICLE(Ped ped, Vehicle vehicle, int seatIndex) { NativeInvoke::Invoke<Void>(0xF75B0D629E1C063D, ped, vehicle, seatIndex); }
}

namespace VEHICLE
{
	static void SET_VEHICLE_STEER_BIAS(Vehicle vehicle, float value) { NativeInvoke::Invoke<Void>(0x42A8EC77D5150CBE, vehicle, value); }
	static Ped GET_PED_IN_VEHICLE_SEAT(Vehicle vehicle, int index) { return NativeInvoke::Invoke<Ped>(0xBB40DD2270B65366, vehicle, index); }
}

namespace OBJECT
{
	static Object CREATE_OBJECT(Object modelHash, float x, float y, float z, BOOL isNetwork, BOOL thisScriptCheck, BOOL dynamic) { return NativeInvoke::Invoke<Object>(0x509D5878EB39E842, modelHash, x, y, z, isNetwork, thisScriptCheck, dynamic); }
	static void DELETE_OBJECT(Object* object) { NativeInvoke::Invoke<Void>(0x539E0AE3E6634B9F, object); }
}

namespace DLC
{
	// Not in the alpha (Auto-start VR: never arm behind the loading screen)
	static BOOL GET_IS_LOADING_SCREEN_ACTIVE() { return NativeInvoke::Invoke<BOOL>(0x10D0A8F259E93EC9); }
}

namespace CAM
{
	// Not in the alpha (Auto-start VR: wait for the fade-in)
	static BOOL IS_SCREEN_FADED_OUT() { return NativeInvoke::Invoke<BOOL>(0xB16FCE9DDC7BA182); }
	// Not in the alpha (beta movement options):
	static void SET_GAMEPLAY_CAM_RELATIVE_HEADING(float heading) { NativeInvoke::Invoke<Void>(0xB4EC2312F4E5B1F1, heading); }
	static void SET_GAMEPLAY_CAM_RELATIVE_PITCH(float pitch, float scale) { NativeInvoke::Invoke<Void>(0x6D0858B8EDFD2B7D, pitch, scale); }
	static Cam CREATE_CAM_WITH_PARAMS(const char* camName, float posX, float posY, float posZ, float rotX, float rotY, float rotZ, float fov, BOOL p8, int p9) { return NativeInvoke::Invoke<Cam>(0xB51194800B257161, camName, posX, posY, posZ, rotX, rotY, rotZ, fov, p8, p9); }
	static void SET_CAM_COORD(Cam cam, float posX, float posY, float posZ) { NativeInvoke::Invoke<Void>(0x4D41783FB745E42E, cam, posX, posY, posZ); }
	static void SET_CAM_ROT(Cam cam, float rotX, float rotY, float rotZ, int rotationOrder) { NativeInvoke::Invoke<Void>(0x85973643155D0B07, cam, rotX, rotY, rotZ, rotationOrder); }
	static void SET_CAM_ACTIVE(Cam cam, BOOL active) { NativeInvoke::Invoke<Void>(0x026FB97D0A425F84, cam, active); }
	static void SET_CAM_NEAR_CLIP(Cam cam, float nearClip) { NativeInvoke::Invoke<Void>(0xC7848EFCCC545182, cam, nearClip); }
	static void RENDER_SCRIPT_CAMS(BOOL render, BOOL ease, int easeTime, BOOL p3, BOOL p4) { NativeInvoke::Invoke<Void>(0x07E5B515DB0636FC, render, ease, easeTime, p3, p4, 0); } // the 6th arg (p5) is passed as 0
	static void DESTROY_CAM(Cam cam, BOOL bScriptHostCam) { NativeInvoke::Invoke<Void>(0x865908C81A2C22E9, cam, bScriptHostCam); } // not in the alpha (VR off)
	static void DETACH_CAM(Cam cam) { NativeInvoke::Invoke<Void>(0xA2FABBE87F4BAD82, cam); }
	static void ATTACH_CAM_TO_ENTITY(Cam cam, Entity entity, float xOffset, float yOffset, float zOffset, BOOL isRelative) { NativeInvoke::Invoke<Void>(0xFEDB7D269E8C60E3, cam, entity, xOffset, yOffset, zOffset, isRelative); }
}

namespace WEAPON
{
	static Hash GET_WEAPONTYPE_MODEL(Hash weaponHash) { return NativeInvoke::Invoke<Hash>(0xF46CDC33180FDA94, weaponHash); }
	static void REQUEST_WEAPON_ASSET(Hash weaponHash, int p1, int p2) { NativeInvoke::Invoke<Void>(0x5443438F033E29C3, weaponHash, p1, p2); }
	static BOOL HAS_WEAPON_ASSET_LOADED(Hash weaponHash) { return NativeInvoke::Invoke<BOOL>(0x36E353271F0E90EE, weaponHash); }
}

namespace GAMEPLAY
{
	static Hash GET_HASH_KEY(const char* value) { return NativeInvoke::Invoke<Hash>(0xD24D37CC275948CC, value); }
	static float GET_FRAME_TIME() { return NativeInvoke::Invoke<float>(0x15C40837039FFAF7); } // not in the alpha
	static void SHOOT_SINGLE_BULLET_BETWEEN_COORDS(float x1, float y1, float z1, float x2, float y2, float z2, int damage, BOOL perfectAccuracy, Hash weaponHash, Ped ownerPed, BOOL isAudible, BOOL isInvisible, float speed) { NativeInvoke::Invoke<Void>(0x867654CBC7606F2C, x1, y1, z1, x2, y2, z2, damage, perfectAccuracy, weaponHash, ownerPed, isAudible, isInvisible, speed); }
}

namespace CONTROLS
{
	// 0xE8A25867FBA3B05E: _SET_CONTROL_NORMAL / SET_CONTROL_VALUE_NEXT_FRAME
	static void _SET_CONTROL_NORMAL(int inputGroup, int control, float amount) { NativeInvoke::Invoke<Void>(0xE8A25867FBA3B05E, inputGroup, control, amount); }
	// Not in the alpha (beta movement options):
	static void DISABLE_CONTROL_ACTION(int inputGroup, int control, BOOL disable) { NativeInvoke::Invoke<Void>(0xFE99B66D079CF6BC, inputGroup, control, disable); }
	static float GET_DISABLED_CONTROL_NORMAL(int inputGroup, int control) { return NativeInvoke::Invoke<float>(0x11E65974A982637C, inputGroup, control); }
	static BOOL _IS_USING_KEYBOARD(int inputGroup) { return NativeInvoke::Invoke<BOOL>(0xA571D46727E2B718, inputGroup); }
	// Not in the alpha (settings menu):
	static void DISABLE_ALL_CONTROL_ACTIONS(int inputGroup) { NativeInvoke::Invoke<Void>(0x5F4B6931816E599B, inputGroup); }
}

namespace SHAPETEST
{
	// 0x377906D8A31E5586: _START_SHAPE_TEST_LOS_PROBE (synchronous "expensive" variant)
	static int _START_SHAPE_TEST_LOS_PROBE(float x1, float y1, float z1, float x2, float y2, float z2, int flags, Entity ignoreEntity, int p8) { return NativeInvoke::Invoke<int>(0x377906D8A31E5586, x1, y1, z1, x2, y2, z2, flags, ignoreEntity, p8); }
	static int START_SHAPE_TEST_CAPSULE(float x1, float y1, float z1, float x2, float y2, float z2, float radius, int flags, Entity ignoreEntity, int p9) { return NativeInvoke::Invoke<int>(0x28579D1B8F8AAC80, x1, y1, z1, x2, y2, z2, radius, flags, ignoreEntity, p9); }
	static int GET_SHAPE_TEST_RESULT(int rayHandle, BOOL* hit, NativeVector3* endCoords, NativeVector3* surfaceNormal, Entity* entityHit) { return NativeInvoke::Invoke<int>(0x3D87450E15D98694, rayHandle, hit, endCoords, surfaceNormal, entityHit); }
}

namespace UI
{
	// Not in the alpha: feed notification for the "VR isn't ready" warning
	static void _SET_NOTIFICATION_TEXT_ENTRY(const char* type) { NativeInvoke::Invoke<Void>(0x202709F4C58A0424, type); }
	static void ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME(const char* text) { NativeInvoke::Invoke<Void>(0x6C188BE134E074AA, text); }
	static int _DRAW_NOTIFICATION(BOOL blink, BOOL showInBrief) { return NativeInvoke::Invoke<int>(0x2ED7843F8F801023, blink, showInBrief); }
	// Not in the alpha: screen text for the settings menu
	static void SET_TEXT_FONT(int fontType) { NativeInvoke::Invoke<Void>(0x66E0276CC5F6B9DA, fontType); }
	static void SET_TEXT_SCALE(float p0, float size) { NativeInvoke::Invoke<Void>(0x07C837F9A01C34C9, p0, size); }
	static void SET_TEXT_COLOUR(int red, int green, int blue, int alpha) { NativeInvoke::Invoke<Void>(0xBE6B23FFA53FB442, red, green, blue, alpha); }
	static void SET_TEXT_CENTRE(BOOL align) { NativeInvoke::Invoke<Void>(0xC02F4DBFB51D988B, align); }
	static void SET_TEXT_RIGHT_JUSTIFY(BOOL toggle) { NativeInvoke::Invoke<Void>(0x6B3C4650BC8BEE47, toggle); }
	static void SET_TEXT_WRAP(float start, float end) { NativeInvoke::Invoke<Void>(0x63145D9C883A1A70, start, end); }
	static void SET_TEXT_OUTLINE() { NativeInvoke::Invoke<Void>(0x2513DFB0FB8400FE); }
	static void _SET_TEXT_ENTRY(const char* text) { NativeInvoke::Invoke<Void>(0x25FBB336DF1804CB, text); }
	// Later game versions take a third argument (0 here)
	static void _DRAW_TEXT(float x, float y) { NativeInvoke::Invoke<Void>(0xCD015E5BB0D96A57, x, y, 0); }
	static BOOL IS_PAUSE_MENU_ACTIVE() { return NativeInvoke::Invoke<BOOL>(0xB0034A223497FFCB); }
}

namespace GRAPHICS
{
	static void DRAW_LINE(float x1, float y1, float z1, float x2, float y2, float z2, int r, int g, int b, int alpha) { NativeInvoke::Invoke<Void>(0x6B7256074AE34680, x1, y1, z1, x2, y2, z2, r, g, b, alpha); }
	static void DRAW_BOX(float x1, float y1, float z1, float x2, float y2, float z2, int r, int g, int b, int alpha) { NativeInvoke::Invoke<Void>(0xD3A9971CADAC7252, x1, y1, z1, x2, y2, z2, r, g, b, alpha); }
	// Not in the alpha (settings menu). Screen-space rectangle, centre x/y and size in 0..1 of the screen; later
	// game versions take a ninth argument (0 here)
	static void DRAW_RECT(float x, float y, float width, float height, int r, int g, int b, int a) { NativeInvoke::Invoke<Void>(0x3A618A217E5154F0, x, y, width, height, r, g, b, a, 0); }
	static void DRAW_POLY(float x1, float y1, float z1, float x2, float y2, float z2, float x3, float y3, float z3, int r, int g, int b, int alpha) { NativeInvoke::Invoke<Void>(0xAC26716048436851, x1, y1, z1, x2, y2, z2, x3, y3, z3, r, g, b, alpha); }
}
