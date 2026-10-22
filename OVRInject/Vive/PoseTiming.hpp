#pragma once
// ---------------------------------------------------------------------------
// PoseTiming - the game camera takes the head pose at the GAME frame's time.
//
// Poses arrive on the HMD render thread at the compositor's fixed cadence
// (often 60 Hz). Originally the game ran SyncOnPoses -> WaitGetPosesSimple,
// which busy-waits for the next pose and then uses the newest one. At 40-45
// game fps that is one pose step on some frames and two on others, which shows
// as a periodic hitch in head motion, and the wait itself snaps game frames
// onto the compositor's grid.
//
// The HMD thread therefore keeps a short ring of timestamped, fully processed
// poses (after PoseGuard, the stabilizer and RoomScale). The game thread, in
// SyncOnPoses, can sample that ring at its OWN time instead of waiting:
//   PoseTiming=0  original behaviour: wait for the next pose, use the newest (default)
//   PoseTiming=1  interpolate at (now - one pose period): smoothest, adds
//                 about one HMD frame of latency
//   PoseTiming=2  extrapolate to now from the last few poses (capped at
//                 MaxPredictMs): no added latency, can overshoot a sudden stop
// With 1 or 2 the game thread is the ONLY writer of HMDSupport::hmd_matrix_;
// the HMD thread only fills the ring. (With 0 the HMD thread writes it as
// before, which also races the game thread's reads - that race goes away too.)
//
// Each sample also carries the TRACKED pose: after the stabilizer, before
// RoomScale, in real metres. It is used to tell tracking jumps from motion.
//
// Config: gtaovr\stabilizer.ini [Timing], re-read once a second:
//   PoseTiming=0..2   InterpDelayMs=0 (0 = auto, about one pose period)
//   MaxPredictMs=20   TraceFrames=0 (1 = log a 300-game-frame summary)
//
// Header-only and free of D3D/OpenVR types so Tests\PoseTimingTest can include
// it. Define POSETIMING_NO_LOG there.
// ---------------------------------------------------------------------------

#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifndef POSETIMING_NO_LOG
#include "../Log.hpp"
#else
#define LOGSTRF(...) ((void)0)
#endif

namespace PoseTiming {

  // q = x, y, z, w. Matrices are column-major (m[col * 4 + row]), translation
  // in m[12..14], like Matrix4 and PoseStabilizer.
  struct Pose { float p[3]; float q[4]; };

  struct Sample {
    double t;      // QPC seconds when the HMD thread produced it
    Pose view;     // final head pose (tracking space, NOT inverted)
    Pose tracked;  // after the stabilizer, before RoomScale (real metres)
  };

  enum TrackState { TRACK_NONE = 0, TRACK_OK = 1, TRACK_LOST = 2, TRACK_FROZEN = 3 };

  // ---------------------------------------------------------------- maths --

  inline double Now() {
    static LARGE_INTEGER freq = { 0 };
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
  }

  inline void QuatFromMatrix(const float* e, float q[4]) {
    float r00 = e[0], r10 = e[1], r20 = e[2];
    float r01 = e[4], r11 = e[5], r21 = e[6];
    float r02 = e[8], r12 = e[9], r22 = e[10];
    float trace = r00 + r11 + r22;
    if (trace > 0.0f) {
      float s = sqrtf(trace + 1.0f) * 2.0f;
      q[3] = 0.25f * s; q[0] = (r21 - r12) / s; q[1] = (r02 - r20) / s; q[2] = (r10 - r01) / s;
    } else if (r00 > r11 && r00 > r22) {
      float s = sqrtf(1.0f + r00 - r11 - r22) * 2.0f;
      q[3] = (r21 - r12) / s; q[0] = 0.25f * s; q[1] = (r01 + r10) / s; q[2] = (r02 + r20) / s;
    } else if (r11 > r22) {
      float s = sqrtf(1.0f + r11 - r00 - r22) * 2.0f;
      q[3] = (r02 - r20) / s; q[0] = (r01 + r10) / s; q[1] = 0.25f * s; q[2] = (r12 + r21) / s;
    } else {
      float s = sqrtf(1.0f + r22 - r00 - r11) * 2.0f;
      q[3] = (r10 - r01) / s; q[0] = (r02 + r20) / s; q[1] = (r12 + r21) / s; q[2] = 0.25f * s;
    }
    float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n > 0.0f) { q[0] /= n; q[1] /= n; q[2] /= n; q[3] /= n; }
    if (q[3] < 0.0f) { q[0] = -q[0]; q[1] = -q[1]; q[2] = -q[2]; q[3] = -q[3]; }   // one sign: w >= 0
  }

  inline Pose PoseFromMatrix(const float* m) {
    Pose r;
    r.p[0] = m[12]; r.p[1] = m[13]; r.p[2] = m[14];
    QuatFromMatrix(m, r.q);
    return r;
  }

  inline void MatrixFromPose(const Pose& s, float m[16]) {
    float x = s.q[0], y = s.q[1], z = s.q[2], w = s.q[3];
    float xx = x * x, yy = y * y, zz = z * z, xy = x * y, xz = x * z, yz = y * z;
    float wx = w * x, wy = w * y, wz = w * z;
    m[0] = 1.0f - 2.0f * (yy + zz); m[1] = 2.0f * (xy + wz);        m[2] = 2.0f * (xz - wy);        m[3] = 0.0f;
    m[4] = 2.0f * (xy - wz);        m[5] = 1.0f - 2.0f * (xx + zz); m[6] = 2.0f * (yz + wx);        m[7] = 0.0f;
    m[8] = 2.0f * (xz + wy);        m[9] = 2.0f * (yz - wx);        m[10] = 1.0f - 2.0f * (xx + yy); m[11] = 0.0f;
    m[12] = s.p[0]; m[13] = s.p[1]; m[14] = s.p[2]; m[15] = 1.0f;
  }

  inline void QuatMul(const float a[4], const float b[4], float o[4]) {
    float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    o[0] = x; o[1] = y; o[2] = z; o[3] = w;
  }

  // a -> b by u; u outside 0..1 extrapolates along the same rotation axis.
  inline void Slerp(const float a[4], const float b[4], float u, float o[4]) {
    float ac[4] = { -a[0], -a[1], -a[2], a[3] };
    float d[4];
    QuatMul(ac, b, d);                          // rotation from a to b
    if (d[3] < 0.0f) { d[0] = -d[0]; d[1] = -d[1]; d[2] = -d[2]; d[3] = -d[3]; }
    float sinHalf = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    float angle = 2.0f * atan2f(sinHalf, d[3]);
    float step[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    if (sinHalf > 1e-7f) {
      float h = 0.5f * angle * u, s = sinf(h) / sinHalf;
      step[0] = d[0] * s; step[1] = d[1] * s; step[2] = d[2] * s; step[3] = cosf(h);
    }
    QuatMul(a, step, o);
    float n = sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2] + o[3] * o[3]);
    if (n > 0.0f) { o[0] /= n; o[1] /= n; o[2] /= n; o[3] /= n; }
    if (o[3] < 0.0f) { o[0] = -o[0]; o[1] = -o[1]; o[2] = -o[2]; o[3] = -o[3]; }
  }

  // Angle between two orientations, radians.
  inline float AngleBetween(const float a[4], const float b[4]) {
    float d = fabsf(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
    if (d > 1.0f) d = 1.0f;
    return 2.0f * acosf(d);
  }

  inline Pose Blend(const Pose& a, const Pose& b, float u) {
    Pose r;
    for (int i = 0; i < 3; ++i) r.p[i] = a.p[i] + (b.p[i] - a.p[i]) * u;
    Slerp(a.q, b.q, u, r.q);
    return r;
  }

  // ----------------------------------------------------------------- ring --

  struct Config {
    int   mode = 0;
    float interpDelayMs = 0.0f;   // 0 = auto
    float maxPredictMs = 20.0f;
    bool  trace = false;
  };

  struct Ring {
    static const int N = 64;
    Sample s[N];
    int head = 0;    // next slot to write
    int count = 0;
    SRWLOCK lock = SRWLOCK_INIT;
  };

  inline Ring& R() { static Ring r; return r; }
  inline Config& Cfg() { static Config c; return c; }
  inline std::atomic<int>& ModeRef() { static std::atomic<int> m(0); return m; }
  inline std::atomic<unsigned long>& PushCount() { static std::atomic<unsigned long> n(0); return n; }

  inline int Mode() { return ModeRef().load(std::memory_order_relaxed); }

  inline void Push(double t, const float* viewM, const float* trackedM) {
    Sample smp;
    smp.t = t;
    smp.view = PoseFromMatrix(viewM);
    smp.tracked = PoseFromMatrix(trackedM);
    Ring& r = R();
    AcquireSRWLockExclusive(&r.lock);
    r.s[r.head] = smp;
    r.head = (r.head + 1) % Ring::N;
    if (r.count < Ring::N) ++r.count;
    ReleaseSRWLockExclusive(&r.lock);
    PushCount().fetch_add(1, std::memory_order_relaxed);
  }

  // Seconds since the newest sample (large when there is none).
  inline double SinceLastPush(double t) {
    Ring& r = R();
    AcquireSRWLockShared(&r.lock);
    double since = r.count > 0 ? t - r.s[(r.head - 1 + Ring::N) % Ring::N].t : 1e9;
    ReleaseSRWLockShared(&r.lock);
    return since;
  }

  inline void Clear() {
    Ring& r = R();
    AcquireSRWLockExclusive(&r.lock);
    r.head = 0; r.count = 0;
    ReleaseSRWLockExclusive(&r.lock);
  }

  // Copies the ring oldest-first; returns the count.
  inline int Snapshot(Sample* out) {
    Ring& r = R();
    AcquireSRWLockShared(&r.lock);
    int n = r.count;
    int start = (r.head - n + Ring::N) % Ring::N;
    for (int i = 0; i < n; ++i) out[i] = r.s[(start + i) % Ring::N];
    ReleaseSRWLockShared(&r.lock);
    return n;
  }

  // Mean spacing of the most recent poses, seconds (0 when unknown).
  inline double PosePeriod(const Sample* s, int n) {
    int k = n < 16 ? n : 16;
    if (k < 2) return 0.0;
    double span = s[n - 1].t - s[n - k].t;
    return span > 0.0 ? span / (double)(k - 1) : 0.0;
  }

  // The pure sampling rule, testable without the ring.
  //   mode 1: interpolate at t - delay;   mode 2: interpolate or extrapolate at t.
  // Returns false when there is nothing to sample. *age = t minus the pose time used.
  inline bool SampleFrom(const Sample* s, int n, double t, int mode, double delay,
                         double maxPredict, Sample* out, double* age) {
    if (n <= 0) return false;
    if (n == 1 || mode == 0) { *out = s[n - 1]; *age = t - s[n - 1].t; return true; }

    double target = mode == 1 ? t - delay : t;
    const Sample& newest = s[n - 1];

    if (target >= newest.t) {
      if (mode == 2) {
        // Velocity over the last three pose steps, not one: a single step's
        // timestamps jitter by a few percent of the step, and extrapolation
        // would multiply that into visible unevenness.
        int back = n - 1 < 3 ? n - 1 : 3;
        const Sample& prev = s[n - 1 - back];
        double span = newest.t - prev.t;
        double ahead = target - newest.t;
        if (ahead > maxPredict) ahead = maxPredict;
        // A jump in that window is a tracking reset or hop, not motion: predicting
        // through it would overshoot by ~40 % for a few frames. Hold instead.
        // Judged as SPEED on the tracked pose (real metres, before RoomScale's
        // amplification), so a brisk walk or a quick look is never mistaken for one
        // whatever the window's length: no head moves 5 m/s or turns 1000 deg/s.
        float dx = newest.tracked.p[0] - prev.tracked.p[0], dy = newest.tracked.p[1] - prev.tracked.p[1],
              dz = newest.tracked.p[2] - prev.tracked.p[2];
        float sp = span > 1e-4 ? (float)span : 1e-4f;
        bool jump = sqrtf(dx * dx + dy * dy + dz * dz) / sp > 5.0f ||
                    AngleBetween(prev.tracked.q, newest.tracked.q) / sp > 1000.0f * 0.0174533f;
        if (!jump && span > 0.001 && span < 0.1 * back) {
          float u = (float)(1.0 + ahead / span);
          out->t = newest.t + ahead;
          out->view = Blend(prev.view, newest.view, u);
          out->tracked = Blend(prev.tracked, newest.tracked, u);
          *age = t - out->t;
          return true;
        }
      }
      *out = newest;                       // hold the newest pose
      *age = t - newest.t;
      return true;
    }

    if (target <= s[0].t) { *out = s[0]; *age = t - s[0].t; return true; }

    for (int i = n - 1; i > 0; --i) {
      if (s[i - 1].t <= target) {
        double span = s[i].t - s[i - 1].t;
        float u = span > 0.0 ? (float)((target - s[i - 1].t) / span) : 1.0f;
        out->t = target;
        out->view = Blend(s[i - 1].view, s[i].view, u);
        out->tracked = Blend(s[i - 1].tracked, s[i].tracked, u);
        *age = t - target;
        return true;
      }
    }
    *out = newest;
    *age = t - newest.t;
    return true;
  }

  inline double DelayFor(const Sample* s, int n) {
    const Config& c = Cfg();
    if (c.interpDelayMs > 0.0f) return c.interpDelayMs / 1000.0;
    double period = PosePeriod(s, n);
    if (period <= 0.0) period = 1.0 / 60.0;
    double d = period * 1.25;              // a little over one period, so jitter keeps the target bracketed
    if (d < 0.004) d = 0.004;
    if (d > 0.040) d = 0.040;
    return d;
  }

  // Game thread: the sample for this game frame.
  inline bool SampleNow(int mode, double t, Sample* out, double* age) {
    static Sample snap[Ring::N];
    int n = Snapshot(snap);
    return SampleFrom(snap, n, t, mode, DelayFor(snap, n), Cfg().maxPredictMs / 1000.0, out, age);
  }

  // ------------------------------------------------------- tracking state --
  // Frozen = the raw pose is bitwise unchanged for > 250 ms while the driver
  // says Running_OK (some phone-based drivers do this during a brief
  // disconnect, without reporting an error).

  struct TrackInfo {
    float  last[12] = { 0 };
    bool   have = false;
    double sameSince = 0.0;
    double lastNote = 0.0;
    std::atomic<int> state{ TRACK_NONE };
  };
  inline TrackInfo& T() { static TrackInfo t; return t; }

  // Returns true when the raw pose is bitwise the same as the previous one (the
  // driver delivered nothing new). Logs state changes at most every 2 s, with
  // a note about the changes it skipped, so flickering tracking can't flood the log.
  inline bool NoteRaw(double t, const float* raw16, bool runningOk) {
    TrackInfo& ti = T();
    float cur[12] = { raw16[0], raw16[1], raw16[2], raw16[4], raw16[5], raw16[6],
                      raw16[8], raw16[9], raw16[10], raw16[12], raw16[13], raw16[14] };
    bool same = ti.have && memcmp(cur, ti.last, sizeof(cur)) == 0;
    if (!same) { memcpy(ti.last, cur, sizeof(cur)); ti.sameSince = t; ti.have = true; }
    ti.lastNote = t;
    int st = !runningOk ? TRACK_LOST : (t - ti.sameSince > 0.25 ? TRACK_FROZEN : TRACK_OK);
    int prev = ti.state.exchange(st);
    static double lastLog = -1e9;
    static bool pending = false;   // a change was held back: log the state it ended in once 2 s have passed
    static const char* const names[4] = { "none", "ok", "lost", "frozen" };
    if (prev != st && prev != TRACK_NONE) {
      if (t - lastLog >= 2.0) {
        LOGSTRF("PoseTiming: tracking state %s -> %s\n", names[prev], names[st]);
        lastLog = t;
        pending = false;
      } else {
        pending = true;
      }
    } else if (pending && t - lastLog >= 2.0) {
      LOGSTRF("PoseTiming: tracking state now %s (changes in the last 2 s not logged one by one)\n", names[st]);
      lastLog = t;
      pending = false;
    }
    return same;
  }

  inline int State(double now) {
    TrackInfo& ti = T();
    int st = ti.state.load();
    if (st != TRACK_NONE && now - ti.lastNote > 0.25) return TRACK_LOST;   // no poses at all
    return st;
  }

  // --------------------------------------------------------------- config --

  inline void IniPath(char* out, size_t n) {
    char mod[MAX_PATH] = { 0 };
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&IniPath, &self);
    GetModuleFileNameA(self, mod, MAX_PATH);
    char* slash = strrchr(mod, '\\');
    if (slash) *(slash + 1) = '\0';
    _snprintf_s(out, n, _TRUNCATE, "%sgtaovr\\stabilizer.ini", mod);
  }

  inline float ReadF(const char* ini, const char* key, float def) {
    char buf[64] = { 0 }, defs[32];
    _snprintf_s(defs, sizeof(defs), _TRUNCATE, "%g", def);
    GetPrivateProfileStringA("Timing", key, defs, buf, sizeof(buf), ini);
    return buf[0] ? (float)atof(buf) : def;   // present but empty -> the default
  }

  // Writes the key only when it is ABSENT. A key present with an empty value is
  // left alone (reads fall back to the default), so nothing is rewritten every
  // second - each write would make the stabilizer reload the file.
  inline void EnsureKey(const char* ini, const char* key, const char* def) {
    char probe[32] = { 0 };
    GetPrivateProfileStringA("Timing", key, "\x01", probe, sizeof(probe), ini);
    if (probe[0] == '\x01' && probe[1] == '\0')
      WritePrivateProfileStringA("Timing", key, def, ini);
  }

  // HMD thread, once a second. Missing keys are written with their defaults
  // (never overwriting a set value), like RoomScale does.
  inline void Reload() {
    static ULONGLONG lastRead = 0;
    static bool loaded = false;
    ULONGLONG now = GetTickCount64();
    if (lastRead && now - lastRead < 1000) return;
    lastRead = now;

    char ini[MAX_PATH];
    IniPath(ini, sizeof(ini));
    EnsureKey(ini, "PoseTiming", "0");
    EnsureKey(ini, "InterpDelayMs", "0");
    EnsureKey(ini, "MaxPredictMs", "20");
    EnsureKey(ini, "TraceFrames", "0");

    Config c;
    c.mode = (int)ReadF(ini, "PoseTiming", 0.0f);
    if (c.mode < 0 || c.mode > 2) c.mode = 0;
    c.interpDelayMs = ReadF(ini, "InterpDelayMs", 0.0f);
    if (c.interpDelayMs < 0.0f) c.interpDelayMs = 0.0f;
    if (c.interpDelayMs > 100.0f) c.interpDelayMs = 100.0f;
    c.maxPredictMs = ReadF(ini, "MaxPredictMs", 20.0f);
    if (c.maxPredictMs < 0.0f) c.maxPredictMs = 0.0f;
    if (c.maxPredictMs > 50.0f) c.maxPredictMs = 50.0f;
    c.trace = ReadF(ini, "TraceFrames", 0.0f) != 0.0f;

    Config& cur = Cfg();
    bool changed = !loaded || c.mode != cur.mode || c.interpDelayMs != cur.interpDelayMs ||
                   c.maxPredictMs != cur.maxPredictMs || c.trace != cur.trace;
    cur = c;
    ModeRef().store(c.mode, std::memory_order_relaxed);
    if (changed) {
      static const char* const names[3] = { "0 (wait for next pose)", "1 (interpolate at game frame time)",
                                            "2 (extrapolate to game frame time)" };
      LOGSTRF("PoseTiming: PoseTiming=%s InterpDelayMs=%.1f%s MaxPredictMs=%.1f TraceFrames=%d\n",
              names[c.mode], c.interpDelayMs, c.interpDelayMs > 0.0f ? "" : " (auto)", c.maxPredictMs, c.trace ? 1 : 0);
    }
    loaded = true;
  }

  // ---------------------------------------------------------------- trace --
  // Game thread, once per SyncOnPoses: dt between calls, how many new HMD
  // poses arrived since the last call, how long the call waited, sample age.

  struct TraceAcc {
    double lastT = 0.0;
    unsigned long lastPush = 0;
    int frames = 0;
    double dtSum = 0.0, dtMin = 1e9, dtMax = 0.0;
    double waitSum = 0.0, waitMax = 0.0;
    double ageSum = 0.0, ageMax = 0.0;
    int newPoses[4] = { 0, 0, 0, 0 };   // 0, 1, 2, 3+
  };

  inline void GameTick(double t, double waitSec, double ageSec) {
    if (!Cfg().trace) return;
    static TraceAcc a;
    unsigned long pushes = PushCount().load(std::memory_order_relaxed);
    if (a.lastT > 0.0) {
      double dt = t - a.lastT;
      if (dt > 0.0 && dt < 0.5) {
        a.frames++;
        a.dtSum += dt; if (dt < a.dtMin) a.dtMin = dt; if (dt > a.dtMax) a.dtMax = dt;
        a.waitSum += waitSec; if (waitSec > a.waitMax) a.waitMax = waitSec;
        a.ageSum += ageSec; if (ageSec > a.ageMax) a.ageMax = ageSec;
        unsigned long k = pushes - a.lastPush;
        a.newPoses[k > 3 ? 3 : k]++;
      }
    }
    a.lastT = t;
    a.lastPush = pushes;
    if (a.frames >= 300) {
      double avg = a.dtSum / a.frames;
      LOGSTRF("PoseTiming trace: mode=%d %d game frames, %.1f fps, frame %.1f/%.1f/%.1f ms (min/avg/max), "
              "new HMD poses per frame 0:%d 1:%d 2:%d 3+:%d, wait %.2f/%.2f ms (avg/max), pose age %.1f/%.1f ms (avg/max)\n",
              Mode(), a.frames, 1.0 / avg, a.dtMin * 1000.0, avg * 1000.0, a.dtMax * 1000.0,
              a.newPoses[0], a.newPoses[1], a.newPoses[2], a.newPoses[3],
              a.waitSum / a.frames * 1000.0, a.waitMax * 1000.0, a.ageSum / a.frames * 1000.0, a.ageMax * 1000.0);
      double lastT = a.lastT;
      unsigned long lastPush = a.lastPush;
      a = TraceAcc();
      a.lastT = lastT;
      a.lastPush = lastPush;
    }
  }

}  // namespace PoseTiming
