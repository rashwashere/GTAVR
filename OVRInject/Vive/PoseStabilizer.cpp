#include "targetver.h"

#include "PoseStabilizer.hpp"

#include "Log.hpp"
#include "Overlay.hpp"

#include <Windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <thread>
#include <string>

using namespace OVRInject;

PoseStabilizer OVRInject::g_pose_stabilizer;

static const char* INI_RELATIVE = "gtaovr\\stabilizer.ini";

static const float PI_F = 3.14159265358979f;

// Hard cap on smoothing so the camera curve stays usable
static const float MAX_SMOOTH_MS_LIMIT = 600.0f;

// Resolves the ini path next to OVRInject.dll (the game root)
static void GetIniPath(char* out, size_t out_size) {
  HMODULE self = NULL;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                     GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     (LPCSTR)&GetIniPath, &self);

  char module_path[MAX_PATH] = { 0 };
  GetModuleFileNameA(self, module_path, MAX_PATH);

  char* last_slash = strrchr(module_path, '\\');
  if (last_slash != nullptr)
    *(last_slash + 1) = '\0';

  _snprintf_s(out, out_size, _TRUNCATE, "%s%s", module_path, INI_RELATIVE);
}

static const char* DEFAULT_INI =
"; GTA: Vive camera stabilizer configuration\n"
"; Toggle ingame with ToggleKey (default Ctrl+Shift+F8).\n"
"; Reload this file ingame with ReloadKey (default Ctrl+Shift+F9).\n"
"\n"
"[Stabilization]\n"
"Enabled=1\n"
"; Preset:\n"
";   Jitter   = tuned default. Removes IMU jitter evenly on all axes.\n"
";   Manual   = per-axis control, see the [Manual] section below.\n"
";   Adaptive = smooth when still, responsive on fast moves ([Adaptive]).\n"
"Preset=Jitter\n"
"; Split switches (1/0): smooth the physical movement and/or the looking-around.\n"
"; StabilizeRotation=0 keeps the raw hand-held turns while movement is stabilized.\n"
"StabilizePosition=1\n"
"StabilizeRotation=1\n"
"\n"
"[Jitter]\n"
"; Jitter preset tuning (milliseconds, 0-600; higher = smoother but laggier)\n"
"PosSmoothMs=180\n"
"RotSmoothMs=140\n"
"\n"
"[Manual]\n"
"; Per-axis smoothing strength, 0-100:\n"
";   0   = RAW (no smoothing on that axis)\n"
";   100 = maximum smoothing (MaxSmoothMs worth)\n"
"; Axes follow where the camera FACES, so LeftRight is always your\n"
"; left/right no matter which way you look.\n"
";\n"
"; Example - keep natural left/right handheld sway, kill up/down shake:\n"
";   YawStrength=0  LateralStrength=0  PitchStrength=70  VerticalStrength=80\n"
"\n"
"; Rotation (look shake):\n"
"PitchStrength=35\n"
"; ^ up-down look\n"
"YawStrength=35\n"
"; ^ left-right look\n"
"RollStrength=35\n"
"; ^ tilt\n"
"\n"
"; Position (movement shake):\n"
"VerticalStrength=45\n"
"; ^ up-down bob\n"
"LateralStrength=45\n"
"; ^ left-right sway\n"
"DepthStrength=45\n"
"; ^ forward-back\n"
"\n"
"; How much smoothing 100 means, in milliseconds. Capped at 600 so the\n"
"; camera curve stays usable.\n"
"MaxSmoothMs=400\n"
"\n"
"[Adaptive]\n"
"; Cutoff in Hz (lower = smoother when still); beta adds speed response\n"
"PosMinCutoff=0.8\n"
"PosBeta=0.02\n"
"RotMinCutoff=0.8\n"
"RotBeta=0.02\n"
"\n"
"[Overlay]\n"
"; On-screen display: whenever you toggle/adjust anything, the current\n"
"; values are drawn in the top-left corner of the game image (visible\n"
"; both on the monitor and inside the VR view). 0 disables it - the\n"
"; window title and beeps still work. Off by default.\n"
"Enabled=0\n"
"; How long the message stays up, in seconds\n"
"DurationSec=2.5\n"
"\n"
"[Hotkeys]\n"
"; Format: optional modifiers + key, joined with '+'. Examples:\n"
";   ToggleKey=Ctrl+Shift+F8    ToggleKey=Alt+Home    ToggleKey=K\n"
"; Modifiers: Ctrl, Shift, Alt (any combination, any order).\n"
"; Matching is exact - a bind without modifiers will NOT fire while\n"
"; Ctrl/Shift/Alt are held, so plain-key binds stay safe to use.\n"
"; Keys: A-Z, 0-9, F1-F24, Home, End, Insert, Delete, PageUp, PageDown,\n"
";   Up, Down, Left, Right, Space, Tab, Enter, Esc, Pause, Numpad0-Numpad9,\n"
";   NumpadAdd, NumpadSub, NumpadMul, NumpadDiv, NumpadDot, Comma, Period,\n"
";   Slash, Semicolon, Quote, LBracket, RBracket, Backslash, Minus, Equals,\n"
";   Grave - or a raw virtual-key code (decimal, or hex like 0x77).\n"
"; Set a key to None (or leave it empty) to unbind it; the menu key always stays bound.\n"
"ToggleKey=Ctrl+Shift+F8\n"
"ReloadKey=Ctrl+Shift+F9\n"
"\n"
"; --- Live ingame tuning ---\n"
"; PresetKey cycles Jitter -> Manual -> Adaptive.\n"
"; TargetKey cycles which setting UpKey/DownKey adjust:\n"
";   Jitter:   All -> Position -> Rotation      (step 20 ms)\n"
";   Manual:   All -> Pitch -> Yaw -> Roll -> Vertical -> Lateral -> Depth\n"
";             (step 5 strength)\n"
";   Adaptive: All -> Position -> Rotation      (step 0.1 Hz)\n"
"; Up = more smoothing, Down = less; hold to repeat. Current values show\n"
"; in the game window title, and the beep pitch tracks the value.\n"
"; Changes are live only - press SaveKey to write them into this file\n"
"; (ReloadKey discards live changes and re-reads the file).\n"
"; This file is also AUTO-RELOADED whenever it changes on disk, so\n"
"; edits made in a text editor apply live.\n"
"PresetKey=Ctrl+Shift+P\n"
"TargetKey=Ctrl+Shift+A\n"
"UpKey=Ctrl+Shift+Up\n"
"DownKey=Ctrl+Shift+Down\n"
"SaveKey=Ctrl+Shift+S\n"
"; Room scale re-anchor (see [RoomScale])\n"
"ReanchorKey=Ctrl+Shift+Home\n";

// Parses a bind string like "Ctrl+Shift+F8" (format in Hotkey.hpp). "None" or
// an empty value unbinds the function. Keeps the previous bind when the string
// has no usable key.
static HotkeyBind ParseBind(const char* str, const HotkeyBind& fallback) {
  HotkeyBind bind = fallback;
  if (!HotkeyParse(str, bind)) {
    LOGSTRF("Stabilizer: could not parse hotkey '%s', keeping the previous bind\n", str);
    return fallback;
  }
  return bind;
}

static bool ModifiersMatch(const HotkeyBind& bind) {
  return HotkeyModifiersMatch(bind);
}

static double NowSeconds() {
  LARGE_INTEGER freq, now;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&now);
  return (double)now.QuadPart / (double)freq.QuadPart;
}

// Repeat timing for held adjustment keys
static const double REPEAT_DELAY_S = 0.45;
static const double REPEAT_INTERVAL_S = 0.09;

// Steps a value and clamps it into [lo, hi]
static float StepClamp(float v, float delta, float lo, float hi) {
  v += delta;
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static float ReadIniFloat(const char* path, const char* section,
                          const char* key, float fallback) {
  char buf[64] = { 0 };
  char fallback_str[64];
  sprintf_s(fallback_str, "%f", fallback);
  GetPrivateProfileStringA(section, key, fallback_str, buf, sizeof(buf), path);
  return (float)atof(buf);
}

// Reads from `section`, falling back to the same key under [Stabilization]
// so older ini files that kept everything in one section keep working
static float ReadIniFloatCompat(const char* path, const char* section,
                                const char* key, float fallback) {
  char buf[64] = { 0 };
  GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), path);
  if (buf[0] != '\0')
    return (float)atof(buf);

  return ReadIniFloat(path, "Stabilization", key, fallback);
}

static int ReadIniInt(const char* path, const char* section,
                      const char* key, int fallback) {
  return (int)GetPrivateProfileIntA(section, key, fallback, path);
}

static float ClampF(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

// Wraps an angle to (-pi, pi]
static float WrapPi(float a) {
  while (a > PI_F) a -= 2.0f * PI_F;
  while (a < -PI_F) a += 2.0f * PI_F;
  return a;
}

// Extracts yaw/pitch/roll (radians) from the rotation part of a
// column-major pose matrix, using R = Ry(yaw) * Rx(pitch) * Rz(roll).
// Yaw = left-right look, pitch = up-down look, roll = tilt.
static void MatrixToEuler(const Matrix4& m, float& yaw, float& pitch, float& roll) {
  const float* e = m.get();

  float sp = ClampF(-e[9], -1.0f, 1.0f);
  pitch = asinf(sp);
  yaw = atan2f(e[8], e[10]);
  roll = atan2f(e[1], e[5]);
}

// Writes the rotation part of a column-major pose matrix from
// yaw/pitch/roll (radians), R = Ry(yaw) * Rx(pitch) * Rz(roll).
// Leaves translation untouched.
static void EulerToMatrix(float yaw, float pitch, float roll, Matrix4& m) {
  float cy = cosf(yaw), sy = sinf(yaw);
  float cp = cosf(pitch), sp = sinf(pitch);
  float cr = cosf(roll), sr = sinf(roll);

  // R[row][col] stored at m[col * 4 + row]
  m[0] = cy * cr + sy * sp * sr;
  m[4] = -cy * sr + sy * sp * cr;
  m[8] = sy * cp;

  m[1] = cp * sr;
  m[5] = cp * cr;
  m[9] = -sp;

  m[2] = -sy * cr + cy * sp * sr;
  m[6] = sy * sr + cy * sp * cr;
  m[10] = cy * cp;
}

// alpha for a first-order low-pass at the given cutoff frequency
static inline float CutoffAlpha(float cutoff_hz, float dt) {
  float tau = 1.0f / (2.0f * PI_F * cutoff_hz);
  return dt / (dt + tau);
}

// alpha for a per-axis manual strength: 0 = raw, 100 = max_ms of smoothing
static float AxisAlpha(float strength, float max_ms, float dt) {
  strength = ClampF(strength, 0.0f, 100.0f);
  float tau = (strength / 100.0f) * (max_ms / 1000.0f);
  if (tau <= 0.0f)
    return 1.0f;
  return 1.0f - expf(-dt / tau);
}

PoseStabilizer::PoseStabilizer() :
    enabled_(true), stabilize_position_(true), stabilize_rotation_(true), preset_(PRESET_JITTER),
    pos_smooth_ms_(180.0f), rot_smooth_ms_(140.0f),
    pitch_strength_(35.0f), yaw_strength_(35.0f), roll_strength_(35.0f),
    vertical_strength_(45.0f), lateral_strength_(45.0f), depth_strength_(45.0f),
    max_smooth_ms_(400.0f),
    pos_min_cutoff_(0.8f), pos_beta_(0.02f),
    rot_min_cutoff_(0.8f), rot_beta_(0.02f),
    adjust_target_(0),
    ini_mtime_(), next_mtime_check_(0.0),
    has_state_(false), last_time_(0.0),
    fyaw_(0.0f), fpitch_(0.0f), froll_(0.0f) {
  HotkeyBind ctrl_shift = { 0, true, true, false };

  toggle_bind_ = ctrl_shift; toggle_bind_.vk = VK_F8;
  reload_bind_ = ctrl_shift; reload_bind_.vk = VK_F9;
  preset_bind_ = ctrl_shift; preset_bind_.vk = 'P';
  target_bind_ = ctrl_shift; target_bind_.vk = 'A';
  up_bind_ = ctrl_shift; up_bind_.vk = VK_UP;
  down_bind_ = ctrl_shift; down_bind_.vk = VK_DOWN;
  save_bind_ = ctrl_shift; save_bind_.vk = 'S';

  BindState empty = { false, 0.0 };
  toggle_state_ = empty;
  reload_state_ = empty;
  preset_state_ = empty;
  target_state_ = empty;
  up_state_ = empty;
  down_state_ = empty;
  save_state_ = empty;
}

void PoseStabilizer::LoadConfig() {
  // Remembered so a reload only re-seeds the filter when it must (preset
  // switch / re-enable). Plain value changes keep smoothing continuous -
  // no hitch while tuning live.
  const int prev_preset = preset_;
  const bool was_enabled = enabled_;

  char ini_path[MAX_PATH * 2];
  GetIniPath(ini_path, sizeof(ini_path));

  // Create the ini with commented defaults on first run
  DWORD attribs = GetFileAttributesA(ini_path);
  if (attribs == INVALID_FILE_ATTRIBUTES) {
    FILE* fp = nullptr;
    if (fopen_s(&fp, ini_path, "w") == 0 && fp != nullptr) {
      fputs(DEFAULT_INI, fp);
      fclose(fp);
      LOGSTRF("Stabilizer: created default config at %s\n", ini_path);
    } else {
      LOGSTRF("Stabilizer: could not create %s, using built-in defaults\n", ini_path);
    }
  }

  enabled_ = ReadIniInt(ini_path, "Stabilization", "Enabled", 1) != 0;

  // Preset by name; falls back to the older numeric Mode key
  // (2 = fixed low-pass -> Jitter, 1 = one-euro -> Adaptive)
  char preset_str[32] = { 0 };
  GetPrivateProfileStringA("Stabilization", "Preset", "", preset_str,
                           sizeof(preset_str), ini_path);
  for (char* p = preset_str; *p; ++p)
    *p = (char)tolower((unsigned char)*p);

  if (strcmp(preset_str, "manual") == 0)
    preset_ = PRESET_MANUAL;
  else if (strcmp(preset_str, "adaptive") == 0)
    preset_ = PRESET_ADAPTIVE;
  else if (strcmp(preset_str, "jitter") == 0)
    preset_ = PRESET_JITTER;
  else
    preset_ = (ReadIniInt(ini_path, "Stabilization", "Mode", 2) == 1)
                  ? PRESET_ADAPTIVE : PRESET_JITTER;

  stabilize_position_ = ReadIniInt(ini_path, "Stabilization", "StabilizePosition", 1) != 0;
  stabilize_rotation_ = ReadIniInt(ini_path, "Stabilization", "StabilizeRotation", 1) != 0;

  pos_smooth_ms_ = ClampF(
      ReadIniFloatCompat(ini_path, "Jitter", "PosSmoothMs", 180.0f),
      0.0f, MAX_SMOOTH_MS_LIMIT);
  rot_smooth_ms_ = ClampF(
      ReadIniFloatCompat(ini_path, "Jitter", "RotSmoothMs", 140.0f),
      0.0f, MAX_SMOOTH_MS_LIMIT);

  pitch_strength_ = ReadIniFloat(ini_path, "Manual", "PitchStrength", 35.0f);
  yaw_strength_ = ReadIniFloat(ini_path, "Manual", "YawStrength", 35.0f);
  roll_strength_ = ReadIniFloat(ini_path, "Manual", "RollStrength", 35.0f);
  vertical_strength_ = ReadIniFloat(ini_path, "Manual", "VerticalStrength", 45.0f);
  lateral_strength_ = ReadIniFloat(ini_path, "Manual", "LateralStrength", 45.0f);
  depth_strength_ = ReadIniFloat(ini_path, "Manual", "DepthStrength", 45.0f);
  max_smooth_ms_ = ClampF(
      ReadIniFloat(ini_path, "Manual", "MaxSmoothMs", 400.0f),
      50.0f, MAX_SMOOTH_MS_LIMIT);

  pos_min_cutoff_ = ReadIniFloatCompat(ini_path, "Adaptive", "PosMinCutoff", 0.8f);
  pos_beta_ = ReadIniFloatCompat(ini_path, "Adaptive", "PosBeta", 0.02f);
  rot_min_cutoff_ = ReadIniFloatCompat(ini_path, "Adaptive", "RotMinCutoff", 0.8f);
  rot_beta_ = ReadIniFloatCompat(ini_path, "Adaptive", "RotBeta", 0.02f);

  struct BindEntry {
    const char* key;
    const char* fallback;
    HotkeyBind* bind;
  };

  char toggle_str[64] = { 0 };
  char reload_str[64] = { 0 };

  BindEntry entries[] = {
    { "PresetKey", "Ctrl+Shift+P", &preset_bind_ },
    { "TargetKey", "Ctrl+Shift+A", &target_bind_ },
    { "UpKey", "Ctrl+Shift+Up", &up_bind_ },
    { "DownKey", "Ctrl+Shift+Down", &down_bind_ },
    { "SaveKey", "Ctrl+Shift+S", &save_bind_ },
  };

  GetPrivateProfileStringA("Hotkeys", "ToggleKey", "Ctrl+Shift+F8",
                           toggle_str, sizeof(toggle_str), ini_path);
  GetPrivateProfileStringA("Hotkeys", "ReloadKey", "Ctrl+Shift+F9",
                           reload_str, sizeof(reload_str), ini_path);

  toggle_bind_ = ParseBind(toggle_str, toggle_bind_);
  reload_bind_ = ParseBind(reload_str, reload_bind_);

  for (int i = 0; i < sizeof(entries) / sizeof(entries[0]); ++i) {
    char buf[64] = { 0 };
    GetPrivateProfileStringA("Hotkeys", entries[i].key, entries[i].fallback,
                             buf, sizeof(buf), ini_path);
    *entries[i].bind = ParseBind(buf, *entries[i].bind);
  }

  bool overlay_enabled = ReadIniInt(ini_path, "Overlay", "Enabled", 0) != 0;
  float overlay_duration = ClampF(
      ReadIniFloat(ini_path, "Overlay", "DurationSec", 2.5f), 0.5f, 10.0f);
  Overlay_Configure(overlay_enabled, overlay_duration);

  if (preset_ != prev_preset) {
    // Per-preset filter state is incompatible - re-seed from raw.
    adjust_target_ = 0;
    has_state_ = false;
  } else if (enabled_ && !was_enabled) {
    // Re-enabled after being off: don't lerp from a stale old pose.
    has_state_ = false;
  }

  static const char* preset_names[] = { "Jitter", "Manual", "Adaptive" };
  LOGSTRF("Stabilizer: enabled=%d preset=%s toggle='%s' reload='%s'\n",
          enabled_ ? 1 : 0, preset_names[preset_], toggle_str, reload_str);

  if (preset_ == PRESET_MANUAL) {
    LOGSTRF("Stabilizer manual: pitch=%.0f yaw=%.0f roll=%.0f vert=%.0f lat=%.0f depth=%.0f maxMs=%.0f\n",
            pitch_strength_, yaw_strength_, roll_strength_,
            vertical_strength_, lateral_strength_, depth_strength_,
            max_smooth_ms_);
  } else if (preset_ == PRESET_JITTER) {
    LOGSTRF("Stabilizer jitter: posMs=%.0f rotMs=%.0f\n",
            pos_smooth_ms_, rot_smooth_ms_);
  }

  RememberIniMtime();
}

// Window-title feedback: cache the game's top-level window and its
// original title so we can append live stabilizer status to it
static HWND g_game_hwnd = NULL;
static char g_base_title[200] = { 0 };

static BOOL CALLBACK FindGameWindowProc(HWND hwnd, LPARAM lparam) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);

  if (pid == GetCurrentProcessId() && GetWindow(hwnd, GW_OWNER) == NULL &&
      IsWindowVisible(hwnd)) {
    *(HWND*)lparam = hwnd;
    return FALSE;
  }

  return TRUE;
}

// Runs the title update and beep off-thread: Beep blocks for its whole
// duration and SetWindowText round-trips to the game's message loop,
// neither of which may stall the pose thread
static void AsyncFeedback(const char* status_text, int beep_freq) {
  std::string status(status_text);

  std::thread([status, beep_freq]() {
    if (g_game_hwnd == NULL || !IsWindow(g_game_hwnd)) {
      g_game_hwnd = NULL;
      EnumWindows(FindGameWindowProc, (LPARAM)&g_game_hwnd);

      if (g_game_hwnd != NULL)
        GetWindowTextA(g_game_hwnd, g_base_title, sizeof(g_base_title));
    }

    if (g_game_hwnd != NULL) {
      char title[512];
      if (g_base_title[0] != '\0')
        _snprintf_s(title, _TRUNCATE, "%s  |  %s", g_base_title, status.c_str());
      else
        _snprintf_s(title, _TRUNCATE, "%s", status.c_str());
      SetWindowTextA(g_game_hwnd, title);
    }

    if (beep_freq > 0)
      Beep(beep_freq, 30);
  }).detach();
}

int PoseStabilizer::TargetCount() const {
  return (preset_ == PRESET_MANUAL) ? 7 : 3;
}

const char* PoseStabilizer::TargetName(int target) const {
  static const char* manual_names[] = {
    "All", "Pitch", "Yaw", "Roll", "Vertical", "Lateral", "Depth"
  };
  static const char* uniform_names[] = { "All", "Position", "Rotation" };

  const char** names = (preset_ == PRESET_MANUAL) ? manual_names : uniform_names;
  return names[target];
}

// Current value of the adjust target scaled to [0,1] for beep pitch
// (1 = maximum smoothing on every preset)
float PoseStabilizer::NormalizedTargetValue() const {
  switch (preset_) {
  case PRESET_MANUAL: {
    const float* axes[] = { &pitch_strength_, &yaw_strength_, &roll_strength_,
                            &vertical_strength_, &lateral_strength_, &depth_strength_ };
    if (adjust_target_ == 0) {
      float sum = 0.0f;
      for (int i = 0; i < 6; ++i) sum += *axes[i];
      return (sum / 6.0f) / 100.0f;
    }
    return *axes[adjust_target_ - 1] / 100.0f;
  }

  case PRESET_ADAPTIVE: {
    // Lower cutoff = more smoothing, so invert for the pitch scale
    float v;
    if (adjust_target_ == 1) v = pos_min_cutoff_;
    else if (adjust_target_ == 2) v = rot_min_cutoff_;
    else v = (pos_min_cutoff_ + rot_min_cutoff_) * 0.5f;
    return ClampF((5.0f - v) / 4.9f, 0.0f, 1.0f);
  }

  default: {
    float v;
    if (adjust_target_ == 1) v = pos_smooth_ms_;
    else if (adjust_target_ == 2) v = rot_smooth_ms_;
    else v = (pos_smooth_ms_ + rot_smooth_ms_) * 0.5f;
    return v / MAX_SMOOTH_MS_LIMIT;
  }
  }
}

void PoseStabilizer::PublishFeedback(bool pitch_beep) {
  char status[256];

  if (!enabled_) {
    _snprintf_s(status, _TRUNCATE, "Stab OFF");
  } else {
    switch (preset_) {
    case PRESET_MANUAL:
      _snprintf_s(status, _TRUNCATE,
                  "Stab[Manual] P%.0f Y%.0f R%.0f V%.0f L%.0f D%.0f > %s",
                  pitch_strength_, yaw_strength_, roll_strength_,
                  vertical_strength_, lateral_strength_, depth_strength_,
                  TargetName(adjust_target_));
      break;

    case PRESET_ADAPTIVE:
      _snprintf_s(status, _TRUNCATE,
                  "Stab[Adaptive] PosCut=%.1fHz RotCut=%.1fHz > %s",
                  pos_min_cutoff_, rot_min_cutoff_, TargetName(adjust_target_));
      break;

    default:
      _snprintf_s(status, _TRUNCATE,
                  "Stab[Jitter] Pos=%.0fms Rot=%.0fms > %s",
                  pos_smooth_ms_, rot_smooth_ms_, TargetName(adjust_target_));
      break;
    }
  }

  int freq = 0;
  if (pitch_beep)
    freq = 400 + (int)(700.0f * NormalizedTargetValue());

  Overlay_Notify(status);
  AsyncFeedback(status, freq);
}

void PoseStabilizer::CyclePreset() {
  preset_ = (preset_ + 1) % 3;
  adjust_target_ = 0;
  has_state_ = false;

  static const char* preset_names[] = { "Jitter", "Manual", "Adaptive" };
  LOGSTRF("Stabilizer: preset -> %s\n", preset_names[preset_]);
}

void PoseStabilizer::CycleTarget() {
  adjust_target_ = (adjust_target_ + 1) % TargetCount();
}

// direction: +1 = more smoothing, -1 = less
void PoseStabilizer::Adjust(int direction) {
  float dir = (float)direction;

  switch (preset_) {
  case PRESET_MANUAL: {
    float* axes[] = { &pitch_strength_, &yaw_strength_, &roll_strength_,
                      &vertical_strength_, &lateral_strength_, &depth_strength_ };
    if (adjust_target_ == 0) {
      for (int i = 0; i < 6; ++i)
        *axes[i] = StepClamp(*axes[i], dir * 5.0f, 0.0f, 100.0f);
    } else {
      float* v = axes[adjust_target_ - 1];
      *v = StepClamp(*v, dir * 5.0f, 0.0f, 100.0f);
    }
    break;
  }

  case PRESET_ADAPTIVE:
    // More smoothing = lower cutoff
    if (adjust_target_ == 0 || adjust_target_ == 1)
      pos_min_cutoff_ = StepClamp(pos_min_cutoff_, -dir * 0.1f, 0.1f, 5.0f);
    if (adjust_target_ == 0 || adjust_target_ == 2)
      rot_min_cutoff_ = StepClamp(rot_min_cutoff_, -dir * 0.1f, 0.1f, 5.0f);
    break;

  default:
    if (adjust_target_ == 0 || adjust_target_ == 1)
      pos_smooth_ms_ = StepClamp(pos_smooth_ms_, dir * 20.0f, 0.0f, MAX_SMOOTH_MS_LIMIT);
    if (adjust_target_ == 0 || adjust_target_ == 2)
      rot_smooth_ms_ = StepClamp(rot_smooth_ms_, dir * 20.0f, 0.0f, MAX_SMOOTH_MS_LIMIT);
    break;
  }
}

void PoseStabilizer::SaveConfig() {
  char ini_path[MAX_PATH * 2];
  GetIniPath(ini_path, sizeof(ini_path));

  char buf[64];
  static const char* preset_names[] = { "Jitter", "Manual", "Adaptive" };

  WritePrivateProfileStringA("Stabilization", "Enabled", enabled_ ? "1" : "0", ini_path);
  WritePrivateProfileStringA("Stabilization", "Preset", preset_names[preset_], ini_path);
  WritePrivateProfileStringA("Stabilization", "StabilizePosition", stabilize_position_ ? "1" : "0", ini_path);
  WritePrivateProfileStringA("Stabilization", "StabilizeRotation", stabilize_rotation_ ? "1" : "0", ini_path);

  sprintf_s(buf, "%.0f", pos_smooth_ms_);
  WritePrivateProfileStringA("Jitter", "PosSmoothMs", buf, ini_path);
  sprintf_s(buf, "%.0f", rot_smooth_ms_);
  WritePrivateProfileStringA("Jitter", "RotSmoothMs", buf, ini_path);

  struct FloatEntry { const char* key; float value; };
  FloatEntry manual_entries[] = {
    { "PitchStrength", pitch_strength_ }, { "YawStrength", yaw_strength_ },
    { "RollStrength", roll_strength_ }, { "VerticalStrength", vertical_strength_ },
    { "LateralStrength", lateral_strength_ }, { "DepthStrength", depth_strength_ },
    { "MaxSmoothMs", max_smooth_ms_ },
  };
  for (int i = 0; i < sizeof(manual_entries) / sizeof(manual_entries[0]); ++i) {
    sprintf_s(buf, "%.0f", manual_entries[i].value);
    WritePrivateProfileStringA("Manual", manual_entries[i].key, buf, ini_path);
  }

  sprintf_s(buf, "%.2f", pos_min_cutoff_);
  WritePrivateProfileStringA("Adaptive", "PosMinCutoff", buf, ini_path);
  sprintf_s(buf, "%.2f", rot_min_cutoff_);
  WritePrivateProfileStringA("Adaptive", "RotMinCutoff", buf, ini_path);

  LOGSTRF("Stabilizer: settings saved to %s\n", ini_path);

  // Own write - refresh the stored mtime so the auto-reload doesn't fire.
  RememberIniMtime();
}

void PoseStabilizer::RememberIniMtime() {
  char ini_path[MAX_PATH * 2];
  GetIniPath(ini_path, sizeof(ini_path));

  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (GetFileAttributesExA(ini_path, GetFileExInfoStandard, &fad))
    ini_mtime_ = fad.ftLastWriteTime;
}

void PoseStabilizer::CheckConfigFileChanged(double now) {
  if (now < next_mtime_check_)
    return;
  next_mtime_check_ = now + 0.5;

  char ini_path[MAX_PATH * 2];
  GetIniPath(ini_path, sizeof(ini_path));

  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (!GetFileAttributesExA(ini_path, GetFileExInfoStandard, &fad))
    return;

  if (CompareFileTime(&fad.ftLastWriteTime, &ini_mtime_) != 0) {
    // Changed on disk (edited while the game runs) - re-read silently.
    LOGSTRF("Stabilizer: config file changed on disk, reloading\n");
    LoadConfig();
  }
}

void PoseStabilizer::HandleHotkeys() {
  double now = NowSeconds();

  CheckConfigFileChanged(now);

  auto fired = [now](const HotkeyBind& bind, BindState& st, bool repeat) -> bool {
    // An unbound function (None) never fires
    bool down = !HotkeyIsNone(bind) && (GetAsyncKeyState(bind.vk) & 0x8000) != 0;
    bool result = false;

    if (down && !st.was_down) {
      if (ModifiersMatch(bind)) {
        result = true;
        st.next_repeat = now + REPEAT_DELAY_S;
      }
    } else if (down && repeat && st.next_repeat > 0.0 &&
               now >= st.next_repeat && ModifiersMatch(bind)) {
      result = true;
      st.next_repeat = now + REPEAT_INTERVAL_S;
    }

    if (!down)
      st.next_repeat = 0.0;

    st.was_down = down;
    return result;
  };

  if (fired(toggle_bind_, toggle_state_, false)) {
    enabled_ = !enabled_;
    has_state_ = false;
    MessageBeep(enabled_ ? MB_ICONASTERISK : MB_ICONHAND);
    LOGSTRF("Stabilizer: %s\n", enabled_ ? "enabled" : "disabled");
    PublishFeedback(false);
  }

  if (fired(reload_bind_, reload_state_, false)) {
    LoadConfig();
    MessageBeep(MB_OK);
    PublishFeedback(false);
  }

  if (fired(preset_bind_, preset_state_, false)) {
    CyclePreset();
    PublishFeedback(true);
  }

  if (fired(target_bind_, target_state_, false)) {
    CycleTarget();
    PublishFeedback(true);
  }

  if (fired(up_bind_, up_state_, true)) {
    Adjust(1);
    PublishFeedback(true);
  }

  if (fired(down_bind_, down_state_, true)) {
    Adjust(-1);
    PublishFeedback(true);
  }

  if (fired(save_bind_, save_state_, false)) {
    SaveConfig();
    MessageBeep(MB_OK);
    PublishFeedback(false);
  }
}

// Column-major element access: m[col * 4 + row]
PoseStabilizer::Quat PoseStabilizer::MatrixToQuat(const Matrix4& m) {
  const float* e = m.get();

  float r00 = e[0], r10 = e[1], r20 = e[2];
  float r01 = e[4], r11 = e[5], r21 = e[6];
  float r02 = e[8], r12 = e[9], r22 = e[10];

  Quat q;
  float trace = r00 + r11 + r22;

  if (trace > 0.0f) {
    float s = sqrtf(trace + 1.0f) * 2.0f;
    q.w = 0.25f * s;
    q.x = (r21 - r12) / s;
    q.y = (r02 - r20) / s;
    q.z = (r10 - r01) / s;
  } else if (r00 > r11 && r00 > r22) {
    float s = sqrtf(1.0f + r00 - r11 - r22) * 2.0f;
    q.w = (r21 - r12) / s;
    q.x = 0.25f * s;
    q.y = (r01 + r10) / s;
    q.z = (r02 + r20) / s;
  } else if (r11 > r22) {
    float s = sqrtf(1.0f + r11 - r00 - r22) * 2.0f;
    q.w = (r02 - r20) / s;
    q.x = (r01 + r10) / s;
    q.y = 0.25f * s;
    q.z = (r12 + r21) / s;
  } else {
    float s = sqrtf(1.0f + r22 - r00 - r11) * 2.0f;
    q.w = (r10 - r01) / s;
    q.x = (r02 + r20) / s;
    q.y = (r12 + r21) / s;
    q.z = 0.25f * s;
  }

  return q;
}

void PoseStabilizer::QuatToMatrix(const Quat& q, Matrix4& m) {
  float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
  float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
  float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;

  m[0] = 1.0f - 2.0f * (yy + zz);
  m[1] = 2.0f * (xy + wz);
  m[2] = 2.0f * (xz - wy);

  m[4] = 2.0f * (xy - wz);
  m[5] = 1.0f - 2.0f * (xx + zz);
  m[6] = 2.0f * (yz + wx);

  m[8] = 2.0f * (xz + wy);
  m[9] = 2.0f * (yz - wx);
  m[10] = 1.0f - 2.0f * (xx + yy);
}

PoseStabilizer::Quat PoseStabilizer::Nlerp(const Quat& a, const Quat& b, float t) {
  // Hemisphere correction so we always take the short way around
  float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
  float sign = (dot >= 0.0f) ? 1.0f : -1.0f;

  Quat r;
  r.x = a.x + t * (sign * b.x - a.x);
  r.y = a.y + t * (sign * b.y - a.y);
  r.z = a.z + t * (sign * b.z - a.z);
  r.w = a.w + t * (sign * b.w - a.w);

  float len = sqrtf(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
  if (len > 0.000001f) {
    r.x /= len; r.y /= len; r.z /= len; r.w /= len;
  } else {
    r = b;
  }

  return r;
}

void PoseStabilizer::ResetState(const Matrix4& raw_pose) {
  const float* e = raw_pose.get();

  fpos_[0] = e[12]; fpos_[1] = e[13]; fpos_[2] = e[14];
  prev_raw_pos_[0] = e[12]; prev_raw_pos_[1] = e[13]; prev_raw_pos_[2] = e[14];
  dpos_lp_[0] = dpos_lp_[1] = dpos_lp_[2] = 0.0f;

  fquat_ = MatrixToQuat(raw_pose);
  prev_raw_quat_ = fquat_;

  MatrixToEuler(raw_pose, fyaw_, fpitch_, froll_);

  has_state_ = true;
}

// Uniform smoothing on all axes (Jitter and Adaptive presets)
void PoseStabilizer::FilterUniform(const float raw_pos[3], const Quat& raw_quat,
                                   float dt) {
  float pos_alpha, rot_alpha;

  if (preset_ == PRESET_ADAPTIVE) {
    // Adaptive (one-euro): raise the cutoff as motion speeds up
    for (int i = 0; i < 3; ++i) {
      float d = (raw_pos[i] - prev_raw_pos_[i]) / dt;
      float da = CutoffAlpha(1.0f, dt);
      dpos_lp_[i] += da * (d - dpos_lp_[i]);
    }

    float speed = sqrtf(dpos_lp_[0] * dpos_lp_[0] +
                        dpos_lp_[1] * dpos_lp_[1] +
                        dpos_lp_[2] * dpos_lp_[2]);

    float dot = fabsf(raw_quat.x * prev_raw_quat_.x + raw_quat.y * prev_raw_quat_.y +
                      raw_quat.z * prev_raw_quat_.z + raw_quat.w * prev_raw_quat_.w);
    if (dot > 1.0f) dot = 1.0f;
    float ang_speed = 2.0f * acosf(dot) / dt;

    pos_alpha = CutoffAlpha(pos_min_cutoff_ + pos_beta_ * speed, dt);
    rot_alpha = CutoffAlpha(rot_min_cutoff_ + rot_beta_ * ang_speed, dt);
  } else {
    // Fixed low-pass: constant time-constant smoothing
    float pos_tau = pos_smooth_ms_ / 1000.0f;
    float rot_tau = rot_smooth_ms_ / 1000.0f;
    pos_alpha = (pos_tau > 0.0f) ? (1.0f - expf(-dt / pos_tau)) : 1.0f;
    rot_alpha = (rot_tau > 0.0f) ? (1.0f - expf(-dt / rot_tau)) : 1.0f;
  }

  // Split switches: a disabled half follows the raw pose exactly
  if (!stabilize_position_) pos_alpha = 1.0f;
  if (!stabilize_rotation_) rot_alpha = 1.0f;

  for (int i = 0; i < 3; ++i)
    fpos_[i] += pos_alpha * (raw_pos[i] - fpos_[i]);

  fquat_ = Nlerp(fquat_, raw_quat, rot_alpha);
}

// Independent per-axis smoothing (Manual preset). Position axes are
// resolved in the facing frame (filtered yaw), so "lateral" is always
// the camera's left/right regardless of which way it points.
void PoseStabilizer::FilterPerAxis(const float raw_pos[3], const Matrix4& raw_pose,
                                   float dt) {
  // Rotation: filter yaw/pitch/roll independently
  float ryaw, rpitch, rroll;
  MatrixToEuler(raw_pose, ryaw, rpitch, rroll);

  float ayaw = AxisAlpha(yaw_strength_, max_smooth_ms_, dt);
  float apitch = AxisAlpha(pitch_strength_, max_smooth_ms_, dt);
  float aroll = AxisAlpha(roll_strength_, max_smooth_ms_, dt);
  if (!stabilize_rotation_)
    ayaw = apitch = aroll = 1.0f; // raw looking-around

  fyaw_ = WrapPi(fyaw_ + ayaw * WrapPi(ryaw - fyaw_));
  fpitch_ += apitch * (rpitch - fpitch_);
  froll_ = WrapPi(froll_ + aroll * WrapPi(rroll - froll_));

  // Position: rotate the error into the facing (yaw) frame, scale each
  // axis by its own alpha, rotate back
  float dx = raw_pos[0] - fpos_[0];
  float dy = raw_pos[1] - fpos_[1];
  float dz = raw_pos[2] - fpos_[2];

  float cy = cosf(fyaw_), sy = sinf(fyaw_);

  float lateral = cy * dx - sy * dz;
  float vertical = dy;
  float depth = sy * dx + cy * dz;

  if (stabilize_position_) {
    lateral *= AxisAlpha(lateral_strength_, max_smooth_ms_, dt);
    vertical *= AxisAlpha(vertical_strength_, max_smooth_ms_, dt);
    depth *= AxisAlpha(depth_strength_, max_smooth_ms_, dt);
  } // else: raw movement, the full error is applied at once

  fpos_[0] += cy * lateral + sy * depth;
  fpos_[1] += vertical;
  fpos_[2] += -sy * lateral + cy * depth;
}

Matrix4 PoseStabilizer::Filter(const Matrix4& raw_pose) {
  if (!enabled_)
    return raw_pose;

  const float* e = raw_pose.get();
  float raw_pos[3] = { e[12], e[13], e[14] };
  Quat raw_quat = MatrixToQuat(raw_pose);

  LARGE_INTEGER freq, now;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&now);
  double t = (double)now.QuadPart / (double)freq.QuadPart;

  float dt = (float)(t - last_time_);
  last_time_ = t;

  // First sample, or tracking was interrupted: snap to the raw pose
  if (!has_state_ || dt <= 0.0f || dt > 0.5f) {
    ResetState(raw_pose);
    return raw_pose;
  }

  Matrix4 out(raw_pose);

  if (preset_ == PRESET_MANUAL) {
    FilterPerAxis(raw_pos, raw_pose, dt);
    EulerToMatrix(fyaw_, fpitch_, froll_, out);
  } else {
    FilterUniform(raw_pos, raw_quat, dt);
    QuatToMatrix(fquat_, out);
  }

  prev_raw_pos_[0] = raw_pos[0];
  prev_raw_pos_[1] = raw_pos[1];
  prev_raw_pos_[2] = raw_pos[2];
  prev_raw_quat_ = raw_quat;

  out[12] = fpos_[0];
  out[13] = fpos_[1];
  out[14] = fpos_[2];

  return out;
}
