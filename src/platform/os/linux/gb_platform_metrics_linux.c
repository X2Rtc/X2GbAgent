#include "platform/os/gb_platform_metrics.h"

#include <stdio.h>
#include <string.h>
#include <sys/sysinfo.h>

static double clamp_percent(double value) {
  if (value < 0.0) return 0.0;
  if (value > 100.0) return 100.0;
  return value;
}

static int read_cpu_totals(unsigned long long *idle_out, unsigned long long *total_out) {
  FILE *fp;
  char label[8];
  unsigned long long user = 0;
  unsigned long long nice = 0;
  unsigned long long system = 0;
  unsigned long long idle = 0;
  unsigned long long iowait = 0;
  unsigned long long irq = 0;
  unsigned long long softirq = 0;
  unsigned long long steal = 0;

  if (!idle_out || !total_out) return -1;
  fp = fopen("/proc/stat", "r");
  if (!fp) return -1;
  if (fscanf(fp,
             "%7s %llu %llu %llu %llu %llu %llu %llu %llu",
             label,
             &user,
             &nice,
             &system,
             &idle,
             &iowait,
             &irq,
             &softirq,
             &steal) < 5) {
    fclose(fp);
    return -1;
  }
  fclose(fp);
  if (strcmp(label, "cpu") != 0) return -1;

  *idle_out = idle + iowait;
  *total_out = user + nice + system + idle + iowait + irq + softirq + steal;
  return 0;
}

static double cpu_usage_percent(void) {
  static unsigned long long prev_idle = 0;
  static unsigned long long prev_total = 0;
  unsigned long long idle = 0;
  unsigned long long total = 0;
  unsigned long long total_delta;
  unsigned long long idle_delta;

  if (read_cpu_totals(&idle, &total) != 0) return 0.0;
  if (prev_total == 0 || total <= prev_total || idle < prev_idle) {
    prev_idle = idle;
    prev_total = total;
    return 0.0;
  }

  total_delta = total - prev_total;
  idle_delta = idle - prev_idle;
  prev_idle = idle;
  prev_total = total;
  if (total_delta == 0 || idle_delta > total_delta) return 0.0;
  return clamp_percent((double) (total_delta - idle_delta) * 100.0 / (double) total_delta);
}

int gb_platform_metrics_get(gb_system_info_t *info) {
  struct sysinfo si;
  if (!info) return -1;
  memset(info, 0, sizeof(*info));
  if (sysinfo(&si) != 0) return -1;
  info->total_memory = (uint64_t) si.totalram * (uint64_t) si.mem_unit;
  info->free_memory = (uint64_t) si.freeram * (uint64_t) si.mem_unit;
  info->cpu_load_percent = cpu_usage_percent();
  return 0;
}
