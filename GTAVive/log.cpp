#include "log.hpp"

#include <cstdio>
#include <cstdarg>
#include <Windows.h>

// Timestamped and tagged for the same reason OVRInject's half is: this file
// is append-only and written by BOTH modules, so without a clock a stale run
// reads exactly like a live one, and without a tag you cannot tell which
// module said what. [vive] here, [ovr] there.
void Log(const char* format, ...)
{
	FILE* file = nullptr;
	if (fopen_s(&file, "gtavrInjectLog.txt", "a") != 0 || file == nullptr)
		return;

	static bool banner = false;
	if (!banner)
	{
		banner = true;
		SYSTEMTIME d;
		GetLocalTime(&d);
		fprintf(file, "\n===== GTAVive v2.0 run start %04d-%02d-%02d %02d:%02d:%02d =====\n",
			d.wYear, d.wMonth, d.wDay, d.wHour, d.wMinute, d.wSecond);
	}

	SYSTEMTIME st;
	GetLocalTime(&st);
	fprintf(file, "%02d:%02d:%02d.%03d [vive] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

	va_list args;
	va_start(args, format);
	vfprintf(file, format, args);
	va_end(args);

	fclose(file);
}

// Diagnostic trace (gtavive_trace.txt): timestamped, separate from the
// shared inject log so the two never interleave.
#include <Windows.h>
void Trace(const char* format, ...)
{
	// ~1 MB an hour: on the first line of a run, start over once it passes
	// 20 MB, keeping the previous log as gtavive_trace.old.txt
	static bool checkedSize = false;
	if (!checkedSize)
	{
		checkedSize = true;
		WIN32_FILE_ATTRIBUTE_DATA info;
		if (GetFileAttributesExA("gtavive_trace.txt", GetFileExInfoStandard, &info) &&
			(info.nFileSizeHigh != 0 || info.nFileSizeLow > 20u * 1024u * 1024u))
			MoveFileExA("gtavive_trace.txt", "gtavive_trace.old.txt", MOVEFILE_REPLACE_EXISTING);
	}

	FILE* file = nullptr;
	if (fopen_s(&file, "gtavive_trace.txt", "a") != 0 || file == nullptr)
		return;

	SYSTEMTIME st;
	GetLocalTime(&st);
	fprintf(file, "%02d:%02d:%02d.%03d  ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

	va_list args;
	va_start(args, format);
	vfprintf(file, format, args);
	va_end(args);

	fclose(file);
}
