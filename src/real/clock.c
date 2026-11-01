#include "clock_manager.h"

#include <windows.h>

/**
 * @brief FILETIME (100ns since 1601) to epoch milliseconds UTC
 *
 * @param ft the time to convert
 * @return the moment in epoch milliseconds UTC, 0 when the time is zero
 */
static uint64_t filetime_to_ms(FILETIME const *const ft) {
  ULARGE_INTEGER u;

  u.LowPart = ft->dwLowDateTime;
  u.HighPart = ft->dwHighDateTime;
  if (u.QuadPart == 0) {
    return 0;
  }
  return (uint64_t)((u.QuadPart - 116444736000000000ULL) / 10000ULL);
}

static uint64_t now_ms(void) {
  FILETIME ft;

  GetSystemTimeAsFileTime(&ft);
  return filetime_to_ms(&ft);
}

/** @brief The clock of the machine, read from the system */
static struct clock_manager const kSystemClock = {
    .now_ms = now_ms,
};

struct clock_manager const *clock_manager_system(void) { return &kSystemClock; }
