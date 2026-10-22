#pragma once
// ---------------------------------------------------------------------------
// RoomScale - amplified walking on the floor plane.
//
// PROBLEM: with the game camera driven 1:1 from the headset, ten real steps
// move the camera ten real steps' worth - which in a GTA-scale world barely
// registers. This amplifies HORIZONTAL head travel (the X/Z floor plane) so a
// small real walk covers a useful in-game distance.
//
// The amplification RULE is selectable (Mode=), not hard-wired. Settings live
// in the [RoomScale] section of gtaovr\stabilizer.ini, which is re-read once a
// second, so edits apply while the game runs.
//
//   Mode 0  LINEAR       travel from the anchor x PlaneGain. Simplest, and
//                        amplifies EVERYTHING - including head sway and
//                        tracking wobble, by the same factor.
//   Mode 1  WALK         (default) only motion faster than a walking pace is
//                        amplified. Standing still, leaning and swaying stay
//                        1:1; actual steps get PlaneGain. Fades in between
//                        WalkSpeedMin and WalkSpeedFull so there is no seam.
//                        This exists because Linear turns 1 cm of tracking
//                        noise into 4 cm of camera shake at 4x.
//   Mode 2  PROGRESSIVE  inside InnerRadius of the anchor, 1:1 (lean and look
//                        around naturally); beyond it, extra distance x gain.
//                        Position-based rather than speed-based.
//
// Walk mode is an INTEGRATOR: it accumulates amplified steps, so after a walk
// out and back at different speeds you will not necessarily be exactly where
// you started in-game. That is inherent to speed-gating, not drift. The
// re-anchor hotkey puts you back on the spot.
//
// DESIGN DECISIONS:
//  * Plane only; vertical has its own gain, default 1.0.
//  * Relative to an ANCHOR, never the tracker origin - no jump on enable.
//    Re-anchored on enable, mode/gain change, hotkey, and PoseGuard resets.
//  * Applied LAST, after PoseGuard and the stabilizer, which are tuned in real
//    metres - amplify first and ordinary steps would trip the hop detector.
//  * Applied to the HMD matrix before inversion: the single source for both the
//    render view and GetHeadPosition(), so render and game camera agree.
//
// Hotkey: [Hotkeys] ReanchorKey in the same file (default Ctrl+Shift+Home)
// re-anchors. It is re-read with the other settings, so a new bind applies
// within a second.
// ---------------------------------------------------------------------------

#include <Windows.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "../Log.hpp"
#include "Hotkey.hpp"

namespace RoomScale {

  enum Mode { MODE_LINEAR = 0, MODE_WALK = 1, MODE_PROGRESSIVE = 2, MODE_COUNT = 3 };
  static const char* const kModeNames[MODE_COUNT] = { "Linear", "Walk", "Progressive" };

  struct State {
    // ---- config (from ini) ----
    bool   enabled       = false;
    int    mode          = MODE_WALK;
    float  planeGain     = 4.0f;
    float  verticalGain  = 1.0f;
    float  maxRadius     = 40.0f;  // metres of AMPLIFIED plane travel from anchor
    float  walkSpeedMin  = 0.15f;  // m/s: below this, 1:1 (sway, leaning)
    float  walkSpeedFull = 0.50f;  // m/s: at/above this, full gain (walking)
    float  innerRadius   = 0.50f;  // m: Progressive's 1:1 zone

    // ---- runtime ----
    bool   anchored = false;
    float  ax = 0.0f, ay = 0.0f, az = 0.0f;
    bool   reanchorRequested = false;

    // Walk-mode integrator
    float  prevX = 0.0f, prevZ = 0.0f;
    float  accX = 0.0f, accZ = 0.0f;   // amplified plane offset from anchor
    float  speedEma = 0.0f;
    LARGE_INTEGER lastQpc = { 0 };

    ULONGLONG lastRead = 0;
    OVRInject::HotkeyBind reanchorKey = { VK_HOME, true, true, false };
    bool   keyWasDown = false;
    bool   loaded     = false;
  };

  inline State& S() { static State s; return s; }

  inline void RequestReanchor() { S().reanchorRequested = true; }

  inline void IniPath(char* out, size_t n) {
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&IniPath, &self);
    char mod[MAX_PATH] = { 0 };
    GetModuleFileNameA(self, mod, MAX_PATH);
    char* slash = strrchr(mod, '\\');
    if (slash) *(slash + 1) = '\0';
    _snprintf_s(out, n, _TRUNCATE, "%sgtaovr\\stabilizer.ini", mod);
  }

  inline float ReadF(const char* ini, const char* key, float def) {
    char buf[64] = { 0 }, defs[32];
    _snprintf_s(defs, sizeof(defs), _TRUNCATE, "%g", def);
    GetPrivateProfileStringA("RoomScale", key, defs, buf, sizeof(buf), ini);
    return (float)atof(buf);
  }

  inline float Clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

  inline void EnsureKey(const char* ini, const char* key, const char* def) {
    char probe[32] = { 0 };
    GetPrivateProfileStringA("RoomScale", key, "", probe, sizeof(probe), ini);
    if (probe[0] == '\0')
      WritePrivateProfileStringA("RoomScale", key, def, ini);
  }

  // Writes any MISSING key with its default, so every setting is visible in
  // the file (and an older ini picks up keys added later) without ever
  // overwriting a value the user has set.
  inline void EnsureDefaults(const char* ini) {
    EnsureKey(ini, "Enabled",       "1");
    EnsureKey(ini, "Mode",          "1");
    EnsureKey(ini, "PlaneGain",     "4.0");
    EnsureKey(ini, "VerticalGain",  "1.0");
    EnsureKey(ini, "MaxRadius",     "40.0");
    EnsureKey(ini, "WalkSpeedMin",  "0.15");
    EnsureKey(ini, "WalkSpeedFull", "0.50");
    EnsureKey(ini, "InnerRadius",   "0.50");
  }

  inline void Reload() {
    State& s = S();
    ULONGLONG now = GetTickCount64();
    if (s.lastRead && now - s.lastRead < 1000) return;
    s.lastRead = now;

    char ini[MAX_PATH];
    IniPath(ini, sizeof(ini));
    EnsureDefaults(ini);

    bool  en  = ReadF(ini, "Enabled", 1.0f) != 0.0f;
    int   md  = (int)ReadF(ini, "Mode", 1.0f);
    float pg  = Clampf(ReadF(ini, "PlaneGain",     4.0f),  0.1f, 20.0f);
    float vg  = Clampf(ReadF(ini, "VerticalGain",  1.0f),  0.1f, 5.0f);
    float mr  = Clampf(ReadF(ini, "MaxRadius",     40.0f), 1.0f, 500.0f);
    float wmn = Clampf(ReadF(ini, "WalkSpeedMin",  0.15f), 0.0f, 3.0f);
    float wfl = Clampf(ReadF(ini, "WalkSpeedFull", 0.50f), 0.0f, 5.0f);
    float ir  = Clampf(ReadF(ini, "InnerRadius",   0.50f), 0.0f, 5.0f);
    if (md < 0 || md >= MODE_COUNT) md = MODE_WALK;
    if (wfl < wmn + 0.01f) wfl = wmn + 0.01f;   // keep the fade band non-empty

    // Re-anchor bind; None or empty unbinds it, an unreadable value falls back to the default
    char keyText[64] = { 0 };
    GetPrivateProfileStringA("Hotkeys", "ReanchorKey", "Ctrl+Shift+Home", keyText, sizeof(keyText), ini);
    OVRInject::HotkeyBind key = { VK_HOME, true, true, false };
    if (!OVRInject::HotkeyParse(keyText, key))
      LOGSTRF("RoomScale: could not parse ReanchorKey '%s', using Ctrl+Shift+Home\n", keyText);
    if (!OVRInject::HotkeyEqual(key, s.reanchorKey)) {
      char shown[64];
      OVRInject::HotkeyFormat(key, shown, sizeof(shown));
      if (s.loaded)
        LOGSTRF("RoomScale: re-anchor key is now %s\n", shown);
      s.reanchorKey = key;
      s.keyWasDown = true;   // a key already held when the bind changes does not fire
    }

    // Anything that would RESCALE existing travel in one frame re-anchors
    // instead, so no change made while playing can cause a jump.
    bool rescale = en != s.enabled || md != s.mode ||
                   pg != s.planeGain || vg != s.verticalGain || ir != s.innerRadius;
    bool changed = rescale || mr != s.maxRadius ||
                   wmn != s.walkSpeedMin || wfl != s.walkSpeedFull;

    if (en && rescale) s.reanchorRequested = true;

    s.enabled = en; s.mode = md; s.planeGain = pg; s.verticalGain = vg;
    s.maxRadius = mr; s.walkSpeedMin = wmn; s.walkSpeedFull = wfl;
    s.innerRadius = ir;

    if (changed || !s.loaded)
      LOGSTRF("RoomScale: %s mode=%s PlaneGain=%.2f VerticalGain=%.2f MaxRadius=%.1f WalkSpeed=%.2f..%.2f m/s InnerRadius=%.2f m\n",
              en ? "ON " : "OFF", kModeNames[md], pg, vg, mr, wmn, wfl, ir);
    s.loaded = true;
  }

  inline void ClampDisc(float& x, float& z, float maxR) {
    float r = sqrtf(x * x + z * z);
    if (r <= maxR) return;
    float k = maxR / r;
    x *= k; z *= k;
    static ULONGLONG lastWarn = 0;
    ULONGLONG now = GetTickCount64();
    if (now - lastWarn > 3000) {
      lastWarn = now;
      LOGSTRF("RoomScale: amplified travel %.1f m hit MaxRadius %.1f m - clamped (re-anchor to reset)\n", r, maxR);
    }
  }

  // m is the HMD pose matrix (tracking space, NOT yet inverted into a view).
  // Translation is m[12], m[13], m[14]; OpenVR is Y-up, so the floor is X/Z.
  template <typename M>
  inline void Apply(M& m) {
    Reload();
    State& s = S();

    bool down = !OVRInject::HotkeyIsNone(s.reanchorKey) && (GetAsyncKeyState(s.reanchorKey.vk) & 0x8000) != 0;
    if (down && !s.keyWasDown && OVRInject::HotkeyModifiersMatch(s.reanchorKey)) {
      s.reanchorRequested = true;
      LOGSTRF("RoomScale: re-anchor requested by hotkey\n");
    }
    s.keyWasDown = down;

    if (!s.enabled) { s.anchored = false; return; }

    float x = m[12], y = m[13], z = m[14];
    if (!(x == x && y == y && z == z)) return;   // NaN: PoseGuard's problem

    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);

    if (!s.anchored || s.reanchorRequested) {
      s.ax = x; s.ay = y; s.az = z;
      s.prevX = x; s.prevZ = z;
      s.accX = s.accZ = 0.0f;
      s.speedEma = 0.0f;
      s.lastQpc = now;
      s.anchored = true;
      s.reanchorRequested = false;
      LOGSTRF("RoomScale: anchored at (%.2f, %.2f, %.2f), mode=%s\n", x, y, z, kModeNames[s.mode]);
      return;   // this frame: zero travel, so no jump
    }

    float ox = 0.0f, oz = 0.0f;

    switch (s.mode) {
      case MODE_LINEAR: {
        ox = (x - s.ax) * s.planeGain;
        oz = (z - s.az) * s.planeGain;
        ClampDisc(ox, oz, s.maxRadius);
        break;
      }

      case MODE_WALK: {
        // Per-frame delta, gain chosen by how FAST the head is travelling.
        // QPC, not GetTickCount64: the tick counter has ~16 ms resolution, so
        // at 60-90 fps dt would read 0 or 16 and the speed would be garbage.
        float dt = (float)(now.QuadPart - s.lastQpc.QuadPart) / (float)freq.QuadPart;
        s.lastQpc = now;
        float dx = x - s.prevX, dz = z - s.prevZ;
        s.prevX = x; s.prevZ = z;

        if (dt > 0.0001f && dt < 0.25f) {
          // Smooth the speed estimate (~100 ms) so single-frame tracking
          // noise cannot flicker the gain on and off.
          float inst = sqrtf(dx * dx + dz * dz) / dt;
          float a = dt / (dt + 0.10f);
          s.speedEma += (inst - s.speedEma) * a;
        }
        // (dt out of range = a stall/hitch: keep the old speed, still apply
        //  the delta so position never desyncs from tracking.)

        float t = Clampf((s.speedEma - s.walkSpeedMin) /
                         (s.walkSpeedFull - s.walkSpeedMin), 0.0f, 1.0f);
        t = t * t * (3.0f - 2.0f * t);                   // smoothstep
        float g = 1.0f + (s.planeGain - 1.0f) * t;

        s.accX += dx * g;
        s.accZ += dz * g;
        ClampDisc(s.accX, s.accZ, s.maxRadius);          // clamp the STATE, so it cannot pile up
        ox = s.accX; oz = s.accZ;
        break;
      }

      case MODE_PROGRESSIVE: {
        float rx = x - s.ax, rz = z - s.az;
        float r = sqrtf(rx * rx + rz * rz);
        if (r > s.innerRadius && r > 0.0001f) {
          float r2 = s.innerRadius + (r - s.innerRadius) * s.planeGain;
          float k = r2 / r;
          ox = rx * k; oz = rz * k;
        } else {
          ox = rx; oz = rz;
        }
        ClampDisc(ox, oz, s.maxRadius);
        break;
      }
    }

    m[12] = s.ax + ox;
    m[13] = s.ay + (y - s.ay) * s.verticalGain;
    m[14] = s.az + oz;
  }

} // namespace RoomScale
