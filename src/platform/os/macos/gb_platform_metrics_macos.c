#include "platform/os/gb_platform_metrics.h"

#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/vm_statistics.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

static double clamp_percent(double value) {
  if (value < 0.0) return 0.0;
  if (value > 100.0) return 100.0;
  return value;
}

static double cpu_usage_percent(void) {
  static natural_t prev_user = 0;
  static natural_t prev_system = 0;
  static natural_t prev_idle = 0;
  static natural_t prev_nice = 0;
  host_cpu_load_info_data_t cpu_info;
  mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
  natural_t user;
  natural_t system;
  natural_t idle;
  natural_t nice;
  unsigned long long active_delta;
  unsigned long long total_delta;

  if (host_statistics(mach_host_self(),
                      HOST_CPU_LOAD_INFO,
                      (host_info_t) &cpu_info,
                      &count) != KERN_SUCCESS) {
    return 0.0;
  }

  user = cpu_info.cpu_ticks[CPU_STATE_USER];
  system = cpu_info.cpu_ticks[CPU_STATE_SYSTEM];
  idle = cpu_info.cpu_ticks[CPU_STATE_IDLE];
  nice = cpu_info.cpu_ticks[CPU_STATE_NICE];

  if (prev_user == 0 && prev_system == 0 && prev_idle == 0 && prev_nice == 0) {
    prev_user = user;
    prev_system = system;
    prev_idle = idle;
    prev_nice = nice;
    return 0.0;
  }

  active_delta = (unsigned long long) (user - prev_user) +
                 (unsigned long long) (system - prev_system) +
                 (unsigned long long) (nice - prev_nice);
  total_delta = active_delta + (unsigned long long) (idle - prev_idle);

  prev_user = user;
  prev_system = system;
  prev_idle = idle;
  prev_nice = nice;

  if (total_delta == 0) return 0.0;
  return clamp_percent((double) active_delta * 100.0 / (double) total_delta);
}

static uint64_t total_memory_bytes(void) {
  uint64_t total = 0;
  size_t size = sizeof(total);
  if (sysctlbyname("hw.memsize", &total, &size, NULL, 0) != 0) return 0;
  return total;
}

static uint64_t available_memory_bytes(void) {
  vm_statistics64_data_t vm_stat;
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  vm_size_t page_size = 0;
  uint64_t pages;

  if (host_page_size(mach_host_self(), &page_size) != KERN_SUCCESS) return 0;
  if (host_statistics64(mach_host_self(),
                        HOST_VM_INFO64,
                        (host_info64_t) &vm_stat,
                        &count) != KERN_SUCCESS) {
    return 0;
  }

  pages = (uint64_t) vm_stat.free_count +
          (uint64_t) vm_stat.inactive_count +
          (uint64_t) vm_stat.speculative_count;
  return pages * (uint64_t) page_size;
}

int gb_platform_metrics_get(gb_system_info_t *info) {
  if (!info) return -1;
  memset(info, 0, sizeof(*info));
  info->total_memory = total_memory_bytes();
  info->free_memory = available_memory_bytes();
  info->cpu_load_percent = cpu_usage_percent();
  return 0;
}
