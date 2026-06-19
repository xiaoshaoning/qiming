#include "libqsim/sim_thread.h"

/* ── Thread ── */

#ifdef _WIN32

int sim_thread_create(sim_thread_t *t, void *(*func)(void*), void *arg) {
    *t = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)func, arg, 0, NULL);
    return *t ? 0 : -1;
}

int sim_thread_join(sim_thread_t t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return 0;
}

/* ── Mutex ── */

int sim_mutex_init(sim_mutex_t *m) {
    InitializeSRWLock(m);
    return 0;
}

int sim_mutex_lock(sim_mutex_t *m) {
    AcquireSRWLockExclusive(m);
    return 0;
}

int sim_mutex_unlock(sim_mutex_t *m) {
    ReleaseSRWLockExclusive(m);
    return 0;
}

int sim_mutex_destroy(sim_mutex_t *m) {
    (void)m;
    return 0; /* SRW locks have no cleanup */
}

/* ── Barrier ── */

int sim_barrier_init(sim_barrier_t *b, unsigned count) {
    b->count = (LONG)count;
    b->sense = 0;
    b->threshold = (LONG)count;
    return 0;
}

/* Sense-reversing spin barrier using InterlockedDecrement.
 * N threads arrive, all decrement count atomically. The last
 * thread to arrive (decrements to 0) resets count and flips
 * sense, releasing all spinners. No kernel transition needed. */
int sim_barrier_wait(sim_barrier_t *b) {
    LONG local_sense = b->sense;
    if (_InterlockedDecrement(&b->count) == 0) {
        /* Last thread: reset count and flip sense to release others */
        b->count = b->threshold;
        b->sense = !local_sense;
    } else {
        /* Spin until sense flips */
        while (b->sense == local_sense) {
            YieldProcessor();  /* PAUSE hint — relax the pipeline */
        }
    }
    return 0;
}

int sim_barrier_destroy(sim_barrier_t *b) {
    (void)b;
    return 0; /* No dynamic resources */
}

#else /* POSIX */

int sim_thread_create(sim_thread_t *t, void *(*func)(void*), void *arg) {
    return pthread_create(t, NULL, func, arg);
}

int sim_thread_join(sim_thread_t t) {
    return pthread_join(t, NULL);
}

int sim_mutex_init(sim_mutex_t *m) {
    return pthread_mutex_init(m, NULL);
}

int sim_mutex_lock(sim_mutex_t *m) {
    return pthread_mutex_lock(m);
}

int sim_mutex_unlock(sim_mutex_t *m) {
    return pthread_mutex_unlock(m);
}

int sim_mutex_destroy(sim_mutex_t *m) {
    return pthread_mutex_destroy(m);
}

int sim_barrier_init(sim_barrier_t *b, unsigned count) {
    return pthread_barrier_init(b, NULL, count);
}

int sim_barrier_wait(sim_barrier_t *b) {
    return pthread_barrier_wait(b);
}

int sim_barrier_destroy(sim_barrier_t *b) {
    return pthread_barrier_destroy(b);
}

#endif /* _WIN32 */
