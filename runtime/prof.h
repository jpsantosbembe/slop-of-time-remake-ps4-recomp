// Between perf.c (frame timing, always built) and the OS's sampling profiler (prof_win.c on Windows, platform/prof_orbis.c
// on the PS4).
#pragma once
#include <stdint.h>

extern volatile uint32_t prof_render_tid;   // the thread that presents (perf_present)
extern volatile uint32_t prof_main_tid;     // the guest main thread (perf_main_thread)
extern volatile uint32_t prof_replay_tid;   // the asynchronous replay (perf_replay_thread): its waits hold up the frame too

// The sampler; perf.c's weak versions do nothing.
int prof_sampler_start(void);               // TOTK_PROFILE: starts sampling; 1 when it runs
void prof_sampler_write(void);              // writes the profile once (prof_write)
void prof_sampler_frame(uint64_t frame, uint64_t now, uint64_t d);   // every present; d: the interval, 0 on the first
// Called ON the thread that takes a role (perf_main_thread, perf_replay_thread, the first present of a render thread):
// the PS4 sampler keeps its pthread_t to signal it, since the PS4 has no way to stop a thread by id.
enum { PROF_ROLE_MAIN, PROF_ROLE_RENDER, PROF_ROLE_REPLAY };
void prof_sampler_thread(int role);
