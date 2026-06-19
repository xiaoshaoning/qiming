#ifndef SIM_THREAD_H
#define SIM_THREAD_H

#include <stddef.h>

/* Platform threading abstraction for libqsim.
 * Supports Win32 (MSVC/MinGW) and POSIX (pthreads). */

#ifdef _WIN32
  #include <windows.h>

  typedef HANDLE sim_thread_t;

  typedef SRWLOCK sim_mutex_t;
  #define SIM_MUTEX_INIT SRWLOCK_INIT

  /* Sense-reversing spin barrier using atomic decrement.
   * For short-duration barriers (microseconds) this avoids the
   * ~1-5 µs kernel transition of SRWLOCK+CONDITION_VARIABLE. */
  typedef struct {
      volatile LONG count;      /* starts at threshold, decremented each arrival */
      volatile LONG sense;      /* toggled each cycle by the last thread */
      LONG threshold;           /* number of threads (fixed at init) */
  } sim_barrier_t;

#else
  #include <pthread.h>

  typedef pthread_t sim_thread_t;

  typedef pthread_mutex_t sim_mutex_t;
  #define SIM_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER

  typedef pthread_barrier_t sim_barrier_t;

#endif

/* Thread */
int sim_thread_create(sim_thread_t *t, void *(*func)(void*), void *arg);
int sim_thread_join(sim_thread_t t);

/* Mutex (used for global event pool fallback) */
int sim_mutex_init(sim_mutex_t *m);
int sim_mutex_lock(sim_mutex_t *m);
int sim_mutex_unlock(sim_mutex_t *m);
int sim_mutex_destroy(sim_mutex_t *m);

/* Barrier (N threads wait, then all proceed) */
int sim_barrier_init(sim_barrier_t *b, unsigned count);
int sim_barrier_wait(sim_barrier_t *b);
int sim_barrier_destroy(sim_barrier_t *b);

/* Platform atomic operations (for parallel_phase flag, etc.)
 * Uses compiler barriers / intrinsics instead of C11 _Atomic for MSVC compat. */
#if defined(_MSC_VER)
  #include <intrin.h>
  #pragma intrinsic(_ReadWriteBarrier)
  #define sim_atomic_store(ptr, val)  ( *(ptr) = (val), _ReadWriteBarrier() )
  #define sim_atomic_load(ptr)        ( _ReadWriteBarrier(), *(volatile int *)(ptr) )
#elif defined(__GNUC__) || defined(__clang__)
  #define sim_atomic_store(ptr, val)  __atomic_store_n((ptr), (val), __ATOMIC_SEQ_CST)
  #define sim_atomic_load(ptr)        __atomic_load_n((ptr), __ATOMIC_SEQ_CST)
#else
  #error "No atomic operation support for this compiler"
#endif

#endif /* SIM_THREAD_H */
