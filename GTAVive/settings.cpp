#include "settings.hpp"

#include <cstdio>
#include <cstring>
#include <cmath>

static unsigned g_generation = 1;

const char* GtaviveIniPath()
{
	static char path[MAX_PATH] = "";
	GetFullPathNameA("gtaovr\\gtavive.ini", MAX_PATH, path, nullptr);
	return path;
}

const char* StabilizerIniPath()
{
	static char path[MAX_PATH * 2] = "";
	if (path[0] == '\0')
	{
		char module[MAX_PATH] = "";
		HMODULE ovr = GetModuleHandleA("OVRInject.dll");
		if (ovr != NULL && GetModuleFileNameA(ovr, module, MAX_PATH) != 0)
		{
			char* slash = strrchr(module, '\\');
			if (slash != nullptr)
				*(slash + 1) = '\0';
			_snprintf_s(path, sizeof(path), _TRUNCATE, "%sgtaovr\\stabilizer.ini", module);
		}
		else
			GetFullPathNameA("gtaovr\\stabilizer.ini", MAX_PATH, path, nullptr);
	}
	return path;
}

float ReadIniDecimal(const char* section, const char* key, float def, float lo, float hi, const char* path)
{
	char buf[32] = "";
	GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), path);
	const char* p = buf;
	while (*p == ' ' || *p == '\t') ++p;
	bool neg = false;
	if (*p == '-' || *p == '+') neg = (*p++ == '-');
	double v = 0.0, scale = 1.0;
	bool digits = false, frac = false;
	for (; *p; ++p)
	{
		if (*p >= '0' && *p <= '9') { digits = true; if (frac) { scale *= 0.1; v += (*p - '0') * scale; } else v = v * 10.0 + (*p - '0'); }
		else if ((*p == '.' || *p == ',') && !frac) frac = true;
		else break;
	}
	if (!digits) v = def; else if (neg) v = -v;
	return (float)(v < lo ? lo : v > hi ? hi : v);
}

void FormatDecimal(char* out, size_t outSize, float v, int decimals)
{
	if (decimals < 0) decimals = 0;
	if (decimals > 4) decimals = 4;
	long long scale = 1;
	for (int i = 0; i < decimals; ++i) scale *= 10;
	long long iv = llround((double)v * (double)scale);
	bool neg = iv < 0;
	if (neg) iv = -iv;
	long long whole = iv / scale, frac = iv % scale;
	int places = decimals;
	while (places > 0 && frac % 10 == 0)
	{
		frac /= 10;
		--places;
	}
	if (places > 0)
		_snprintf_s(out, outSize, _TRUNCATE, "%s%lld.%0*lld", neg ? "-" : "", whole, places, frac);
	else
		_snprintf_s(out, outSize, _TRUNCATE, "%s%lld", neg ? "-" : "", whole);
}

void WriteIniString(const char* section, const char* key, const char* value, const char* path)
{
	// The gtaovr folder may not exist yet on a fresh install; the ini call does not create folders
	char dir[MAX_PATH * 2];
	strncpy_s(dir, path, _TRUNCATE);
	char* slash = strrchr(dir, '\\');
	if (slash != nullptr)
	{
		*slash = '\0';
		CreateDirectoryA(dir, nullptr);
	}
	WritePrivateProfileStringA(section, key, value, path);
	++g_generation;
}

void WriteIniDecimal(const char* section, const char* key, float v, int decimals, const char* path)
{
	char buf[32];
	FormatDecimal(buf, sizeof(buf), v, decimals);
	WriteIniString(section, key, buf, path);
}

unsigned SettingsGeneration()
{
	return g_generation;
}
