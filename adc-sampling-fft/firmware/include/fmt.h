// fmt.h - minimal, header-only "{}"-style formatted logging over Serial.
//
// This is a tiny, dependency-free stand-in for printf-style logging that
// avoids pulling in the full C library's variadic printf (which is heavier
// on a resource-constrained MCU and does not type-check arguments). Usage:
//
//     logln("boot FS={} Hz, MAX_N={}, ok={}", FS, MAX_N, ok ? 1 : 0);
//
// Each "{}" in the format string is replaced, in order, by the next
// argument via Serial.print(), so it works for any type Serial.print()
// already understands (int, uint32_t, float, const char*, etc.).
//
// Not part of the original baseline archive (sampling.cpp depends on it
// but it was not included in the uploaded ZIP); added here so the firmware
// in this repository compiles standalone.

#pragma once
#include <Arduino.h>

namespace fmtdetail {

// Base case: no more arguments - print the remainder of the format string
// verbatim (any stray "{}" left over is printed literally).
inline void logImpl(const char *fmt) {
  while (*fmt) {
    Serial.write(*fmt++);
  }
}

// Recursive case: consume one argument for the next "{}" placeholder.
template <typename T, typename... Rest>
inline void logImpl(const char *fmt, T value, Rest... rest) {
  while (*fmt) {
    if (fmt[0] == '{' && fmt[1] == '}') {
      Serial.print(value);
      logImpl(fmt + 2, rest...);
      return;
    }
    Serial.write(*fmt++);
  }
}

} // namespace fmtdetail

// Formats `fmt` with `args...` (substituting "{}" placeholders in order)
// and writes the result to Serial, followed by a newline.
template <typename... Args> inline void logln(const char *fmt, Args... args) {
  fmtdetail::logImpl(fmt, args...);
  Serial.write('\n');
  Serial.flush();
}
