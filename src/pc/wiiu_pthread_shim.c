#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef TARGET_WII_U
#include <coreinit/thread.h>
#include <coreinit/mutex.h>

#define SHIM_MAX_MUTEXES 256
#define SHIM_MAX_THREADS 64
#define SHIM_THREAD_STACK_SIZE (128 * 1024)

typedef struct {
    bool used;
    OSMutex mutex;
} ShimMutexEntry;

typedef struct {
    bool used;
    bool detached;
    OSThread thread;
    void *stack;
    void *(*start_routine)(void *);
    void *arg;
} ShimThreadEntry;

static ShimMutexEntry sMutexEntries[SHIM_MAX_MUTEXES];
static ShimThreadEntry sThreadEntries[SHIM_MAX_THREADS];
static volatile int sShimLock = 0;

static void shim_lock(void) {
    while (__sync_lock_test_and_set(&sShimLock, 1)) {
        OSYieldThread();
    }
}

static void shim_unlock(void) {
    __sync_lock_release(&sShimLock);
}

static int shim_mutex_handle_to_index(pthread_mutex_t handle) {
    if (handle == 0 || handle == (pthread_mutex_t)_PTHREAD_MUTEX_INITIALIZER) { return -1; }
    int index = (int)handle - 1;
    if (index < 0 || index >= SHIM_MAX_MUTEXES) { return -1; }
    return index;
}

static int shim_ensure_mutex(pthread_mutex_t *mutex) {
    int index = shim_mutex_handle_to_index(*mutex);
    if (index >= 0) { return index; }

    for (int i = 0; i < SHIM_MAX_MUTEXES; i++) {
        if (!sMutexEntries[i].used) {
            sMutexEntries[i].used = true;
            OSInitMutex(&sMutexEntries[i].mutex);
            *mutex = (pthread_mutex_t)(i + 1);
            return i;
        }
    }
    return -1;
}

static int shim_thread_entry(int argc, const char **argv) {
    (void)argv;
    int index = argc - 1;
    if (index < 0 || index >= SHIM_MAX_THREADS) { return 0; }

    ShimThreadEntry *entry = &sThreadEntries[index];
    if (!entry->used || entry->start_routine == NULL) { return 0; }

    void *result = entry->start_routine(entry->arg);
    return (int)(intptr_t)result;
}

int pthread_mutexattr_init(pthread_mutexattr_t *attr) {
    if (attr == NULL) { return EINVAL; }
    memset(attr, 0, sizeof(*attr));
    attr->is_initialized = 1;
    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attr) {
    if (attr == NULL) { return EINVAL; }
    memset(attr, 0, sizeof(*attr));
    return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type) {
    if (attr == NULL) { return EINVAL; }
#if defined(_UNIX98_THREAD_MUTEX_ATTRIBUTES)
    attr->type = type;
#else
    (void)type;
#endif
    return 0;
}

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr) {
    (void)attr;
    if (mutex == NULL) { return EINVAL; }

    shim_lock();
    int index = shim_ensure_mutex(mutex);
    shim_unlock();
    return (index >= 0) ? 0 : ENOMEM;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex) {
    if (mutex == NULL) { return EINVAL; }

    shim_lock();
    int index = shim_mutex_handle_to_index(*mutex);
    if (index < 0 || !sMutexEntries[index].used) {
        shim_unlock();
        return EINVAL;
    }
    sMutexEntries[index].used = false;
    *mutex = 0;
    shim_unlock();
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
    if (mutex == NULL) { return EINVAL; }

    shim_lock();
    int index = shim_ensure_mutex(mutex);
    shim_unlock();
    if (index < 0) { return ENOMEM; }

    OSLockMutex(&sMutexEntries[index].mutex);
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex) {
    if (mutex == NULL) { return EINVAL; }

    shim_lock();
    int index = shim_ensure_mutex(mutex);
    shim_unlock();
    if (index < 0) { return ENOMEM; }

    return OSTryLockMutex(&sMutexEntries[index].mutex) ? 0 : EBUSY;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    if (mutex == NULL) { return EINVAL; }

    int index = shim_mutex_handle_to_index(*mutex);
    if (index < 0 || !sMutexEntries[index].used) { return EINVAL; }

    OSUnlockMutex(&sMutexEntries[index].mutex);
    return 0;
}

int pthread_attr_init(pthread_attr_t *attr) {
    if (attr == NULL) { return EINVAL; }
    memset(attr, 0, sizeof(*attr));
    attr->is_initialized = 1;
    attr->detachstate = PTHREAD_CREATE_JOINABLE;
    attr->stacksize = SHIM_THREAD_STACK_SIZE;
    return 0;
}

int pthread_attr_destroy(pthread_attr_t *attr) {
    if (attr == NULL) { return EINVAL; }
    memset(attr, 0, sizeof(*attr));
    return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize) {
    if (attr == NULL) { return EINVAL; }
    attr->stacksize = (int)stacksize;
    return 0;
}

int pthread_attr_getschedparam(const pthread_attr_t *attr, struct sched_param *param) {
    if (attr == NULL || param == NULL) { return EINVAL; }
    *param = attr->schedparam;
    return 0;
}

int pthread_attr_setschedparam(pthread_attr_t *attr, const struct sched_param *param) {
    if (attr == NULL || param == NULL) { return EINVAL; }
    attr->schedparam = *param;
    return 0;
}

int pthread_attr_setschedpolicy(pthread_attr_t *attr, int policy) {
    if (attr == NULL) { return EINVAL; }
    attr->schedpolicy = policy;
    return 0;
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg) {
    if (thread == NULL || start_routine == NULL) { return EINVAL; }

    size_t stackSize = SHIM_THREAD_STACK_SIZE;
    int detached = PTHREAD_CREATE_JOINABLE;
    if (attr != NULL) {
        if (attr->stacksize > 0) { stackSize = (size_t)attr->stacksize; }
        detached = attr->detachstate;
    }

    shim_lock();
    int index = -1;
    for (int i = 0; i < SHIM_MAX_THREADS; i++) {
        if (!sThreadEntries[i].used) {
            index = i;
            sThreadEntries[i].used = true;
            break;
        }
    }
    shim_unlock();
    if (index < 0) { return EAGAIN; }

    void *stack = malloc(stackSize);
    if (stack == NULL) {
        shim_lock();
        sThreadEntries[index].used = false;
        shim_unlock();
        return ENOMEM;
    }

    ShimThreadEntry *entry = &sThreadEntries[index];
    entry->stack = stack;
    entry->start_routine = start_routine;
    entry->arg = arg;
    entry->detached = (detached == PTHREAD_CREATE_DETACHED);

    BOOL created = OSCreateThread(&entry->thread, shim_thread_entry, index + 1, NULL,
                                  (uint8_t *)stack + stackSize, (uint32_t)stackSize, 16,
                                  OS_THREAD_ATTRIB_AFFINITY_ANY);
    if (!created) {
        free(stack);
        shim_lock();
        memset(entry, 0, sizeof(*entry));
        shim_unlock();
        return EAGAIN;
    }

    if (entry->detached) {
        OSDetachThread(&entry->thread);
    }

    OSResumeThread(&entry->thread);
    *thread = (pthread_t)(index + 1);
    return 0;
}

int pthread_join(pthread_t thread, void **value_ptr) {
    int index = (int)thread - 1;
    if (index < 0 || index >= SHIM_MAX_THREADS) { return ESRCH; }

    shim_lock();
    if (!sThreadEntries[index].used || sThreadEntries[index].detached) {
        shim_unlock();
        return ESRCH;
    }
    shim_unlock();

    int result = 0;
    if (!OSJoinThread(&sThreadEntries[index].thread, &result)) { return ESRCH; }
    if (value_ptr != NULL) { *value_ptr = (void *)(intptr_t)result; }

    shim_lock();
    free(sThreadEntries[index].stack);
    memset(&sThreadEntries[index], 0, sizeof(sThreadEntries[index]));
    shim_unlock();
    return 0;
}

int pthread_detach(pthread_t thread) {
    int index = (int)thread - 1;
    if (index < 0 || index >= SHIM_MAX_THREADS) { return ESRCH; }

    shim_lock();
    if (!sThreadEntries[index].used) {
        shim_unlock();
        return ESRCH;
    }
    sThreadEntries[index].detached = true;
    shim_unlock();

    OSDetachThread(&sThreadEntries[index].thread);
    return 0;
}

void pthread_exit(void *value_ptr) {
    OSExitThread((int)(intptr_t)value_ptr);
}

pthread_t pthread_self(void) {
    OSThread *current = OSGetCurrentThread();
    if (current == NULL) { return 0; }

    shim_lock();
    for (int i = 0; i < SHIM_MAX_THREADS; i++) {
        if (sThreadEntries[i].used && &sThreadEntries[i].thread == current) {
            pthread_t handle = (pthread_t)(i + 1);
            shim_unlock();
            return handle;
        }
    }
    shim_unlock();
    return 0;
}

int pthread_equal(pthread_t t1, pthread_t t2) {
    return t1 == t2;
}

int pipe(int pipefd[2]) {
    if (pipefd == NULL) { return -1; }
    errno = ENOSYS;
    return -1;
}

#endif // TARGET_WII_U
