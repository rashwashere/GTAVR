#pragma once

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")

// Wraps one attached DirectInput game controller, selected by enumeration
// index. Matches the 0x18-byte object layout recovered from the alpha:
// { int index; int enumCounter; IDirectInput8A*; IDirectInputDevice8A*; }
class Joystick
{
public:
	Joystick(int index)
		: index_(index), enum_counter_(0), dinput_(nullptr), device_(nullptr)
	{
	}

	// Creates the DirectInput device for the index_-th attached game
	// controller and configures it (c_dfDIJoystick2, exclusive foreground).
	HRESULT Initialize();

	int index_;
	int enum_counter_;
	IDirectInput8A* dinput_;
	IDirectInputDevice8A* device_;
};

// Counts attached game controllers.
int GetConnectedJoystickCount();
