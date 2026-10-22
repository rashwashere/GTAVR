#pragma once

#include "Math/Matrices.h"
#include "Hotkey.hpp"

namespace OVRInject
{
  /**
  Smooths the raw HMD pose (device-to-absolute) before it reaches the
  renderer and the game camera. Designed for IMU-tracked headsets (for
  example phone-based ones) where sensor jitter makes the camera shaky.

  Presets:
    Jitter   - tuned uniform smoothing that removes IMU jitter.
    Manual   - independent strength per axis (pitch/yaw/roll rotation,
               vertical/lateral/depth position, in the facing frame).
    Adaptive - one-euro filter: glassy when still, fast on big moves.

  Configured through gtaovr/stabilizer.ini next to OVRInject.dll, and
  tunable live ingame: cycle preset / cycle target setting / adjust
  up-down (with key repeat) / save back to the ini. Feedback goes to
  the game window title and pitch-coded beeps.
  */
  class PoseStabilizer {
  public:
    enum Preset {
      PRESET_JITTER = 0,
      PRESET_MANUAL = 1,
      PRESET_ADAPTIVE = 2,
    };

    PoseStabilizer();

    /**
    Loads (or creates) gtaovr/stabilizer.ini. Safe to call again at
    runtime to re-read tuning values.
    */
    void LoadConfig();

    /**
    Writes the current (possibly live-adjusted) settings back to the ini.
    */
    void SaveConfig();

    /**
    Polls the configured hotkeys. Call once per tracking update.
    */
    void HandleHotkeys();

    /**
    Filters a raw device-to-absolute HMD pose. Returns the input
    unchanged when disabled.
    */
    Matrix4 Filter(const Matrix4& raw_pose);

  private:
    struct Quat {
      float x, y, z, w;
    };

    /**
    Edge/repeat state for one hotkey
    */
    struct BindState {
      bool was_down;
      double next_repeat;
    };

    static Quat MatrixToQuat(const Matrix4& m);
    static void QuatToMatrix(const Quat& q, Matrix4& m);
    static Quat Nlerp(const Quat& a, const Quat& b, float t);

    void ResetState(const Matrix4& raw_pose);

    void FilterUniform(const float raw_pos[3], const Quat& raw_quat, float dt);
    void FilterPerAxis(const float raw_pos[3], const Matrix4& raw_pose, float dt);

    // Auto-reload: re-read the ini when its file changes on disk (e.g.
    // edited in a text editor while playing). Polled at most twice a second.
    void RememberIniMtime();
    void CheckConfigFileChanged(double now);

    void CyclePreset();
    void CycleTarget();
    void Adjust(int direction);
    int TargetCount() const;
    const char* TargetName(int target) const;
    float NormalizedTargetValue() const;
    void PublishFeedback(bool pitch_beep);

    // Config
    bool enabled_;

    // Split switches: smooth the physical movement (position, 3DOF) and/or
    // the looking-around (rotation). Off = that half passes through raw, so
    // a hand-held look keeps its natural turns while the up/down bounce of
    // the hands is taken out.
    bool stabilize_position_;
    bool stabilize_rotation_;
    int preset_;

    // Jitter preset
    float pos_smooth_ms_;
    float rot_smooth_ms_;

    // Manual preset: per-axis strengths, 0 (raw) .. 100 (MaxSmoothMs)
    float pitch_strength_;
    float yaw_strength_;
    float roll_strength_;
    float vertical_strength_;
    float lateral_strength_;
    float depth_strength_;
    float max_smooth_ms_;

    // Adaptive preset (one-euro)
    float pos_min_cutoff_;
    float pos_beta_;
    float rot_min_cutoff_;
    float rot_beta_;

    HotkeyBind toggle_bind_;
    HotkeyBind reload_bind_;
    HotkeyBind preset_bind_;
    HotkeyBind target_bind_;
    HotkeyBind up_bind_;
    HotkeyBind down_bind_;
    HotkeyBind save_bind_;

    BindState toggle_state_;
    BindState reload_state_;
    BindState preset_state_;
    BindState target_state_;
    BindState up_state_;
    BindState down_state_;
    BindState save_state_;

    // Which setting the Up/Down keys adjust (0 = All; meaning depends
    // on the active preset)
    int adjust_target_;

    // Auto-reload state
    FILETIME ini_mtime_;
    double next_mtime_check_;

    // Filter state
    bool has_state_;
    float fpos_[3];
    Quat fquat_;
    float fyaw_, fpitch_, froll_;
    Quat prev_raw_quat_;
    float prev_raw_pos_[3];
    float dpos_lp_[3];       // low-passed position derivative (one-euro)
    double last_time_;
  };

  extern PoseStabilizer g_pose_stabilizer;
}
