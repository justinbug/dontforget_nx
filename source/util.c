/* util.c -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "util.h"
#include "config.h"

#if DEBUG_LOG

// One persistent log handle, flushed after every line so the last messages
// survive a crash. Reopening the file per call (the old behaviour) is far too
// slow on SD when the engine logs thousands of lines during boot.
static FILE *s_log = NULL;
static Mutex s_log_lock;

void userAppInit(void) {
  mutexInit(&s_log_lock);
  s_log = fopen(LOG_NAME, "w");
}

void userAppExit(void) {
  if (s_log) { fclose(s_log); s_log = NULL; }
}

#endif

int debugPrintf(char *text, ...) {
#if DEBUG_LOG
  if (!s_log) {
    s_log = fopen(LOG_NAME, "a");
    if (!s_log) return 0;
  }
  va_list list;
  mutexLock(&s_log_lock);
  va_start(list, text);
  vfprintf(s_log, text, list);
  va_end(list);
  fflush(s_log);
  mutexUnlock(&s_log_lock);
#else
  (void)text;
#endif
  return 0;
}

// Shared TLS block for the engine stack-protector guard at tpidr_el0 + 0x28.
// static uint8_t s_tls_block[0x1000] __attribute__((aligned(16)));

void tls_setup_guard(void) {
  void *current_tls = armGetTlsRw();
  if (current_tls) {
    uint64_t *guard_ptr = (uint64_t *)((uint8_t *)current_tls + 0x28);
    if (*guard_ptr == 0x0123456789ABCDEFull) {
      return; // Already set up!
    }
  }

  uint8_t *tls = malloc(0x1000);
  if (tls) {
    memset(tls, 0, 0x1000);
    *(uint64_t *)(tls + 0x28) = 0x0123456789ABCDEFull;
    armSetTlsRw(tls);
    debugPrintf("tls_setup_guard: allocated unique TLS at %p\n", tls);
  } else {
    debugPrintf("tls_setup_guard: FAILED to allocate TLS!\n");
  }
}

// boost the CPU to 1785MHz while loading
void cpu_boost(int on) {
  appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
}

int ret0(void) { return 0; }

int retm1(void) { return -1; }
