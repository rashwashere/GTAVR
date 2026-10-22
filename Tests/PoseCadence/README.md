# PoseCadence

Standalone OpenVR (0.9.20, IVRSystem_012) recorder that measures how the HMD driver delivers poses: rate, gaps, frozen stretches and invalid periods. No game involved.

Run: start SteamVR first (this tool never starts it), run `build.bat`, then from this folder `bin\posecadence.exe [seconds]` (default 180).
Copy `openvr_api.dll` (the one shipped with the mod) into `bin\` next to the exe.

Output goes to `out\`: `hmd_pose_<stamp>.csv` (one row per new pose) and `summary_<stamp>.txt`.

CSV columns: t_s (seconds since start, QPC), px,py,pz (metres, standing universe), qx,qy,qz,qw (normalized rotation quaternion),
valid (bPoseIsValid), result (eTrackingResult, 200 = Running_OK), dt_ms (ms since previous new pose; 0 on first row).
A "new pose" is a bitwise change of the 3x4 matrix. A "frozen" stretch is an identical pose for more than 250 ms while valid and result 200.
