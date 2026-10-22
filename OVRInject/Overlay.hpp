#pragma once

struct IDXGISwapChain;

namespace OVRInject
{
  /**
  Minimal on-screen text overlay, drawn onto the game backbuffer inside
  the Present hook - before the frame is mirrored to VR, so the text is
  visible on the monitor AND inside the headset view.

  Thread-safe: Overlay_Notify may be called from any thread (the pose
  thread calls it); Overlay_Render runs on the game's render thread.
  */

  // Enables/disables the overlay and sets how long a message stays up
  void Overlay_Configure(bool enabled, float duration_seconds);

  // Shows `text` for the configured duration (replaces prior text)
  void Overlay_Notify(const char* text);

  // Draws the overlay if a message is active. Called from hookedPresent.
  void Overlay_Render(IDXGISwapChain* swap_chain);
}
