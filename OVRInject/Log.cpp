#include "targetver.h"

#include <cstdio>
#include <stdarg.h>

#include "Log.hpp"

#include <Windows.h>

#pragma warning(push, 0)

// This log is append-only and shared with GTAVive, so lines from earlier runs
// stay in the file. Every line carries a wall-clock time and every run opens
// with a banner, so old lines can be told apart from the current run.
static void LogStamp(FILE* fp)
{
	static bool banner = false;
	if (!banner)
	{
		banner = true;
		SYSTEMTIME d;
		GetLocalTime(&d);
		fprintf(fp, "\n===== OVRInject run start %04d-%02d-%02d %02d:%02d:%02d =====\n",
			d.wYear, d.wMonth, d.wDay, d.wHour, d.wMinute, d.wSecond);
	}

	SYSTEMTIME st;
	GetLocalTime(&st);
	fprintf(fp, "%02d:%02d:%02d.%03d [ovr] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

void LOGSTRF(char* format, ...)
{
	va_list args;
	va_start(args, format);

	FILE* fp = fopen("gtavrInjectLog.txt", "a");
	if (fp != NULL) {
		LogStamp(fp);
		vfprintf(fp, format, args);
		fclose(fp);
	}

	va_end(args);
};

void LOGWNDF(char* format, ...)
{
  char* buf_fmtted = (char*)malloc(strlen(format) * 4 + 1);

  va_list args;
  va_start(args, format);
  vsprintf(buf_fmtted, format, args);

  va_end(args);

  MessageBoxA(0, buf_fmtted, "Error Logged", MB_OK);

  free(buf_fmtted);
};

void LOGFATALF(char* format, ...)
{
  char* buf_fmtted = (char*)malloc(strlen(format) * 4 + 1);

  va_list args;
  va_start(args, format);
  vsprintf(buf_fmtted, format, args);

  va_end(args);

  MessageBoxA(0, buf_fmtted, "Fatal Error Logged", MB_OK);

  free(buf_fmtted);

  exit(1);
};

void LOGOUTF(char* format, ...) {
  char buf_fmtted[4096];

  va_list args;
  va_start(args, format);
  vsprintf(buf_fmtted, format, args);

  va_end(args);

  OutputDebugStringA(buf_fmtted);
};

#pragma warning(pop)