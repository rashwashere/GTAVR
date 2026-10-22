#pragma once

// Ini file access shared by the gameplay code, the key binds and the settings menu.
// Decimals are read and written by hand, so the process locale never changes the format ("1.25", never "1,25").

#include <windows.h>

// gtaovr\gtavive.ini (relative to the game directory).
const char* GtaviveIniPath();

// gtaovr\stabilizer.ini next to OVRInject.dll, the file OVRInject reads and auto-reloads.
const char* StabilizerIniPath();

// A decimal value ("-12.5", "0,8" is accepted too); def when missing or unreadable, then clamped to [lo, hi].
float ReadIniDecimal(const char* section, const char* key, float def, float lo, float hi, const char* path);

// Formats v with at most `decimals` places, trailing zeros removed ("4", "0.25", "-12.5").
void FormatDecimal(char* out, size_t outSize, float v, int decimals);

// Writes one value and marks the settings as changed (see SettingsGeneration).
void WriteIniString(const char* section, const char* key, const char* value, const char* path);
void WriteIniDecimal(const char* section, const char* key, float v, int decimals, const char* path);

// Increments on every write through the functions above; readers that cache ini values re-read when it changes,
// so a change made in the menu applies on the next tick instead of after the once-a-second poll.
unsigned SettingsGeneration();
