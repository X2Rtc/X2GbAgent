#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "gb_platform.h"
#include "platform/os/gb_platform_metrics.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <process.h>

typedef struct {
  gb_thread_fn fn;
  void *arg;
} gb_thread_start_t;

static unsigned __stdcall gb_thread_entry(void *arg) {
  gb_thread_start_t *start = (gb_thread_start_t *) arg;
  gb_thread_fn fn = start->fn;
  void *fn_arg = start->arg;
  free(start);
  (void) fn(fn_arg);
  return 0;
}

int gb_mutex_init(gb_mutex_t *mutex) {
  if (!mutex) return -1;
  InitializeCriticalSection(mutex);
  return 0;
}

void gb_mutex_destroy(gb_mutex_t *mutex) {
  if (mutex) DeleteCriticalSection(mutex);
}

void gb_mutex_lock(gb_mutex_t *mutex) {
  EnterCriticalSection(mutex);
}

void gb_mutex_unlock(gb_mutex_t *mutex) {
  LeaveCriticalSection(mutex);
}

int gb_cond_init(gb_cond_t *cond) {
  if (!cond) return -1;
  InitializeConditionVariable(cond);
  return 0;
}

void gb_cond_destroy(gb_cond_t *cond) {
  (void) cond;
}

void gb_cond_wait(gb_cond_t *cond, gb_mutex_t *mutex) {
  SleepConditionVariableCS(cond, mutex, INFINITE);
}

void gb_cond_broadcast(gb_cond_t *cond) {
  WakeAllConditionVariable(cond);
}

int gb_thread_create(gb_thread_t *thread, gb_thread_fn fn, void *arg) {
  gb_thread_start_t *start;
  uintptr_t handle;
  unsigned id = 0;
  if (!thread || !fn) return -1;
  memset(thread, 0, sizeof(*thread));
  start = (gb_thread_start_t *) malloc(sizeof(*start));
  if (!start) return -1;
  start->fn = fn;
  start->arg = arg;
  handle = _beginthreadex(NULL, 0, gb_thread_entry, start, 0, &id);
  if (handle == 0) {
    free(start);
    return -1;
  }
  thread->handle = (HANDLE) handle;
  thread->id = (DWORD) id;
  return 0;
}

void gb_thread_join(gb_thread_t thread) {
  if (thread.handle) {
    WaitForSingleObject(thread.handle, INFINITE);
    CloseHandle(thread.handle);
  }
}

void gb_thread_detach(gb_thread_t thread) {
  if (thread.handle) CloseHandle(thread.handle);
}

int gb_thread_is_current(gb_thread_t thread) {
  return thread.id == GetCurrentThreadId();
}

void gb_sleep_ms(unsigned milliseconds) {
  Sleep((DWORD) milliseconds);
}

void gb_sleep_us(unsigned microseconds) {
  DWORD ms = (DWORD) ((microseconds + 999U) / 1000U);
  Sleep(ms);
}

int gb_get_system_info(gb_system_info_t *info) {
  return gb_platform_metrics_get(info);
}

#else

#include <errno.h>
#include <time.h>
#include <unistd.h>

int gb_mutex_init(gb_mutex_t *mutex) {
  return pthread_mutex_init(mutex, NULL);
}

void gb_mutex_destroy(gb_mutex_t *mutex) {
  (void) pthread_mutex_destroy(mutex);
}

void gb_mutex_lock(gb_mutex_t *mutex) {
  (void) pthread_mutex_lock(mutex);
}

void gb_mutex_unlock(gb_mutex_t *mutex) {
  (void) pthread_mutex_unlock(mutex);
}

int gb_cond_init(gb_cond_t *cond) {
  return pthread_cond_init(cond, NULL);
}

void gb_cond_destroy(gb_cond_t *cond) {
  (void) pthread_cond_destroy(cond);
}

void gb_cond_wait(gb_cond_t *cond, gb_mutex_t *mutex) {
  (void) pthread_cond_wait(cond, mutex);
}

void gb_cond_broadcast(gb_cond_t *cond) {
  (void) pthread_cond_broadcast(cond);
}

int gb_thread_create(gb_thread_t *thread, gb_thread_fn fn, void *arg) {
  return pthread_create(thread, NULL, fn, arg);
}

void gb_thread_join(gb_thread_t thread) {
  (void) pthread_join(thread, NULL);
}

void gb_thread_detach(gb_thread_t thread) {
  (void) pthread_detach(thread);
}

int gb_thread_is_current(gb_thread_t thread) {
  return pthread_equal(pthread_self(), thread);
}

void gb_sleep_ms(unsigned milliseconds) {
  struct timespec ts;
  ts.tv_sec = (time_t) (milliseconds / 1000U);
  ts.tv_nsec = (long) (milliseconds % 1000U) * 1000000L;
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

void gb_sleep_us(unsigned microseconds) {
  struct timespec ts;
  ts.tv_sec = (time_t) (microseconds / 1000000U);
  ts.tv_nsec = (long) (microseconds % 1000000U) * 1000L;
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

int gb_get_system_info(gb_system_info_t *info) {
  return gb_platform_metrics_get(info);
}

#endif
