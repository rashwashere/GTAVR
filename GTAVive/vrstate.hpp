#pragma once
// Read-only state block for the VRCam companion script: whether GTAVive's VR camera is rendering and whether
// GTAVive's settings menu is open. GTAVive is the only writer; readers never write.
//
//   name    Local\gtavive_cam_v1, 32 bytes
//   +0   u32  magic        'VRC1' = 0x31435256, written last when the block is created
//   +4   u32  version      1
//   +8   u32  pid          the game's process id (a reader requires its own)
//   +12  u32  armed        1 while the VR camera is rendering, else 0
//   +16  u64  heartbeatMs  GetTickCount64() at the last script tick
//   +24  u32  menuOpen     1 while the settings menu is open, else 0
//   +28  u32  reserved     0
//
// A reader treats the block as valid only when magic, version and pid match and the heartbeat is under
// 2 seconds old, so a stopped script never reads as "armed" or "menu open".

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
    uint32_t reserved;
  };
#pragma pack(pop)
  static_assert(sizeof(Block) == 32, "gtavive_cam_v1 layout is fixed");

  // Call once per script tick, with VR on or off. The block is created on first use; if that fails the
  // call does nothing and readers simply find no block.
  inline void Publish(bool armed, bool menuOpen) {
    static Block* p = nullptr;
    static bool tried = false;
    if (p == nullptr) {
      if (tried) return;
      tried = true;
      HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Block),
                                    "Local\\gtavive_cam_v1");
      if (h == nullptr) return;
      p = static_cast<Block*>(MapViewOfFile(h, FILE_MAP_WRITE, 0, 0, sizeof(Block)));
      if (p == nullptr) { CloseHandle(h); return; }
      p->version = 1;
      p->pid = GetCurrentProcessId();
      p->armed = 0;
      p->heartbeatMs = GetTickCount64();
      p->menuOpen = 0;
      p->reserved = 0;
      MemoryBarrier();
      p->magic = 0x31435256;
    }
    p->armed = armed ? 1u : 0u;
    p->menuOpen = menuOpen ? 1u : 0u;
    p->heartbeatMs = GetTickCount64();
  }

}  // namespace VrState
