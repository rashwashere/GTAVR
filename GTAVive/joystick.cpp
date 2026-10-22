#include "joystick.hpp"

static BOOL CALLBACK EnumJoysticksCallback(const DIDEVICEINSTANCEA* instance, VOID* context)
{
	Joystick* joystick = reinterpret_cast<Joystick*>(context);

	if (joystick->enum_counter_ == joystick->index_)
	{
		if (SUCCEEDED(joystick->dinput_->CreateDevice(instance->guidInstance, &joystick->device_, nullptr)))
			return DIENUM_STOP;
	}

	joystick->enum_counter_++;
	return DIENUM_CONTINUE;
}

HRESULT Joystick::Initialize()
{
	HRESULT hr = DirectInput8Create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION,
		IID_IDirectInput8A, reinterpret_cast<VOID**>(&dinput_), nullptr);
	if (FAILED(hr))
		return hr;

	hr = dinput_->EnumDevices(DI8DEVCLASS_GAMECTRL, EnumJoysticksCallback, this, DIEDFL_ATTACHEDONLY);
	if (FAILED(hr))
		return hr;

	if (device_ == nullptr)
		return E_FAIL;

	hr = device_->SetDataFormat(&c_dfDIJoystick2);
	if (FAILED(hr))
		return hr;

	hr = device_->SetCooperativeLevel(nullptr, DISCL_EXCLUSIVE | DISCL_FOREGROUND);
	if (FAILED(hr))
		return hr;

	return S_OK;
}

static BOOL CALLBACK CountJoysticksCallback(const DIDEVICEINSTANCEA* instance, VOID* context)
{
	(*reinterpret_cast<int*>(context))++;
	return DIENUM_CONTINUE;
}

int GetConnectedJoystickCount()
{
	int count = 0;

	IDirectInput8A* dinput = nullptr;
	if (FAILED(DirectInput8Create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION,
		IID_IDirectInput8A, reinterpret_cast<VOID**>(&dinput), nullptr)))
		return 0;

	dinput->EnumDevices(DI8DEVCLASS_GAMECTRL, CountJoysticksCallback, &count, DIEDFL_ATTACHEDONLY);
	return count;
}
