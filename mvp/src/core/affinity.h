#pragma once

// CPU pinning helpers for the matching engine thread.
//
// In production you want the matcher to own exactly one physical core, with
// no context switches, no interrupt handlers, and no other tenants. The
// Linux recipe is:
//
//   1. Boot kernel with `isolcpus=3 nohz_full=3 rcu_nocbs=3` — core 3 is now
//      invisible to the scheduler, gets no scheduler tick, and won't be
//      chosen for RCU callbacks.
//   2. `echo 0 > /sys/bus/workqueue/devices/writeback/cpumask` etc. to move
//      kernel threads off the isolated core.
//   3. At runtime, pin the matcher to core 3 via pthread_setaffinity_np.
//   4. Optionally set SCHED_FIFO priority 99 to preempt anything that sneaks
//      through.
//
// macOS has no equivalent of isolcpus; the best we can do is a thread
// affinity *hint* via THREAD_AFFINITY_POLICY. The kernel is free to ignore
// it, but in practice it keeps the thread on a single performance core on
// M-series, which cuts migration jitter.

#include <cstdint>
#include <cstdio>
#include <thread>

#if defined(__linux__)
  #include <pthread.h>
  #include <sched.h>
#elif defined(__APPLE__)
  #include <mach/mach.h>
  #include <mach/thread_act.h>
  #include <mach/thread_policy.h>
  #include <pthread.h>
#endif

namespace hft::core {

// Pin the calling (or specified) thread to a single CPU. Returns true on
// success. On macOS "success" means the hint was accepted — the kernel may
// still migrate us; the only hard pin is on Linux with isolcpus.
inline bool pin_thread_to_cpu(int cpu) {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    int rc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    if (rc != 0) {
        std::fprintf(stderr, "pin_thread_to_cpu(%d) failed: rc=%d\n", cpu, rc);
        return false;
    }
    return true;
#elif defined(__APPLE__)
    // THREAD_AFFINITY_POLICY groups threads that should share an L2. Using
    // (cpu+1) as the tag gives us a distinct affinity set per target core.
    thread_affinity_policy_data_t policy;
    policy.affinity_tag = cpu + 1;
    mach_port_t mt = pthread_mach_thread_np(pthread_self());
    kern_return_t kr = thread_policy_set(mt, THREAD_AFFINITY_POLICY,
                                          reinterpret_cast<thread_policy_t>(&policy),
                                          THREAD_AFFINITY_POLICY_COUNT);
    if (kr != KERN_SUCCESS) {
        // Apple Silicon (M1/M2/M3) returns KERN_NOT_SUPPORTED (46) — the
        // policy simply isn't wired up on ARM macOS. That's fine; we still
        // get most of the jitter reduction from the fact that macOS's QoS
        // classifier keeps a hot CPU-bound thread on a P-core. Log at debug
        // level only; don't warn on every run.
        if (kr != 46) {
            std::fprintf(stderr,
                         "thread_policy_set(cpu=%d) failed: %d\n", cpu, kr);
        }
        return false;
    }
    return true;
#else
    (void)cpu;
    return false;
#endif
}

// Bump scheduling priority to SCHED_FIFO on Linux (requires CAP_SYS_NICE).
// No-op on macOS — Mach scheduler doesn't expose an equivalent from userspace
// that's useful for HFT.
inline bool set_realtime_priority(int prio = 90) {
#if defined(__linux__)
    sched_param sp{};
    sp.sched_priority = prio;
    int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    return rc == 0;
#else
    (void)prio;
    return false;
#endif
}

} // namespace hft::core
