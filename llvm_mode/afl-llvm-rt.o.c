/*
   american fuzzy lop++ - LLVM instrumentation bootstrap
   ---------------------------------------------------

   Written by Laszlo Szekeres <lszekeres@google.com> and
              Michal Zalewski

   LLVM integration design comes from Laszlo Szekeres.

   Copyright 2015, 2016 Google Inc. All rights reserved.
   Copyright 2019-2020 AFLplusplus Project. All rights reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at:

     http://www.apache.org/licenses/LICENSE-2.0

   This code is the rewrite of afl-as.h's main_payload.

*/

#ifdef __ANDROID__
#include "android-ashmem.h"
#endif
#include "config.h"
#include "types.h"
#include "cmplog.h"
#include "llvm-ngram-coverage.h"

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <errno.h>

#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <sys/types.h>

#ifdef __linux__
#include "snapshot-inl.h"
#endif

/* This is a somewhat ugly hack for the experimental 'trace-pc-guard' mode.
   Basically, we need to make sure that the forkserver is initialized after
   the LLVM-generated runtime initialization pass, not before. */

#define CONST_PRIO 5

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE MAP_FIXED
#endif

#include <sys/mman.h>
#include <fcntl.h>

/* Globals needed by the injected instrumentation. The __afl_area_initial region
   is used for instrumentation output before __afl_map_shm() has a chance to
   run. It will end up as .comm, so it shouldn't be too wasteful. */

#ifdef AFL_REAL_LD
u8 __afl_area_initial[256000];
#else
u8                  __afl_area_initial[MAP_SIZE];
#endif
u8 *__afl_area_ptr = __afl_area_initial;
u8 *__afl_dictionary;

u32 __afl_final_loc;
u32 __afl_map_size = MAP_SIZE;
u32 __afl_dictionary_len;
u64 __afl_map_addr;

#ifdef __ANDROID__
PREV_LOC_T __afl_prev_loc[NGRAM_SIZE_MAX];
u32        __afl_prev_ctx;
u32        __afl_cmp_counter;
#else
__thread PREV_LOC_T __afl_prev_loc[NGRAM_SIZE_MAX];
__thread u32        __afl_prev_ctx;
__thread u32        __afl_cmp_counter;
#endif

struct cmp_map *__afl_cmp_map;

/* Running in persistent mode? */

static u8 is_persistent;

/* Error reporting to forkserver controller */

void send_forkserver_error(int error) {

  u32 status;
  if (!error || error > 0xffff) return;
  status = (FS_OPT_ERROR | FS_OPT_SET_ERROR(error));
  if (write(FORKSRV_FD + 1, (char *)&status, 4) != 4) return;

}

/* SHM setup. */

static void __afl_map_shm(void) {

  char *id_str = getenv(SHM_ENV_VAR);

  if (__afl_final_loc) {

    __afl_map_size = __afl_final_loc;
    if (__afl_final_loc > MAP_SIZE) {

      char *ptr;
      u32   val = 0;
      if ((ptr = getenv("AFL_MAP_SIZE")) != NULL) val = atoi(ptr);
      if (val < __afl_final_loc) {

        if (__afl_final_loc > FS_OPT_MAX_MAPSIZE) {

          fprintf(stderr,
                  "Error: AFL++ tools *require* to set AFL_MAP_SIZE to %u to "
                  "be able to run this instrumented program!\n",
                  __afl_final_loc);
          if (id_str) {

            send_forkserver_error(FS_ERROR_MAP_SIZE);
            exit(-1);

          }

        } else {

          fprintf(stderr,
                  "Warning: AFL++ tools will need to set AFL_MAP_SIZE to %u to "
                  "be able to run this instrumented program!\n",
                  __afl_final_loc);

        }

      }

    }

  }

  /* If we're running under AFL, attach to the appropriate region, replacing the
     early-stage __afl_area_initial region that is needed to allow some really
     hacky .init code to work correctly in projects such as OpenSSL. */

  if (getenv("AFL_DEBUG"))
    fprintf(stderr,
            "DEBUG: id_str %s, __afl_map_addr 0x%llx, MAP_SIZE %u, "
            "__afl_final_loc %u, max_size_forkserver %u/0x%x\n",
            id_str == NULL ? "<null>" : id_str, __afl_map_addr, MAP_SIZE,
            __afl_final_loc, FS_OPT_MAX_MAPSIZE, FS_OPT_MAX_MAPSIZE);

  if (id_str) {

#ifdef USEMMAP
    const char *   shm_file_path = id_str;
    int            shm_fd = -1;
    unsigned char *shm_base = NULL;

    /* create the shared memory segment as if it was a file */
    shm_fd = shm_open(shm_file_path, O_RDWR, 0600);
    if (shm_fd == -1) {

      fprintf(stderr, "shm_open() failed\n");
      send_forkserver_error(FS_ERROR_SHM_OPEN);
      exit(1);

    }

    /* map the shared memory segment to the address space of the process */
    if (__afl_map_addr) {

      shm_base =
          mmap((void *)__afl_map_addr, __afl_map_size, PROT_READ | PROT_WRITE,
               MAP_FIXED_NOREPLACE | MAP_SHARED, shm_fd, 0);

    } else {

      shm_base = mmap(0, __afl_map_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                      shm_fd, 0);

    }

    if (shm_base == MAP_FAILED) {

      close(shm_fd);
      shm_fd = -1;

      fprintf(stderr, "mmap() failed\n");
      if (__afl_map_addr)
        send_forkserver_error(FS_ERROR_MAP_ADDR);
      else
        send_forkserver_error(FS_ERROR_MMAP);
      exit(2);

    }

    __afl_area_ptr = shm_base;
#else
    u32 shm_id = atoi(id_str);

    __afl_area_ptr = shmat(shm_id, (void *)__afl_map_addr, 0);

#endif

    /* Whooooops. */

    if (__afl_area_ptr == (void *)-1) {

      if (__afl_map_addr)
        send_forkserver_error(FS_ERROR_MAP_ADDR);
      else
        send_forkserver_error(FS_ERROR_SHMAT);
      _exit(1);

    }

    /* Write something into the bitmap so that even with low AFL_INST_RATIO,
       our parent doesn't give up on us. */

    __afl_area_ptr[0] = 1;

  } else if (__afl_map_addr) {

    __afl_area_ptr =
        mmap((void *)__afl_map_addr, __afl_map_size, PROT_READ | PROT_WRITE,
             MAP_FIXED_NOREPLACE | MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (__afl_area_ptr == MAP_FAILED) {

      fprintf(stderr, "can not aquire mmap for address %p\n",
              (void *)__afl_map_addr);
      exit(1);

    }

  }

  id_str = getenv(CMPLOG_SHM_ENV_VAR);

  if (id_str) {

#ifdef USEMMAP
    const char *   shm_file_path = id_str;
    int            shm_fd = -1;
    unsigned char *shm_base = NULL;

    /* create the shared memory segment as if it was a file */
    shm_fd = shm_open(shm_file_path, O_RDWR, 0600);
    if (shm_fd == -1) {

      fprintf(stderr, "shm_open() failed\n");
      exit(1);

    }

    /* map the shared memory segment to the address space of the process */
    shm_base = mmap(0, sizeof(struct cmp_map), PROT_READ | PROT_WRITE,
                    MAP_SHARED, shm_fd, 0);
    if (shm_base == MAP_FAILED) {

      close(shm_fd);
      shm_fd = -1;

      fprintf(stderr, "mmap() failed\n");
      exit(2);

    }

    __afl_cmp_map = shm_base;
#else
    u32 shm_id = atoi(id_str);

    __afl_cmp_map = shmat(shm_id, NULL, 0);
#endif

    if (__afl_cmp_map == (void *)-1) _exit(1);

  }

}

#ifdef __linux__
static void __afl_start_snapshots(void) {

  static u8 tmp[4] = {0, 0, 0, 0};
  s32       child_pid;
  u32       status = 0;
  u32       already_read_first = 0;
  u32       was_killed;

  u8 child_stopped = 0;

  void (*old_sigchld_handler)(int) = 0;  // = signal(SIGCHLD, SIG_DFL);

  /* Phone home and tell the parent that we're OK. If parent isn't there,
     assume we're not running in forkserver mode and just execute program. */

  status |= (FS_OPT_ENABLED | FS_OPT_SNAPSHOT);
  if (__afl_map_size <= FS_OPT_MAX_MAPSIZE)
    status |= (FS_OPT_SET_MAPSIZE(__afl_map_size) | FS_OPT_MAPSIZE);
  if (__afl_dictionary_len > 0 && __afl_dictionary) status |= FS_OPT_AUTODICT;
  memcpy(tmp, &status, 4);

  if (write(FORKSRV_FD + 1, tmp, 4) != 4) return;

  if (__afl_dictionary_len > 0 && __afl_dictionary) {

    if (read(FORKSRV_FD, &was_killed, 4) != 4) _exit(1);

    if ((was_killed & (FS_OPT_ENABLED | FS_OPT_AUTODICT)) ==
        (FS_OPT_ENABLED | FS_OPT_AUTODICT)) {

      // great lets pass the dictionary through the forkserver FD
      u32 len = __afl_dictionary_len, offset = 0;
      s32 ret;

      if (write(FORKSRV_FD + 1, &len, 4) != 4) {

        write(2, "Error: could not send dictionary len\n",
              strlen("Error: could not send dictionary len\n"));
        _exit(1);

      }

      while (len != 0) {

        ret = write(FORKSRV_FD + 1, __afl_dictionary + offset, len);

        if (ret < 1) {

          write(2, "Error: could not send dictionary\n",
                strlen("Error: could not send dictionary\n"));
          _exit(1);

        }

        len -= ret;
        offset += ret;

      }

    } else {

      // uh this forkserver master does not understand extended option passing
      // or does not want the dictionary
      already_read_first = 1;

    }

  }

  while (1) {

    int status;

    if (already_read_first) {

      already_read_first = 0;

    } else {

      /* Wait for parent by reading from the pipe. Abort if read fails. */
      if (read(FORKSRV_FD, &was_killed, 4) != 4) _exit(1);

    }

    /* If we stopped the child in persistent mode, but there was a race
       condition and afl-fuzz already issued SIGKILL, write off the old
       process. */

    if (child_stopped && was_killed) {

      child_stopped = 0;
      if (waitpid(child_pid, &status, 0) < 0) _exit(1);

    }

    if (!child_stopped) {

      /* Once woken up, create a clone of our process. */

      child_pid = fork();
      if (child_pid < 0) _exit(1);

      /* In child process: close fds, resume execution. */

      if (!child_pid) {

        signal(SIGCHLD, old_sigchld_handler);

        close(FORKSRV_FD);
        close(FORKSRV_FD + 1);

        if (!afl_snapshot_do()) { raise(SIGSTOP); }

        __afl_area_ptr[0] = 1;
        memset(__afl_prev_loc, 0, NGRAM_SIZE_MAX * sizeof(PREV_LOC_T));

        return;

      }

    } else {

      /* Special handling for persistent mode: if the child is alive but
         currently stopped, simply restart it with SIGCONT. */

      kill(child_pid, SIGCONT);
      child_stopped = 0;

    }

    /* In parent process: write PID to pipe, then wait for child. */

    if (write(FORKSRV_FD + 1, &child_pid, 4) != 4) _exit(1);

    if (waitpid(child_pid, &status, WUNTRACED) < 0) _exit(1);

    /* In persistent mode, the child stops itself with SIGSTOP to indicate
       a successful run. In this case, we want to wake it up without forking
       again. */

    if (WIFSTOPPED(status)) child_stopped = 1;

    /* Relay wait status to pipe, then loop back. */

    if (write(FORKSRV_FD + 1, &status, 4) != 4) _exit(1);

  }

}

#endif

/* Fork server logic. */

static void __afl_start_forkserver(void) {

#ifdef __linux__
  if (!is_persistent && !__afl_cmp_map && !getenv("AFL_NO_SNAPSHOT") &&
      afl_snapshot_init() >= 0) {

    __afl_start_snapshots();
    return;

  }

#endif

  u8  tmp[4] = {0, 0, 0, 0};
  s32 child_pid;
  u32 status = 0;
  u32 already_read_first = 0;
  u32 was_killed;

  u8 child_stopped = 0;

  void (*old_sigchld_handler)(int) = 0;  // = signal(SIGCHLD, SIG_DFL);

  if (__afl_map_size <= FS_OPT_MAX_MAPSIZE)
    status |= (FS_OPT_SET_MAPSIZE(__afl_map_size) | FS_OPT_MAPSIZE);
  if (__afl_dictionary_len > 0 && __afl_dictionary) status |= FS_OPT_AUTODICT;
  if (status) status |= (FS_OPT_ENABLED);
  memcpy(tmp, &status, 4);

  /* Phone home and tell the parent that we're OK. If parent isn't there,
     assume we're not running in forkserver mode and just execute program. */

  if (write(FORKSRV_FD + 1, tmp, 4) != 4) return;

  if (__afl_dictionary_len > 0 && __afl_dictionary) {

    if (read(FORKSRV_FD, &was_killed, 4) != 4) _exit(1);

    if ((was_killed & (FS_OPT_ENABLED | FS_OPT_AUTODICT)) ==
        (FS_OPT_ENABLED | FS_OPT_AUTODICT)) {

      // great lets pass the dictionary through the forkserver FD
      u32 len = __afl_dictionary_len, offset = 0;
      s32 ret;

      if (write(FORKSRV_FD + 1, &len, 4) != 4) {

        write(2, "Error: could not send dictionary len\n",
              strlen("Error: could not send dictionary len\n"));
        _exit(1);

      }

      while (len != 0) {

        ret = write(FORKSRV_FD + 1, __afl_dictionary + offset, len);

        if (ret < 1) {

          write(2, "Error: could not send dictionary\n",
                strlen("Error: could not send dictionary\n"));
          _exit(1);

        }

        len -= ret;
        offset += ret;

      }

    } else {

      // uh this forkserver master does not understand extended option passing
      // or does not want the dictionary
      already_read_first = 1;

    }

  }

  while (1) {

    int status;

    /* Wait for parent by reading from the pipe. Abort if read fails. */

    if (already_read_first) {

      already_read_first = 0;

    } else {

      if (read(FORKSRV_FD, &was_killed, 4) != 4) _exit(1);

    }

    /* If we stopped the child in persistent mode, but there was a race
       condition and afl-fuzz already issued SIGKILL, write off the old
       process. */

    if (child_stopped && was_killed) {

      child_stopped = 0;
      if (waitpid(child_pid, &status, 0) < 0) _exit(1);

    }

    if (!child_stopped) {

      /* Once woken up, create a clone of our process. */

      child_pid = fork();
      if (child_pid < 0) _exit(1);

      /* In child process: close fds, resume execution. */

      if (!child_pid) {

        signal(SIGCHLD, old_sigchld_handler);

        close(FORKSRV_FD);
        close(FORKSRV_FD + 1);
        return;

      }

    } else {

      /* Special handling for persistent mode: if the child is alive but
         currently stopped, simply restart it with SIGCONT. */

      kill(child_pid, SIGCONT);
      child_stopped = 0;

    }

    /* In parent process: write PID to pipe, then wait for child. */

    if (write(FORKSRV_FD + 1, &child_pid, 4) != 4) _exit(1);

    if (waitpid(child_pid, &status, is_persistent ? WUNTRACED : 0) < 0)
      _exit(1);

    /* In persistent mode, the child stops itself with SIGSTOP to indicate
       a successful run. In this case, we want to wake it up without forking
       again. */

    if (WIFSTOPPED(status)) child_stopped = 1;

    /* Relay wait status to pipe, then loop back. */

    if (write(FORKSRV_FD + 1, &status, 4) != 4) _exit(1);

  }

}

/* A simplified persistent mode handler, used as explained in
 * llvm_mode/README.md. */

int __afl_persistent_loop(unsigned int max_cnt) {

  static u8  first_pass = 1;
  static u32 cycle_cnt;

  if (first_pass) {

    /* Make sure that every iteration of __AFL_LOOP() starts with a clean slate.
       On subsequent calls, the parent will take care of that, but on the first
       iteration, it's our job to erase any trace of whatever happened
       before the loop. */

    if (is_persistent) {

      memset(__afl_area_ptr, 0, __afl_map_size);
      __afl_area_ptr[0] = 1;
      memset(__afl_prev_loc, 0, NGRAM_SIZE_MAX * sizeof(PREV_LOC_T));

    }

    cycle_cnt = max_cnt;
    first_pass = 0;
    return 1;

  }

  if (is_persistent) {

    if (--cycle_cnt) {

      raise(SIGSTOP);

      __afl_area_ptr[0] = 1;
      memset(__afl_prev_loc, 0, NGRAM_SIZE_MAX * sizeof(PREV_LOC_T));

      return 1;

    } else {

      /* When exiting __AFL_LOOP(), make sure that the subsequent code that
         follows the loop is not traced. We do that by pivoting back to the
         dummy output region. */

      __afl_area_ptr = __afl_area_initial;

    }

  }

  return 0;

}

/* This one can be called from user code when deferred forkserver mode
    is enabled. */

void __afl_manual_init(void) {

  static u8 init_done;

  if (!init_done) {

    __afl_map_shm();
    __afl_start_forkserver();
    init_done = 1;

  }

}

/* Proper initialization routine. */

__attribute__((constructor(CONST_PRIO))) void __afl_auto_init(void) {

  is_persistent = !!getenv(PERSIST_ENV_VAR);

  if (getenv(DEFER_ENV_VAR)) return;

  __afl_manual_init();

}

/* The following stuff deals with supporting -fsanitize-coverage=trace-pc-guard.
   It remains non-operational in the traditional, plugin-backed LLVM mode.
   For more info about 'trace-pc-guard', see llvm_mode/README.md.

   The first function (__sanitizer_cov_trace_pc_guard) is called back on every
   edge (as opposed to every basic block). */

void __sanitizer_cov_trace_pc_guard(uint32_t *guard) {

  __afl_area_ptr[*guard]++;

}

/* Init callback. Populates instrumentation IDs. Note that we're using
   ID of 0 as a special value to indicate non-instrumented bits. That may
   still touch the bitmap, but in a fairly harmless way. */

void __sanitizer_cov_trace_pc_guard_init(uint32_t *start, uint32_t *stop) {

  u32 inst_ratio = 100;
  u8 *x;

  if (start == stop || *start) return;

  x = getenv("AFL_INST_RATIO");
  if (x) inst_ratio = atoi(x);

  if (!inst_ratio || inst_ratio > 100) {

    fprintf(stderr, "[-] ERROR: Invalid AFL_INST_RATIO (must be 1-100).\n");
    abort();

  }

  /* Make sure that the first element in the range is always set - we use that
     to avoid duplicate calls (which can happen as an artifact of the underlying
     implementation in LLVM). */

  *(start++) = R(MAP_SIZE - 1) + 1;

  while (start < stop) {

    if (R(100) < inst_ratio)
      *start = R(MAP_SIZE - 1) + 1;
    else
      *start = 0;

    start++;

  }

}

///// CmpLog instrumentation

void __cmplog_ins_hook1(uint8_t arg1, uint8_t arg2) {

  if (!__afl_cmp_map) return;

  uintptr_t k = (uintptr_t)__builtin_return_address(0);
  k = (k >> 4) ^ (k << 8);
  k &= CMP_MAP_W - 1;

  __afl_cmp_map->headers[k].type = CMP_TYPE_INS;

  u32 hits = __afl_cmp_map->headers[k].hits;
  __afl_cmp_map->headers[k].hits = hits + 1;
  // if (!__afl_cmp_map->headers[k].cnt)
  //  __afl_cmp_map->headers[k].cnt = __afl_cmp_counter++;

  __afl_cmp_map->headers[k].shape = 0;

  hits &= CMP_MAP_H - 1;
  __afl_cmp_map->log[k][hits].v0 = arg1;
  __afl_cmp_map->log[k][hits].v1 = arg2;

}

void __cmplog_ins_hook2(uint16_t arg1, uint16_t arg2) {

  if (!__afl_cmp_map) return;

  uintptr_t k = (uintptr_t)__builtin_return_address(0);
  k = (k >> 4) ^ (k << 8);
  k &= CMP_MAP_W - 1;

  __afl_cmp_map->headers[k].type = CMP_TYPE_INS;

  u32 hits = __afl_cmp_map->headers[k].hits;
  __afl_cmp_map->headers[k].hits = hits + 1;

  __afl_cmp_map->headers[k].shape = 1;

  hits &= CMP_MAP_H - 1;
  __afl_cmp_map->log[k][hits].v0 = arg1;
  __afl_cmp_map->log[k][hits].v1 = arg2;

}

void __cmplog_ins_hook4(uint32_t arg1, uint32_t arg2) {

  if (!__afl_cmp_map) return;

  uintptr_t k = (uintptr_t)__builtin_return_address(0);
  k = (k >> 4) ^ (k << 8);
  k &= CMP_MAP_W - 1;

  __afl_cmp_map->headers[k].type = CMP_TYPE_INS;

  u32 hits = __afl_cmp_map->headers[k].hits;
  __afl_cmp_map->headers[k].hits = hits + 1;

  __afl_cmp_map->headers[k].shape = 3;

  hits &= CMP_MAP_H - 1;
  __afl_cmp_map->log[k][hits].v0 = arg1;
  __afl_cmp_map->log[k][hits].v1 = arg2;

}

void __cmplog_ins_hook8(uint64_t arg1, uint64_t arg2) {

  if (!__afl_cmp_map) return;

  uintptr_t k = (uintptr_t)__builtin_return_address(0);
  k = (k >> 4) ^ (k << 8);
  k &= CMP_MAP_W - 1;

  __afl_cmp_map->headers[k].type = CMP_TYPE_INS;

  u32 hits = __afl_cmp_map->headers[k].hits;
  __afl_cmp_map->headers[k].hits = hits + 1;

  __afl_cmp_map->headers[k].shape = 7;

  hits &= CMP_MAP_H - 1;
  __afl_cmp_map->log[k][hits].v0 = arg1;
  __afl_cmp_map->log[k][hits].v1 = arg2;

}

#if defined(__APPLE__)
#pragma weak __sanitizer_cov_trace_const_cmp1 = __cmplog_ins_hook1
#pragma weak __sanitizer_cov_trace_const_cmp2 = __cmplog_ins_hook2
#pragma weak __sanitizer_cov_trace_const_cmp4 = __cmplog_ins_hook4
#pragma weak __sanitizer_cov_trace_const_cmp8 = __cmplog_ins_hook8

#pragma weak __sanitizer_cov_trace_cmp1 = __cmplog_ins_hook1
#pragma weak __sanitizer_cov_trace_cmp2 = __cmplog_ins_hook2
#pragma weak __sanitizer_cov_trace_cmp4 = __cmplog_ins_hook4
#pragma weak __sanitizer_cov_trace_cmp8 = __cmplog_ins_hook8
#else
void __sanitizer_cov_trace_const_cmp1(uint8_t arg1, uint8_t arg2)
    __attribute__((alias("__cmplog_ins_hook1")));
void __sanitizer_cov_trace_const_cmp2(uint16_t arg1, uint16_t arg2)
    __attribute__((alias("__cmplog_ins_hook2")));
void __sanitizer_cov_trace_const_cmp4(uint32_t arg1, uint32_t arg2)
    __attribute__((alias("__cmplog_ins_hook4")));
void __sanitizer_cov_trace_const_cmp8(uint64_t arg1, uint64_t arg2)
    __attribute__((alias("__cmplog_ins_hook8")));

void __sanitizer_cov_trace_cmp1(uint8_t arg1, uint8_t arg2)
    __attribute__((alias("__cmplog_ins_hook1")));
void __sanitizer_cov_trace_cmp2(uint16_t arg1, uint16_t arg2)
    __attribute__((alias("__cmplog_ins_hook2")));
void __sanitizer_cov_trace_cmp4(uint32_t arg1, uint32_t arg2)
    __attribute__((alias("__cmplog_ins_hook4")));
void __sanitizer_cov_trace_cmp8(uint64_t arg1, uint64_t arg2)
    __attribute__((alias("__cmplog_ins_hook8")));
#endif                                                /* defined(__APPLE__) */

void __sanitizer_cov_trace_switch(uint64_t val, uint64_t *cases) {

  for (uint64_t i = 0; i < cases[0]; i++) {

    uintptr_t k = (uintptr_t)__builtin_return_address(0) + i;
    k = (k >> 4) ^ (k << 8);
    k &= CMP_MAP_W - 1;

    __afl_cmp_map->headers[k].type = CMP_TYPE_INS;

    u32 hits = __afl_cmp_map->headers[k].hits;
    __afl_cmp_map->headers[k].hits = hits + 1;

    __afl_cmp_map->headers[k].shape = 7;

    hits &= CMP_MAP_H - 1;
    __afl_cmp_map->log[k][hits].v0 = val;
    __afl_cmp_map->log[k][hits].v1 = cases[i + 2];

  }

}

// POSIX shenanigan to see if an area is mapped.
// If it is mapped as X-only, we have a problem, so maybe we should add a check
// to avoid to call it on .text addresses
static int area_is_mapped(void *ptr, size_t len) {

  char *p = ptr;
  char *page = (char *)((uintptr_t)p & ~(sysconf(_SC_PAGE_SIZE) - 1));

  int r = msync(page, (p - page) + len, MS_ASYNC);
  if (r < 0) return errno != ENOMEM;
  return 1;

}

void __cmplog_rtn_hook(void *ptr1, void *ptr2) {

  if (!__afl_cmp_map) return;

  if (!area_is_mapped(ptr1, 32) || !area_is_mapped(ptr2, 32)) return;

  uintptr_t k = (uintptr_t)__builtin_return_address(0);
  k = (k >> 4) ^ (k << 8);
  k &= CMP_MAP_W - 1;

  __afl_cmp_map->headers[k].type = CMP_TYPE_RTN;

  u32 hits = __afl_cmp_map->headers[k].hits;
  __afl_cmp_map->headers[k].hits = hits + 1;

  __afl_cmp_map->headers[k].shape = 31;

  hits &= CMP_MAP_RTN_H - 1;
  __builtin_memcpy(((struct cmpfn_operands *)__afl_cmp_map->log[k])[hits].v0,
                   ptr1, 32);
  __builtin_memcpy(((struct cmpfn_operands *)__afl_cmp_map->log[k])[hits].v1,
                   ptr2, 32);

}

