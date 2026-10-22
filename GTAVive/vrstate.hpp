#pragma once
// Shared state between GTAVive and the VRCam companion script, which run in the same game process. Each side
// writes only its own block and only reads the other one.
//
// Local\gtavive_cam_v1, 32 bytes, written by GTAVive: whether the VR camera is rendering and whether GTAVive's
// settings menu is open.
//   +0   u32  magic        'VRC1' = 0x31435256, written last when the block is created
//   +4   u32  version      1
//   +8   u32  pid          the game's process id (a reader requires its own)
//   +12  u32  armed        1 while the VR camera is rendering, else 0
//   +16  u64  heartbeatMs  GetTickCount64() at the last script tick
//   +24  u32  menuOpen     1 while the settings menu is open, else 0
//   +28  u16  link         1 = GTAVive opens its settings menu when VRCam asks (vrcam_state_v1 +24); 0 in builds
//                          without the menu link (the field was reserved and 0 there)
//   +30  u16  backToVrcam  incremented each time the settings menu, opened from VRCam, is left with Back / Esc on
//                          its main page; VRCam then opens its own menu again
//
// Local\vrcam_state_v1, 32 bytes, written by VRCam: that VRCam runs, which key opens its menu, and requests.
//   +0   u32  magic        'VCS1' = 0x31534356, written last when the block is created
//   +4   u32  version      1
//   +8   u32  pid          the game's process id
//   +12  u32  menuKey      VRCam's OpenMenu key: virtual-key code in bits 0-7, bit 8 Ctrl, bit 9 Shift, bit 10 Alt
//   +16  u64  heartbeatMs  GetTickCount64() at VRCam's last menu tick
//   +24  u32  openGtavr    incremented each time VRCam's "GTAVR Settings" row is selected; GTAVive opens its menu
//   +28  u32  menuOpen     1 while VRCam's menu or its text input box is open, else 0
//
// A reader treats a block as valid only when magic, version and pid match and the heartbeat is under 2 seconds old,
// so a stopped script never reads as running, "armed" or "menu open". The counters are compared with the last value
// seen; when a block becomes valid its current counter is taken as seen, so only changes made while both sides run
// act.

#include <Windows.h>
#include <cstdint>

namespace VrState {

#pragma pack(push, 1)
  struct Block {
    uint32_t magic;
    uint32_t version;
    uint32_t pid;
    uint32_t armed;
    uint64_t heartbeatMs;
    uint32_t menuOpen;
    uint16_t link;
    uint16_t backToVrcam;
  };

  struct VrcamBlock {
    uint32_t magic;
    uint32_t version;
    uint32_t pid;
    uint32_t menuKey;
    uint64_t heartbeatMs;
    uint32_t openGtavr;
    uint32_t menuOpen;
  };
#pragma pack(pop)
  static_assert(sizeof(Block) == 32, "gtavive_cam_v1 layout is fixed");
  static_assert(sizeof(VrcamBlock) == 32, "vrcam_state_v1 layout is fixed");

  const uint32_t kVrcamMagic = 0x31534356;   // 'VCS1'
  const uint32_t kVrcamKeyCtrl = 0x100, kVrcamKeyShift = 0x200, kVrcamKeyAlt = 0x400;

  // The backToVrcam counter published by the next Publish() call.
  inline uint16_t& BackToVrcamCounter() {
    static uint16_t counter = 0;
    return counter;
  }

  // Asks VRCam to open its menu again (the settings menu was opened from VRCam and is left with Back / Esc).
  inline void SignalBackToVrcam() {
    ++BackToVrcamCounter();
  }

  // Call once per script tick, with VR on or off. The block is created on first use; if that fails the
  // call does nothing and readers simply find no block.
  inline void Publish(bool armed, bool menuOpen) {
    static volatile Block* p = nullptr;
    static bool tried = false;
    if (p == nullptr) {
      if (tried) return;
      tried = true;
      HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Block),
                                    "Local\\gtavive_cam_v1");
      if (h == nullptr) return;
      p = static_cast<volatile Block*>(MapViewOfFile(h, FILE_MAP_WRITE, 0, 0, sizeof(Block)));
      if (p == nullptr) { CloseHandle(h); return; }
      p->version = 1;
      p->pid = GetCurrentProcessId();
      p->armed = 0;
      p->heartbeatMs = GetTickCount64();
      p->menuOpen = 0;
      p->link = 1;
      p->backToVrcam = BackToVrcamCounter();
      MemoryBarrier();
      p->magic = 0x31435256;
    }
    p->armed = armed ? 1u : 0u;
    p->menuOpen = menuOpen ? 1u : 0u;
    p->backToVrcam = BackToVrcamCounter();
    p->heartbeatMs = GetTickCount64();
  }

  struct VrcamState {
    bool present;         // VRCam runs in this process (valid block)
    uint32_t menuKey;     // see menuKey above; 0 when not present
    uint32_t openGtavr;
    bool menuOpen;
  };

  // VRCam's block, read once per call. Looked for at most once a second while VRCam has not created it.
  inline VrcamState ReadVrcam() {
    static const volatile VrcamBlock* p = nullptr;
    static ULONGLONG lastTry = 0;
    VrcamState s = { false, 0, 0, false };
    ULONGLONG now = GetTickCount64();
    if (p == nullptr) {
      if (lastTry != 0 && now - lastTry < 1000) return s;
      lastTry = now;
      HANDLE h = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\vrcam_state_v1");
      if (h == nullptr) return s;
      p = static_cast<const volatile VrcamBlock*>(MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(VrcamBlock)));
      CloseHandle(h);   // the view keeps the mapping alive, also across a VRCam reload
      if (p == nullptr) return s;
    }
    if (p->magic != kVrcamMagic || p->version != 1 || p->pid != GetCurrentProcessId())
      return s;
    ULONGLONG beat = p->heartbeatMs;
    if (now < beat || now - beat >= 2000)
      return s;
    s.present = true;
    s.menuKey = p->menuKey;
    s.openGtavr = p->openGtavr;
    s.menuOpen = p->menuOpen == 1;
    return s;
  }

}  // namespace VrState
