#pragma once

// Text format for key binds, shared by OVRInject (stabilizer.ini [Hotkeys]) and GTAVive (gtavive.ini [Keys]).
//
// A bind is optional modifiers and one key joined with '+', case-insensitive, spaces ignored:
//   Ctrl+Shift+F8    Alt+Home    K    Ctrl+0x77
// Modifiers: Ctrl (or Control), Shift, Alt. Keys: A-Z, 0-9, F1-F24, Numpad0-Numpad9 and the names in kHotkeyNames,
// or a raw virtual-key code (decimal, or hex like 0x77). Matching is exact: a bind without modifiers does not fire
// while Ctrl, Shift or Alt is held.
// "None" (any case) or an empty value means the function has no key: the bind parses to vk 0 and never fires.

#include <Windows.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace OVRInject
{
  struct HotkeyBind {
    int vk;
    bool ctrl;
    bool shift;
    bool alt;
  };

  struct HotkeyName {
    const char* name;
    int vk;
  };

  // Display spelling first; the parser compares case-insensitively. When a key has two names, the first one is
  // the one written back.
  static const HotkeyName kHotkeyNames[] = {
    { "Home", VK_HOME }, { "End", VK_END },
    { "Insert", VK_INSERT }, { "Delete", VK_DELETE },
    { "PageUp", VK_PRIOR }, { "PageDown", VK_NEXT },
    { "PgUp", VK_PRIOR }, { "PgDn", VK_NEXT },
    { "Up", VK_UP }, { "Down", VK_DOWN },
    { "Left", VK_LEFT }, { "Right", VK_RIGHT },
    { "Space", VK_SPACE }, { "Tab", VK_TAB },
    { "Enter", VK_RETURN }, { "Return", VK_RETURN },
    { "Esc", VK_ESCAPE }, { "Escape", VK_ESCAPE },
    { "Backspace", VK_BACK }, { "Pause", VK_PAUSE },
    { "NumLock", VK_NUMLOCK }, { "ScrollLock", VK_SCROLL },
    { "NumpadAdd", VK_ADD }, { "NumpadSub", VK_SUBTRACT },
    { "NumpadMul", VK_MULTIPLY }, { "NumpadDiv", VK_DIVIDE },
    { "NumpadDot", VK_DECIMAL },
    { "Comma", VK_OEM_COMMA }, { "Period", VK_OEM_PERIOD },
    { "Slash", VK_OEM_2 }, { "Semicolon", VK_OEM_1 },
    { "Quote", VK_OEM_7 }, { "LBracket", VK_OEM_4 },
    { "RBracket", VK_OEM_6 }, { "Backslash", VK_OEM_5 },
    { "Minus", VK_OEM_MINUS }, { "Equals", VK_OEM_PLUS },
    { "Grave", VK_OEM_3 }, { "Tilde", VK_OEM_3 },
  };

  // Maps one key-name token (no spaces) to a virtual-key code; 0 if unknown.
  inline int HotkeyNameToVk(const char* name) {
    size_t len = strlen(name);
    if (len == 0)
      return 0;

    if (len == 1) {
      char c = (char)tolower((unsigned char)name[0]);
      if (c >= 'a' && c <= 'z') return 'A' + (c - 'a');
      if (c >= '0' && c <= '9') return c;
    }

    if ((name[0] == 'f' || name[0] == 'F') && len >= 2 && len <= 3 &&
        isdigit((unsigned char)name[1]) && (len == 2 || isdigit((unsigned char)name[2]))) {
      int n = atoi(name + 1);
      if (n >= 1 && n <= 24)
        return VK_F1 + (n - 1);
    }

    if (len == 7 && _strnicmp(name, "numpad", 6) == 0 && isdigit((unsigned char)name[6]))
      return VK_NUMPAD0 + (name[6] - '0');

    for (size_t i = 0; i < sizeof(kHotkeyNames) / sizeof(kHotkeyNames[0]); ++i) {
      if (_stricmp(name, kHotkeyNames[i].name) == 0)
        return kHotkeyNames[i].vk;
    }

    if (name[0] == '0' && len > 2 && (name[1] == 'x' || name[1] == 'X')) {
      int vk = (int)strtol(name, nullptr, 16);
      return (vk > 0 && vk < 256) ? vk : 0;
    }

    for (size_t i = 0; i < len; ++i) {
      if (!isdigit((unsigned char)name[i]))
        return 0;
    }
    int vk = atoi(name);
    return (vk > 0 && vk < 256) ? vk : 0;
  }

  // True for an unbound bind (vk 0), written as "None".
  inline bool HotkeyIsNone(const HotkeyBind& bind) {
    return bind.vk <= 0;
  }

  // True when the text is empty, only spaces, or "None" (any case, spaces ignored).
  inline bool HotkeyIsNoneText(const char* str) {
    char token[8];
    int len = 0;
    for (const char* p = str; *p != '\0'; ++p) {
      if (*p == ' ' || *p == '\t')
        continue;
      if (len >= (int)sizeof(token) - 1)
        return false;
      token[len++] = *p;
    }
    token[len] = '\0';
    return len == 0 || _stricmp(token, "none") == 0;
  }

  // Parses a bind string. "None" or an empty string gives the unbound bind (vk 0) and returns true. Returns false
  // (and leaves out untouched) when the string names no usable key.
  inline bool HotkeyParse(const char* str, HotkeyBind& out) {
    if (HotkeyIsNoneText(str)) {
      HotkeyBind none = { 0, false, false, false };
      out = none;
      return true;
    }

    HotkeyBind bind = { 0, false, false, false };
    char token[64];
    int token_len = 0;

    for (const char* p = str; ; ++p) {
      char c = *p;
      if (c != '\0' && c != '+') {
        if (c != ' ' && c != '\t' && token_len < (int)sizeof(token) - 1)
          token[token_len++] = c;
        continue;
      }

      token[token_len] = '\0';
      if (token_len > 0) {
        if (_stricmp(token, "ctrl") == 0 || _stricmp(token, "control") == 0)
          bind.ctrl = true;
        else if (_stricmp(token, "shift") == 0)
          bind.shift = true;
        else if (_stricmp(token, "alt") == 0)
          bind.alt = true;
        else
          bind.vk = HotkeyNameToVk(token);
      }
      token_len = 0;

      if (c == '\0')
        break;
    }

    if (bind.vk == 0)
      return false;
    out = bind;
    return true;
  }

  // Writes the display / ini spelling of one virtual-key code ("F8", "Home", "0x5D").
  inline void HotkeyVkName(int vk, char* out, size_t out_size) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
      _snprintf_s(out, out_size, _TRUNCATE, "%c", (char)vk);
      return;
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
      _snprintf_s(out, out_size, _TRUNCATE, "F%d", vk - VK_F1 + 1);
      return;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
      _snprintf_s(out, out_size, _TRUNCATE, "Numpad%d", vk - VK_NUMPAD0);
      return;
    }
    for (size_t i = 0; i < sizeof(kHotkeyNames) / sizeof(kHotkeyNames[0]); ++i) {
      if (kHotkeyNames[i].vk == vk) {
        _snprintf_s(out, out_size, _TRUNCATE, "%s", kHotkeyNames[i].name);
        return;
      }
    }
    _snprintf_s(out, out_size, _TRUNCATE, "0x%02X", vk);
  }

  // Writes a bind in the format HotkeyParse reads ("Ctrl+Shift+F8", or "None" when unbound).
  inline void HotkeyFormat(const HotkeyBind& bind, char* out, size_t out_size) {
    if (HotkeyIsNone(bind)) {
      _snprintf_s(out, out_size, _TRUNCATE, "None");
      return;
    }
    char key[32];
    HotkeyVkName(bind.vk, key, sizeof(key));
    _snprintf_s(out, out_size, _TRUNCATE, "%s%s%s%s",
                bind.ctrl ? "Ctrl+" : "", bind.shift ? "Shift+" : "", bind.alt ? "Alt+" : "", key);
  }

  inline bool HotkeyEqual(const HotkeyBind& a, const HotkeyBind& b) {
    return a.vk == b.vk && a.ctrl == b.ctrl && a.shift == b.shift && a.alt == b.alt;
  }

  // True when the physically held modifiers are exactly the bind's modifiers.
  inline bool HotkeyModifiersMatch(const HotkeyBind& bind) {
    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    return ctrl == bind.ctrl && shift == bind.shift && alt == bind.alt;
  }
}
