// Offline checks for OVRInject\Vive\PoseTiming.hpp. No game, no headset.
// Build + run: build.bat. Exit code = number of failed checks.
#define POSETIMING_NO_LOG
#include "../../OVRInject/Vive/PoseTiming.hpp"
#include <cstdio>
#include <vector>
#include <random>

using namespace PoseTiming;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) { ++g_pass; } else { ++g_fail; printf("FAIL %s:%d  ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const float PI_F = 3.14159265f;

// Yaw (rotation about Y) as a pose, plus a position moving along X.
static Pose YawPose(float yawRad, float x) {
  Pose p;
  p.p[0] = x; p.p[1] = 1.6f; p.p[2] = 0.0f;
  p.q[0] = 0.0f; p.q[1] = sinf(yawRad * 0.5f); p.q[2] = 0.0f; p.q[3] = cosf(yawRad * 0.5f);
  return p;
}
static float YawOf(const Pose& p) { return 2.0f * atan2f(p.q[1], p.q[3]); }

static Sample MakeSample(double t, float yaw, float x) {
  Sample s; s.t = t; s.view = YawPose(yaw, x); s.tracked = YawPose(yaw, x * 0.25f); return s;
}

static void TestMatrixRoundTrip() {
  Pose a = YawPose(0.7f, 1.25f);
  float q2[4] = { 0.2f, -0.4f, 0.1f, 0.0f };
  q2[3] = sqrtf(1.0f - (q2[0] * q2[0] + q2[1] * q2[1] + q2[2] * q2[2]));
  memcpy(a.q, q2, sizeof(q2));
  float m[16];
  MatrixFromPose(a, m);
  Pose b = PoseFromMatrix(m);
  float dot = fabsf(a.q[0] * b.q[0] + a.q[1] * b.q[1] + a.q[2] * b.q[2] + a.q[3] * b.q[3]);
  CHECK(dot > 0.99999f, "matrix round trip keeps the rotation (dot %.6f)", dot);
  CHECK(fabsf(a.p[0] - b.p[0]) < 1e-6f && fabsf(a.p[1] - b.p[1]) < 1e-6f, "matrix round trip keeps the position");
}

static void TestSlerp() {
  float a[4] = { 0, 0, 0, 1 };
  Pose b = YawPose(10.0f * PI_F / 180.0f, 0);
  float o[4];
  Slerp(a, b.q, 0.5f, o);
  CHECK(fabsf(2.0f * atan2f(o[1], o[3]) * 180.0f / PI_F - 5.0f) < 0.01f, "slerp halfway of 10 deg is 5 deg");
  Slerp(a, b.q, 1.5f, o);
  CHECK(fabsf(2.0f * atan2f(o[1], o[3]) * 180.0f / PI_F - 15.0f) < 0.01f, "slerp at 1.5 extrapolates 10 deg to 15 deg");
  Pose c = YawPose(350.0f * PI_F / 180.0f, 0);   // = -10 deg: must take the short way
  Slerp(a, c.q, 0.5f, o);
  CHECK(fabsf(2.0f * atan2f(o[1], o[3]) * 180.0f / PI_F + 5.0f) < 0.01f, "slerp takes the short way round");
}

static void TestEdges() {
  Sample out; double age;
  CHECK(!SampleFrom(nullptr, 0, 1.0, 1, 0.02, 0.02, &out, &age), "no samples -> nothing to sample");
  Sample one = MakeSample(1.0, 0.3f, 0.0f);
  CHECK(SampleFrom(&one, 1, 1.5, 2, 0.02, 0.02, &out, &age) && fabsf(YawOf(out.view) - 0.3f) < 1e-5f,
        "one sample -> held");
  Sample two[2] = { MakeSample(1.0, 0.0f, 0.0f), MakeSample(1.01, 0.1f, 0.0f) };
  SampleFrom(two, 2, 2.0, 2, 0.02, 0.02, &out, &age);   // a second later: prediction must be capped
  CHECK(fabsf(YawOf(out.view) - 0.3f) < 1e-3f, "extrapolation is capped at MaxPredict (got %.4f, want 0.3)", YawOf(out.view));
  SampleFrom(two, 2, 1.005, 1, 0.0, 0.02, &out, &age);
  CHECK(fabsf(YawOf(out.view) - 0.05f) < 1e-4f && fabsf(out.view.p[0]) < 1e-6f, "interpolates between two samples");
  CHECK(fabsf(YawOf(out.tracked) - 0.05f) < 1e-4f, "tracked pose is interpolated the same way");
  Sample gap[2] = { MakeSample(1.0, 0.0f, 0.0f), MakeSample(1.3, 0.3f, 0.0f) };   // 300 ms gap: no prediction across it
  SampleFrom(gap, 2, 1.31, 2, 0.02, 0.02, &out, &age);
  CHECK(fabsf(YawOf(out.view) - 0.3f) < 1e-5f, "no extrapolation across a tracking gap");
}

// The hitch, reproduced: HMD poses at 60 Hz of a smooth 90 deg/s turn, game frames at ~43 fps with jitter.
// For each game frame, (yaw moved) / (frame time) should equal the true speed. Mode 0's "newest pose"
// gives 1-step and 2-step frames; modes 1 and 2 should track real frame time.
static void TestCadence() {
  const double hmdDt = 1.0 / 60.0;
  const float speed = 90.0f * PI_F / 180.0f;
  std::vector<Sample> poses;
  for (int i = 0; i < 1200; ++i) {
    double t = i * hmdDt + 0.0003 * ((i * 7919) % 5 - 2);   // small timestamp jitter
    poses.push_back(MakeSample(t, speed * (float)(i * hmdDt), 0.0f));
  }
  std::mt19937 rng(12345);
  std::uniform_real_distribution<double> jit(-0.002, 0.002);

  for (int mode = 0; mode <= 2; ++mode) {
    double t = 1.0, prevT = 0.0;
    float prevYaw = 0.0f;
    int frames = 0, bad = 0;
    double worst = 0.0;
    int oneStep = 0, twoStep = 0;
    while (t < 18.0) {
      int n = 0;
      while (n < (int)poses.size() && poses[n].t <= t) ++n;   // what the HMD thread has delivered so far
      int k = n < 64 ? n : 64;
      const Sample* ring = &poses[n - k];
      Sample out; double age;
      double delay = DelayFor(ring, k);
      SampleFrom(ring, k, t, mode, delay, 0.020, &out, &age);
      float yaw = YawOf(out.view);
      if (prevT > 0.0) {
        float moved = yaw - prevYaw;
        while (moved < -PI_F) moved += 2 * PI_F;   // quaternion yaw spans (-2pi, 2pi]: unwrap fully
        while (moved > PI_F) moved -= 2 * PI_F;
        double dt = t - prevT;
        double ratio = moved / (speed * dt);   // 1.0 = moved exactly by real frame time
        double dev = fabs(ratio - 1.0);
        if (dev > worst) worst = dev;
        if (dev > 0.10) ++bad;
        if (mode == 0) { int steps = (int)lround(moved / (speed * hmdDt)); if (steps == 1) ++oneStep; if (steps == 2) ++twoStep; }
        ++frames;
      }
      prevT = t; prevYaw = yaw;
      t += 1.0 / 43.0 + jit(rng);
    }
    printf("  mode %d: %d frames, %d off by >10%% from real frame time, worst %.0f%%%s\n", mode, frames, bad,
           worst * 100.0, mode == 0 ? "" : "");
    if (mode == 0) {
      printf("          (mode 0: %d one-step and %d two-step frames - the hitch)\n", oneStep, twoStep);
      CHECK(bad > frames / 4, "mode 0 reproduces the uneven 1-step / 2-step motion (%d of %d frames off)", bad, frames);
    } else {
      CHECK(bad == 0, "mode %d moves the camera by real frame time on every frame (%d of %d off, worst %.1f%%)",
            mode, bad, frames, worst * 100.0);
    }
  }
}

static void TestTrackState() {
  float m[16];
  MatrixFromPose(YawPose(0.1f, 0.0f), m);
  NoteRaw(10.0, m, true);
  CHECK(State(10.0) == TRACK_OK, "fresh pose -> ok");
  NoteRaw(10.2, m, true);
  CHECK(State(10.2) == TRACK_OK, "unchanged for 200 ms -> still ok");
  NoteRaw(10.3, m, true);
  CHECK(State(10.3) == TRACK_FROZEN, "unchanged for 300 ms while ok -> frozen");
  MatrixFromPose(YawPose(0.11f, 0.0f), m);
  NoteRaw(10.31, m, true);
  CHECK(State(10.31) == TRACK_OK, "pose moves again -> ok");
  NoteRaw(10.32, m, false);
  CHECK(State(10.32) == TRACK_LOST, "driver says not Running_OK -> lost");
  MatrixFromPose(YawPose(0.12f, 0.0f), m);
  NoteRaw(10.33, m, true);
  CHECK(State(10.70) == TRACK_LOST, "no poses at all for 370 ms -> lost");
}

// Mode 2 must not extrapolate through a tracking reset (2.5 m jump) or a big rotation jump.
static void TestNoPredictThroughJump() {
  Sample s[4] = { MakeSample(1.000, 0.00f, 0.00f), MakeSample(1.010, 0.01f, 0.01f),
                  MakeSample(1.020, 0.02f, 0.02f), MakeSample(1.030, 0.03f, 2.53f) };   // reset: 2.5 m jump
  Sample out; double age;
  SampleFrom(s, 4, 1.045, 2, 0.0, 0.020, &out, &age);
  CHECK(fabsf(out.view.p[0] - 2.53f) < 1e-4f, "mode 2 holds the newest pose after a 2.5 m reset (got x=%.3f)", out.view.p[0]);
  Sample r[4] = { MakeSample(1.000, 0.0f, 0.0f), MakeSample(1.010, 0.0f, 0.0f),
                  MakeSample(1.020, 0.0f, 0.0f), MakeSample(1.030, 1.2f, 0.0f) };       // ~69 deg in one step
  SampleFrom(r, 4, 1.045, 2, 0.0, 0.020, &out, &age);
  CHECK(fabsf(YawOf(out.view) - 1.2f) < 1e-4f, "mode 2 holds the newest pose after a rotation jump (yaw %.3f)", YawOf(out.view));
  Sample m[4] = { MakeSample(1.000, 0.00f, 0.000f), MakeSample(1.010, 0.01f, 0.005f),
                  MakeSample(1.020, 0.02f, 0.010f), MakeSample(1.030, 0.03f, 0.015f) }; // ordinary motion
  SampleFrom(m, 4, 1.040, 2, 0.0, 0.020, &out, &age);
  CHECK(fabsf(out.view.p[0] - 0.020f) < 1e-4f, "ordinary motion is still predicted (x=%.4f, want 0.020)", out.view.p[0]);

  // Fast but real motion must still be predicted, at any driver rate.
  auto Walk = [](double hz, float speed, float turnDegPerS, Sample* w) {
    for (int i = 0; i < 4; ++i) {
      double t = 1.0 + i / hz;
      Sample s = MakeSample(t, turnDegPerS * 0.0174533f * (float)(i / hz), 0.0f);
      s.tracked.p[0] = speed * (float)(i / hz);         // real metres
      s.view.p[0] = 4.0f * s.tracked.p[0];              // RoomScale PlaneGain 4 on top
      w[i] = s;
    }
  };
  Sample w[4];
  Walk(60.0, 1.6f, 0.0f, w);   // brisk walk, 60 Hz
  SampleFrom(w, 4, w[3].t + 0.010, 2, 0.0, 0.020, &out, &age);
  CHECK(out.view.p[0] > w[3].view.p[0] + 1e-4f, "a 1.6 m/s walk (x4 RoomScale) at 60 Hz is predicted, not held");
  Walk(30.0, 1.0f, 0.0f, w);   // 30 Hz driver
  SampleFrom(w, 4, w[3].t + 0.010, 2, 0.0, 0.020, &out, &age);
  CHECK(out.view.p[0] > w[3].view.p[0] + 1e-4f, "a 1.0 m/s walk with a 30 Hz driver is predicted, not held");
  Walk(30.0, 0.0f, 320.0f, w); // quick look, 30 Hz
  SampleFrom(w, 4, w[3].t + 0.010, 2, 0.0, 0.020, &out, &age);
  CHECK(YawOf(out.view) > YawOf(w[3].view) + 1e-4f, "a 320 deg/s look at 30 Hz is predicted, not held");
}

int main() {
  TestNoPredictThroughJump();
  TestMatrixRoundTrip();
  TestSlerp();
  TestEdges();
  TestCadence();
  TestTrackState();
  printf("PoseTimingTest: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
