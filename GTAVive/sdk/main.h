#pragma once

// Clean-room ScriptHookV SDK interface for GTAVive.
// Declarations match the ScriptHookV.dll exports the original GTAVive.asi
// (May 2016) imports; signatures are dictated by the mangled import names:
//   ?scriptWait@@YAXK@Z
//   ?scriptRegister@@YAXPEAUHINSTANCE__@@P6AXXZ@Z
//   ?scriptUnregister@@YAXPEAUHINSTANCE__@@@Z
//   ?nativeInit@@YAX_K@Z
//   ?nativePush64@@YAX_K@Z
//   ?nativeCall@@YAPEA_KXZ
//   ?keyboardHandlerRegister@@YAXP6AXKGEHHHH@Z@Z
//   ?keyboardHandlerUnregister@@YAXP6AXKGEHHHH@Z@Z

#include <windows.h>

#define SHV_IMPORT __declspec(dllimport)

SHV_IMPORT void scriptWait(DWORD time);
SHV_IMPORT void scriptRegister(HINSTANCE module, void(*scriptMain)());
SHV_IMPORT void scriptUnregister(HINSTANCE module);

SHV_IMPORT void nativeInit(unsigned __int64 hash);
SHV_IMPORT void nativePush64(unsigned __int64 val);
SHV_IMPORT unsigned __int64* nativeCall();

typedef void(*TKeyboardFn)(DWORD key, WORD repeats, BYTE scanCode, BOOL isExtended, BOOL isWithAlt, BOOL wasDownBefore, BOOL isUpNow);
SHV_IMPORT void keyboardHandlerRegister(TKeyboardFn function);
SHV_IMPORT void keyboardHandlerUnregister(TKeyboardFn function);

#define WAIT scriptWait

// --- script types ---

typedef int Void;
typedef int Any;
typedef unsigned int uint;
typedef unsigned int Hash;
typedef int Entity;
typedef int Player;
typedef int FireId;
typedef int Ped;
typedef int Vehicle;
typedef int Cam;
typedef int CarGenerator;
typedef int Group;
typedef int Train;
typedef int Pickup;
typedef int Object;
typedef int Weapon;
typedef int Interior;
typedef int Blip;
typedef int Texture;
typedef int TextureDict;
typedef int CoverPoint;
typedef int Camera;
typedef int TaskSequence;
typedef int ColourIndex;
typedef int Sphere;
typedef int ScrHandle;

// Native NativeVector3 return type: each component occupies an 8-byte slot.
struct NativeVector3
{
	float x;
	DWORD _paddingx;
	float y;
	DWORD _paddingy;
	float z;
	DWORD _paddingz;
};
