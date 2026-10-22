// Textually includes PoseStabilizer.cpp so the file-static parser and
// math functions are visible for testing.
#include "Vive/PoseStabilizer.cpp"

#include <cstdio>

using namespace OVRInject;

static int failures = 0;

static void Check(const char* input, int vk, bool c, bool s, bool a) {
  HotkeyBind fallback = { 0x999, false, false, false };
  HotkeyBind b = ParseBind(input, fallback);
  bool ok = (b.vk == vk && b.ctrl == c && b.shift == s && b.alt == a);
  printf("%-22s -> vk=0x%02X ctrl=%d shift=%d alt=%d  %s\n",
         input, b.vk, b.ctrl, b.shift, b.alt, ok ? "OK" : "FAIL");
  if (!ok) ++failures;
}

static void CheckNear(const char* label, float got, float want, float tol) {
  bool ok = fabsf(got - want) <= tol;
  printf("%-40s got=%9.5f want=%9.5f  %s\n", label, got, want, ok ? "OK" : "FAIL");
  if (!ok) ++failures;
}

static void EulerRoundTrip(float yaw_deg, float pitch_deg, float roll_deg) {
  float y = yaw_deg * PI_F / 180.0f;
  float p = pitch_deg * PI_F / 180.0f;
  float r = roll_deg * PI_F / 180.0f;

  Matrix4 m;
  EulerToMatrix(y, p, r, m);

  float y2, p2, r2;
  MatrixToEuler(m, y2, p2, r2);

  char label[128];
  sprintf_s(label, "euler(%g,%g,%g) yaw", yaw_deg, pitch_deg, roll_deg);
  CheckNear(label, y2, y, 0.0005f);
  sprintf_s(label, "euler(%g,%g,%g) pitch", yaw_deg, pitch_deg, roll_deg);
  CheckNear(label, p2, p, 0.0005f);
  sprintf_s(label, "euler(%g,%g,%g) roll", yaw_deg, pitch_deg, roll_deg);
  CheckNear(label, r2, r, 0.0005f);
}


// Live-tuning step/clamp and the tuning hotkey defaults
static void TuningTests() {
  printf("\n=== StepClamp ===\n");
  CheckNear("step up within range", StepClamp(45.0f, 5.0f, 0.0f, 100.0f), 50.0f, 0.0001f);
  CheckNear("clamp at 100", StepClamp(98.0f, 5.0f, 0.0f, 100.0f), 100.0f, 0.0001f);
  CheckNear("clamp at 0", StepClamp(3.0f, -5.0f, 0.0f, 100.0f), 0.0f, 0.0001f);
  CheckNear("ms step", StepClamp(180.0f, 20.0f, 0.0f, 600.0f), 200.0f, 0.0001f);
  CheckNear("ms clamp 600", StepClamp(595.0f, 20.0f, 0.0f, 600.0f), 600.0f, 0.0001f);
  CheckNear("cutoff clamp low", StepClamp(0.15f, -0.1f, 0.1f, 5.0f), 0.1f, 0.0001f);

  printf("\n=== Default tuning binds parse ===\n");
  Check("Ctrl+Shift+P", 'P', true, true, false);
  Check("Ctrl+Shift+A", 'A', true, true, false);
  Check("Ctrl+Shift+Up", VK_UP, true, true, false);
  Check("Ctrl+Shift+Down", VK_DOWN, true, true, false);
  Check("Ctrl+Shift+S", 'S', true, true, false);
}
int main() {
  TuningTests();
  printf("=== Hotkey parser ===\n");
  Check("Ctrl+Shift+F8", 0x77, true, true, false);
  Check("Alt+Home", VK_HOME, false, false, true);
  Check("Shift+Numpad5", VK_NUMPAD5, false, true, false);
  Check("Ctrl+0x77", 0x77, true, false, false);
  Check("garbage!!", 0x999, false, false, false);
  // None / empty = unbound (vk 0), written back as "None"
  Check("None", 0, false, false, false);
  Check(" nOnE ", 0, false, false, false);
  Check("", 0, false, false, false);
  Check("Nonexistent", 0x999, false, false, false);
  {
    HotkeyBind none = { 0, false, false, false };
    char text[16];
    HotkeyFormat(none, text, sizeof(text));
    bool ok = strcmp(text, "None") == 0;
    printf("%-22s -> \"%s\"  %s\n", "format unbound", text, ok ? "OK" : "FAIL");
    if (!ok) ++failures;
  }

  printf("\n=== Euler round trips ===\n");
  EulerRoundTrip(0, 0, 0);
  EulerRoundTrip(30, -20, 10);
  EulerRoundTrip(-120, 45, -30);
  EulerRoundTrip(170, -80, 5);
  EulerRoundTrip(-179, 89, 0);

  printf("\n=== Rotation matrix orthonormality ===\n");
  {
    Matrix4 m;
    EulerToMatrix(1.1f, -0.6f, 0.4f, m);
    const float* e = m.get();
    // Column lengths must be 1
    for (int c = 0; c < 3; ++c) {
      float len = sqrtf(e[c*4]*e[c*4] + e[c*4+1]*e[c*4+1] + e[c*4+2]*e[c*4+2]);
      char label[64];
      sprintf_s(label, "column %d length", c);
      CheckNear(label, len, 1.0f, 0.0005f);
    }
    // Columns orthogonal
    float d01 = e[0]*e[4] + e[1]*e[5] + e[2]*e[6];
    CheckNear("col0 . col1", d01, 0.0f, 0.0005f);
  }

  printf("\n=== AxisAlpha behavior ===\n");
  // strength 0 -> alpha 1 (raw pass-through)
  CheckNear("strength 0 = raw", AxisAlpha(0.0f, 400.0f, 0.011f), 1.0f, 0.00001f);
  // more strength -> smaller alpha (more smoothing)
  {
    float a30 = AxisAlpha(30.0f, 400.0f, 0.011f);
    float a90 = AxisAlpha(90.0f, 400.0f, 0.011f);
    bool ok = a90 < a30 && a30 < 1.0f && a90 > 0.0f;
    printf("%-40s a30=%.4f a90=%.4f  %s\n", "monotonic strength", a30, a90, ok ? "OK" : "FAIL");
    if (!ok) ++failures;
  }
  // clamping beyond 100 == 100
  CheckNear("strength 250 clamps to 100",
            AxisAlpha(250.0f, 400.0f, 0.011f),
            AxisAlpha(100.0f, 400.0f, 0.011f), 0.000001f);

  printf("\n=== WrapPi ===\n");
  CheckNear("wrap 190deg", WrapPi(190.0f * PI_F / 180.0f), -170.0f * PI_F / 180.0f, 0.001f);
  CheckNear("wrap -190deg", WrapPi(-190.0f * PI_F / 180.0f), 170.0f * PI_F / 180.0f, 0.001f);
  CheckNear("wrap 10deg", WrapPi(10.0f * PI_F / 180.0f), 10.0f * PI_F / 180.0f, 0.0001f);

  printf("\n=== Manual filter convergence (simulated) ===\n");
  {
    // Simulate: vertical jitter only, VerticalStrength=80 LateralStrength=0.
    // Feed a pose with sinusoidal Y jitter and constant X drift; lateral
    // must track raw exactly, vertical must be strongly attenuated.
    float fpos[3] = { 0, 1.6f, 0 };
    float maxms = 400.0f;
    float dt = 1.0f / 90.0f;
    float t = 0;
    float max_vert_err = 0, max_lat_err = 0;
    float raw_x = 0, raw_y = 1.6f;
    for (int i = 0; i < 900; ++i) {
      t += dt;
      raw_x = 0.2f * t;                       // steady lateral motion
      raw_y = 1.6f + 0.03f * sinf(2.0f * PI_F * 8.0f * t);  // 8 Hz vertical jitter
      float dx = raw_x - fpos[0];
      float dy = raw_y - fpos[1];
      // yaw = 0 frame: lateral = dx, vertical = dy
      float lat = dx * AxisAlpha(0.0f, maxms, dt);
      float vert = dy * AxisAlpha(80.0f, maxms, dt);
      fpos[0] += lat;
      fpos[1] += vert;
      if (i > 200) {
        float ve = fabsf(fpos[1] - 1.6f);       // deviation from jitter center
        float rawve = 0.03f;
        if (ve > max_vert_err) max_vert_err = ve;
        float le = fabsf(fpos[0] - raw_x);
        if (le > max_lat_err) max_lat_err = le;
      }
    }
    bool vert_ok = max_vert_err < 0.03f * 0.15f;  // >85% of jitter removed
    bool lat_ok = max_lat_err < 0.0001f;          // raw pass-through
    printf("%-40s residual=%.5f (raw amp 0.03)  %s\n", "vertical jitter attenuated >85%",
           max_vert_err, vert_ok ? "OK" : "FAIL");
    printf("%-40s err=%.7f  %s\n", "lateral strength 0 = exact raw",
           max_lat_err, lat_ok ? "OK" : "FAIL");
    if (!vert_ok) ++failures;
    if (!lat_ok) ++failures;
  }

  printf("\n%s (%d failures)\n", failures == 0 ? "ALL PASS" : "FAILURES", failures);
  return failures;
}

