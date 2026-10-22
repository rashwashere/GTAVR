// Offline test for OVRInject\Vive\RoomScale.hpp.
// Feeds synthetic head paths through RoomScale::Apply and checks each mode.
// The ini is read from <exe dir>\gtaovr\stabilizer.ini, which this test writes.

#include <Windows.h>
#include <cstdio>
#include <cmath>
#include "Vive/RoomScale.hpp"

struct M { float v[16] = {}; float& operator[](int i) { return v[i]; } };

static int g_fail = 0;
#define EXPECT(cond, ...) do { if (!(cond)) { ++g_fail; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char g_ini[MAX_PATH];

static void SetIni(int mode, float gain, float maxR = 40.0f) {
  char b[32];
  WritePrivateProfileStringA("RoomScale", "Enabled", "1", g_ini);
  sprintf_s(b, "%d", mode); WritePrivateProfileStringA("RoomScale", "Mode", b, g_ini);
  sprintf_s(b, "%g", gain); WritePrivateProfileStringA("RoomScale", "PlaneGain", b, g_ini);
  sprintf_s(b, "%g", maxR); WritePrivateProfileStringA("RoomScale", "MaxRadius", b, g_ini);
  WritePrivateProfileStringA("RoomScale", "VerticalGain", "1", g_ini);
  RoomScale::S().lastRead = 0;             // force re-read now
  RoomScale::S().anchored = false;         // fresh anchor per scenario
}

// One frame at ~90 Hz; returns the output position.
static void Step(float x, float y, float z, float& ox, float& oy, float& oz) {
  M m; m[12] = x; m[13] = y; m[14] = z;
  RoomScale::Apply(m);
  ox = m[12]; oy = m[13]; oz = m[14];
  Sleep(11);
}

int main() {
  // ini next to the exe, like OVRInject next to GTA5
  char dir[MAX_PATH]; GetModuleFileNameA(nullptr, dir, MAX_PATH); *(strrchr(dir, '\\') + 1) = 0;
  char sub[MAX_PATH]; sprintf_s(sub, "%sgtaovr", dir); CreateDirectoryA(sub, nullptr);
  sprintf_s(g_ini, "%sgtaovr\\stabilizer.ini", dir); DeleteFileA(g_ini);
  float x, y, z;

  printf("[1] first frame anchors, no jump (all modes)\n");
  for (int mode = 0; mode < 3; ++mode) {
    SetIni(mode, 4.0f);
    Step(1.0f, 1.6f, -0.5f, x, y, z);
    EXPECT(fabsf(x - 1.0f) < 1e-5f && fabsf(z + 0.5f) < 1e-5f, "mode %d moved on the anchor frame (%.3f,%.3f)", mode, x, z);
  }

  printf("[2] LINEAR: 1 m real walk = 4 m at gain 4\n");
  SetIni(0, 4.0f);
  Step(0, 1.6f, 0, x, y, z);
  for (int i = 1; i <= 90; ++i) Step(i / 90.0f, 1.6f, 0, x, y, z);
  EXPECT(fabsf(x - 4.0f) < 0.01f, "linear x=%.3f expected 4.0", x);
  EXPECT(fabsf(y - 1.6f) < 1e-4f, "linear changed height y=%.3f", y);

  printf("[3] WALK: standing sway stays 1:1\n");
  SetIni(1, 4.0f);
  Step(0, 1.6f, 0, x, y, z);
  float maxDev = 0;
  for (int i = 0; i < 180; ++i) {                 // 2 s of 1 cm sway at ~0.5 Hz (~3 cm/s)
    float s = 0.01f * sinf(i * 0.035f);
    Step(s, 1.6f, 0, x, y, z);
    maxDev = fmaxf(maxDev, fabsf(x - s));
  }
  EXPECT(maxDev < 0.002f, "walk amplified slow sway: max deviation %.4f m", maxDev);

  printf("[4] WALK: walking pace (1 m/s) is amplified ~4x\n");
  SetIni(1, 4.0f);
  Step(0, 1.6f, 0, x, y, z);
  for (int i = 1; i <= 90; ++i) Step(i / 90.0f, 1.6f, 0, x, y, z); // ~1 m in ~1 s
  printf("    1 m real at ~1 m/s -> %.2f m in game\n", x);
  EXPECT(x > 3.0f && x < 4.05f, "walk output %.3f outside 3..4 (speed EMA ramps in)", x);

  printf("[5] PROGRESSIVE: inside 0.5 m is 1:1, beyond is x4\n");
  SetIni(2, 4.0f);
  Step(0, 1.6f, 0, x, y, z);
  Step(0.3f, 1.6f, 0, x, y, z);
  EXPECT(fabsf(x - 0.3f) < 1e-4f, "progressive inner zone x=%.3f expected 0.3", x);
  Step(1.0f, 1.6f, 0, x, y, z);
  EXPECT(fabsf(x - (0.5f + 0.5f * 4.0f)) < 1e-3f, "progressive outer x=%.3f expected 2.5", x);

  printf("[6] MaxRadius clamps amplified travel\n");
  SetIni(0, 10.0f, 5.0f);
  Step(0, 1.6f, 0, x, y, z);
  Step(3.0f, 1.6f, 0, x, y, z);                      // 30 m amplified -> clamp 5
  EXPECT(fabsf(x - 5.0f) < 1e-3f, "clamp x=%.3f expected 5.0", x);

  printf("[7] re-anchor request (PoseGuard reset) -> no jump\n");
  SetIni(1, 4.0f);
  Step(0, 1.6f, 0, x, y, z);
  for (int i = 1; i <= 45; ++i) Step(i / 90.0f, 1.6f, 0, x, y, z);
  RoomScale::RequestReanchor();
  Step(0.01f, 1.28f, 0.01f, x, y, z);                // tracking reset lands on origin
  EXPECT(fabsf(x - 0.01f) < 1e-5f && fabsf(y - 1.28f) < 1e-5f, "re-anchor frame moved: (%.3f,%.3f)", x, y);

  printf("[8] NaN input passes through untouched, state not poisoned\n");
  SetIni(1, 4.0f);
  Step(0, 1.6f, 0, x, y, z);
  Step(NAN, 1.6f, 0, x, y, z);
  Step(0.001f, 1.6f, 0, x, y, z);
  EXPECT(x == x && y == y && z == z, "NaN leaked into output");

  printf("[9] disabled = identity\n");
  WritePrivateProfileStringA("RoomScale", "Enabled", "0", g_ini);
  RoomScale::S().lastRead = 0;
  Step(7.0f, 1.6f, 3.0f, x, y, z);
  EXPECT(x == 7.0f && z == 3.0f, "disabled changed pose (%.3f,%.3f)", x, z);

  printf("\n%s (%d failure%s)\n", g_fail ? "FAIL" : "PASS", g_fail, g_fail == 1 ? "" : "s");
  return g_fail ? 1 : 0;
}
