#pragma once

// Appends a printf-style line to gtavrInjectLog.txt in the game directory
// (shared with OVRInject, which logs to the same file).
void Log(const char* format, ...);

// Timestamped diagnostic line to gtavive_trace.txt in the game directory.
void Trace(const char* format, ...);
