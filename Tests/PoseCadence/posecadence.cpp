// Standalone OpenVR pose recorder: measures how the HMD driver delivers poses.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <mmsystem.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#include "../../ThirdParty/openvr/headers/openvr.h"
#pragma comment(lib, "winmm.lib")

static double nowS(LARGE_INTEGER f, LARGE_INTEGER s) {
    LARGE_INTEGER n; QueryPerformanceCounter(&n);
    return double(n.QuadPart - s.QuadPart) / double(f.QuadPart);
}
static void quat(const float m[3][4], double q[4]) {
    double tr = m[0][0] + m[1][1] + m[2][2], x, y, z, w;
    if (tr > 0) { double s = sqrt(tr + 1.0) * 2; w = 0.25 * s; x = (m[2][1] - m[1][2]) / s; y = (m[0][2] - m[2][0]) / s; z = (m[1][0] - m[0][1]) / s; }
    else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) { double s = sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2; w = (m[2][1] - m[1][2]) / s; x = 0.25 * s; y = (m[0][1] + m[1][0]) / s; z = (m[0][2] + m[2][0]) / s; }
    else if (m[1][1] > m[2][2]) { double s = sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2; w = (m[0][2] - m[2][0]) / s; x = (m[0][1] + m[1][0]) / s; y = 0.25 * s; z = (m[1][2] + m[2][1]) / s; }
    else { double s = sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2; w = (m[1][0] - m[0][1]) / s; x = (m[0][2] + m[2][0]) / s; y = (m[1][2] + m[2][1]) / s; z = 0.25 * s; }
    double n = sqrt(x * x + y * y + z * z + w * w); if (n == 0) n = 1;
    q[0] = x / n; q[1] = y / n; q[2] = z / n; q[3] = w / n;
}
static double pct(std::vector<double> v, double p) {
    if (v.empty()) return 0; std::sort(v.begin(), v.end());
    double r = p / 100.0 * (v.size() - 1); size_t i = (size_t)r; double f = r - i;
    return i + 1 < v.size() ? v[i] * (1 - f) + v[i + 1] * f : v[i];
}
struct Seg { double t, len; };
int main(int argc, char** argv) {
    double dur = argc > 1 ? atof(argv[1]) : 180.0; if (dur <= 0) dur = 180;
    vr::EVRInitError err = vr::VRInitError_None;
    vr::IVRSystem* vs = vr::VR_Init(&err, vr::VRApplication_Background);
    if (err != vr::VRInitError_None || !vs) { printf("VR_Init failed: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(err)); return 2; }
    timeBeginPeriod(1);
    SYSTEMTIME st; GetLocalTime(&st); char stamp[32];
    sprintf(stamp, "%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    CreateDirectoryA("out", NULL);
    std::string csvp = std::string("out\\hmd_pose_") + stamp + ".csv", sump = std::string("out\\summary_") + stamp + ".txt";
    FILE* csv = fopen(csvp.c_str(), "w");
    if (!csv) { printf("cannot open csv\n"); vr::VR_Shutdown(); return 3; }
    fprintf(csv, "t_s,px,py,pz,qx,qy,qz,qw,valid,result,dt_ms\n");
    LARGE_INTEGER f, s; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&s);
    static vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
    float prev[3][4]; bool have = false; double lastT = 0; bool lastGood = false;
    std::vector<double> dts; double sx = 0, sy = 0, sz = 0, sx2 = 0, sy2 = 0, sz2 = 0; long nv = 0;
    std::vector<Seg> frozen, bad; std::vector<std::string> badWhy;
    bool inBad = false; double badStart = 0; int badRes = 0; bool badValid = true;
    long polls = 0, total = 0;
    double t = 0;
    while ((t = nowS(f, s)) < dur) {
        vs->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0f, poses, vr::k_unMaxTrackedDeviceCount);
        polls++;
        const vr::TrackedDevicePose_t& p = poses[0];
        bool good = p.bPoseIsValid && p.eTrackingResult == vr::TrackingResult_Running_OK;
        if (!good && !inBad) { inBad = true; badStart = t; badRes = p.eTrackingResult; badValid = p.bPoseIsValid; }
        if (good && inBad) { inBad = false; bad.push_back({badStart, t - badStart}); char b[64]; sprintf(b, "valid=%d result=%d", badValid, badRes); badWhy.push_back(b); }
        float cur[3][4]; memcpy(cur, p.mDeviceToAbsoluteTracking.m, sizeof(cur));
        if (!have || memcmp(cur, prev, sizeof(cur)) != 0) {
            double dt = have ? (t - lastT) * 1000.0 : 0.0;
            if (have) {
                dts.push_back(dt);
                if (dt > 250.0 && lastGood) frozen.push_back({lastT, dt / 1000.0});
            }
            double q[4]; quat(cur, q);
            double px = cur[0][3], py = cur[1][3], pz = cur[2][3];
            fprintf(csv, "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%d,%d,%.3f\n", t, px, py, pz, q[0], q[1], q[2], q[3], p.bPoseIsValid ? 1 : 0, (int)p.eTrackingResult, dt);
            if (p.bPoseIsValid) { nv++; sx += px; sy += py; sz += pz; sx2 += px * px; sy2 += py * py; sz2 += pz * pz; }
            memcpy(prev, cur, sizeof(cur)); have = true; lastT = t; lastGood = good; total++;
        }
        Sleep(1);
    }
    double end = nowS(f, s);
    if (have && lastGood && (end - lastT) > 0.25) frozen.push_back({lastT, end - lastT});
    if (inBad) { bad.push_back({badStart, end - badStart}); char b[64]; sprintf(b, "valid=%d result=%d (until end)", badValid, badRes); badWhy.push_back(b); }
    fclose(csv);
    auto sd = [&](double a, double a2) { if (nv < 2) return 0.0; double m = a / nv; double v = a2 / nv - m * m; return sqrt(v < 0 ? 0 : v) * 1000.0; };
    std::string o; char b[512];
    sprintf(b, "PoseCadence summary (%s)\nduration_s=%.2f polls=%ld (%.0f polls/s)\n", stamp, end, polls, polls / end); o += b;
    sprintf(b, "total new poses: %ld\nmean rate: %.2f Hz\n", total, total / end); o += b;
    double mx = dts.empty() ? 0 : *std::max_element(dts.begin(), dts.end());
    sprintf(b, "dt_ms: p1=%.2f p10=%.2f p50=%.2f p90=%.2f p99=%.2f max=%.2f\n", pct(dts, 1), pct(dts, 10), pct(dts, 50), pct(dts, 90), pct(dts, 99), mx); o += b;
    long c25 = 0, c50 = 0, c250 = 0; for (double d : dts) { if (d > 25) c25++; if (d > 50) c50++; if (d > 250) c250++; }
    sprintf(b, "dt > 25 ms: %ld\ndt > 50 ms: %ld\ndt > 250 ms: %ld\n", c25, c50, c250); o += b;
    sprintf(b, "frozen stretches (>250 ms identical while valid & result 200): %zu\n", frozen.size()); o += b;
    for (auto& z : frozen) { sprintf(b, "  start t=%.3f s, length %.3f s\n", z.t, z.len); o += b; }
    sprintf(b, "invalid / non-200 periods: %zu\n", bad.size()); o += b;
    for (size_t i = 0; i < bad.size(); i++) { sprintf(b, "  start t=%.3f s, length %.3f s, %s\n", bad[i].t, bad[i].len, badWhy[i].c_str()); o += b; }
    sprintf(b, "position std over run (mm): x=%.4f y=%.4f z=%.4f (n=%ld valid poses)\n", sd(sx, sx2), sd(sy, sy2), sd(sz, sz2), nv); o += b;
    sprintf(b, "csv: %s\n", csvp.c_str()); o += b;
    fputs(o.c_str(), stdout);
    FILE* sf = fopen(sump.c_str(), "w"); if (sf) { fputs(o.c_str(), sf); fclose(sf); }
    vr::VR_Shutdown(); timeEndPeriod(1);
    return 0;
}
