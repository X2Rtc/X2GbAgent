#ifndef GB_PLATFORM_H
#define GB_PLATFORM_H

#include <stdint.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
typedef struct {
  HANDLE handle;
  DWORD id;
} gb_thread_t;
typedef CRITICAL_SECTION gb_mutex_t;
typedef CONDITION_VARIABLE gb_cond_t;
#else
#include <pthread.h>
typedef pthread_t gb_thread_t;
typedef pthread_mutex_t gb_mutex_t;
typedef pthread_cond_t gb_cond_t;
#endif

typedef void *(*gb_thread_fn)(void *arg);

typedef struct {
  uint64_t total_memory;
  uint64_t free_memory;
  double cpu_load_percent;
} gb_system_info_t;

int gb_mutex_init(gb_mutex_t *mutex);
void gb_mutex_destroy(gb_mutex_t *mutex);
void gb_mutex_lock(gb_mutex_t *mutex);
void gb_mutex_unlock(gb_mutex_t *mutex);

int gb_cond_init(gb_cond_t *cond);
void gb_cond_destroy(gb_cond_t *cond);
void gb_cond_wait(gb_cond_t *cond, gb_mutex_t *mutex);
void gb_cond_broadcast(gb_cond_t *cond);

int gb_thread_create(gb_thread_t *thread, gb_thread_fn fn, void *arg);
void gb_thread_join(gb_thread_t thread);
void gb_thread_detach(gb_thread_t thread);
int gb_thread_is_current(gb_thread_t thread);

void gb_sleep_ms(unsigned milliseconds);
void gb_sleep_us(unsigned microseconds);
int gb_get_system_info(gb_system_info_t *info);

#endif
