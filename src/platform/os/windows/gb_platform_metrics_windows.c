#include "platform/os/gb_platform_metrics.h"

#include <string.h>
#include <sysinfoapi.h>

static unsigned long long filetime_to_ull(const FILETIME *ft) {
  return ((unsigned long long) ft->dwHighDateTime << 32) | ft->dwLowDateTime;
}

static double clamp_percent(double value) {
  if (value < 0.0) return 0.0;
  if (value > 100.0) return 100.0;
  return value;
}

static double cpu_usage_percent(void) {
  static unsigned long long prev_idle = 0;
  static unsigned long long prev_total = 0;
  static unsigned long long last_sample_ms = 0;
  static double cached_percent = 0.0;
  FILETIME idle_time;
  FILETIME kernel_time;
  FILETIME user_time;
  unsigned long long now_ms;
  unsigned long long idle;
  unsigned long long kernel;
  unsigned long long user;
  unsigned long long total;
  unsigned long long total_delta;
  unsigned long long idle_delta;

  now_ms = GetTickCount64();
  if (last_sample_ms != 0 && now_ms - last_sample_ms < 900ULL) return cached_percent;

  if (!GetSystemTimes(&idle_time, &kernel_time, &user_time)) return 0.0;
  idle = filetime_to_ull(&idle_time);
  kernel = filetime_to_ull(&kernel_time);
  user = filetime_to_ull(&user_time);
  total = kernel + user;

  if (prev_total == 0 || total <= prev_total || idle < prev_idle) {
    prev_idle = idle;
    prev_total = total;
    last_sample_ms = now_ms;
    return cached_percent;
  }

  total_delta = total - prev_total;
  idle_delta = idle - prev_idle;
  prev_idle = idle;
  prev_total = total;
  last_sample_ms = now_ms;
  if (total_delta == 0 || idle_delta > total_delta) return cached_percent;
  cached_percent = clamp_percent((double) (total_delta - idle_delta) * 100.0 / (double) total_delta);
  return cached_percent;
}

int gb_platform_metrics_get(gb_system_info_t *info) {
  MEMORYSTATUSEX mem;
  if (!info) return -1;
  memset(info, 0, sizeof(*info));

  mem.dwLength = sizeof(mem);
  if (GlobalMemoryStatusEx(&mem)) {
    info->total_memory = (uint64_t) mem.ullTotalPhys;
    info->free_memory = (uint64_t) mem.ullAvailPhys;
  }
  info->cpu_load_percent = cpu_usage_percent();
  return 0;
}
