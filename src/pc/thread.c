#include "thread.h"

#include <assert.h>
#include <string.h>

int init_thread_handle(struct ThreadHandle *handle, void *(*entry)(void *), void *arg, void *sp, size_t sp_size) {
    int err1 = init_mutex(handle);
    int err2 = init_thread(handle, entry, arg, sp, sp_size);
    return (err1 != 0 || err2 != 0);
}

void cleanup_thread_handle(struct ThreadHandle *handle) {
    assert(handle != NULL);

    int err = destroy_mutex(handle);
    assert(err == 0);

    memset((void *)handle, 0, sizeof(struct ThreadHandle));
}

int init_thread(struct ThreadHandle *handle, void *(*entry)(void *), void *arg, void *sp, size_t sp_size) {
    assert(handle != NULL);

    int ret = 0;

#ifdef TARGET_WII_U
    // Wii U shim provides pthread_create()/join(); avoid advanced attr APIs here.
    (void)sp;
    (void)sp_size;
    ret = pthread_create(&handle->thread, NULL, entry, arg);
#else
    pthread_attr_t thattr = { 0 };

    int err = pthread_attr_init(&thattr);
    assert(err == 0);

    err = pthread_attr_setdetachstate(&thattr, PTHREAD_CREATE_JOINABLE);
    assert(err == 0);

    if (sp != NULL && sp_size > 0) {
        err = pthread_attr_setstack(&thattr, sp, sp_size);
        assert(err == 0);
    }

    ret = pthread_create(&handle->thread, &thattr, entry, arg);

    err = pthread_attr_destroy(&thattr);
    assert(err == 0);
#endif

    handle->state = (ret == 0) ? RUNNING : INVALID;
    return ret;
}

int join_thread(struct ThreadHandle *handle) {
    assert(handle != NULL);

    handle->state = STOPPED;
    return pthread_join(handle->thread, NULL);
}

int detach_thread(struct ThreadHandle *handle) {
    assert(handle != NULL);

    handle->state = STOPPED;
    return pthread_detach(handle->thread);
}

void exit_thread() {
    pthread_exit(NULL);
}

int stop_thread(struct ThreadHandle *handle) {
    assert(handle != NULL);

    handle->state = STOPPED;
#ifdef TARGET_WII_U
    // pthread_cancel is not guaranteed by the Wii U shim; join to stop cleanly.
    return pthread_join(handle->thread, NULL);
#else
    return pthread_cancel(handle->thread);
#endif
}

int init_mutex(struct ThreadHandle *handle) {
    assert(handle != NULL);
#ifdef TARGET_WII_U
    return pthread_mutex_init(&handle->mutex, NULL);
#else
    pthread_mutexattr_t mtattr;

    int err = pthread_mutexattr_init(&mtattr);
    assert(err == 0);

    err = pthread_mutexattr_settype(&mtattr, PTHREAD_MUTEX_ERRORCHECK);
    assert(err == 0);

    int ret = pthread_mutex_init(&handle->mutex, &mtattr);

    err = pthread_mutexattr_destroy(&mtattr);
    assert(err == 0);

    return ret;
#endif
}

int destroy_mutex(struct ThreadHandle *handle) {
    assert(handle != NULL);
    return pthread_mutex_destroy(&handle->mutex);
}

int lock_mutex(struct ThreadHandle *handle) {
    assert(handle != NULL);
    return pthread_mutex_lock(&handle->mutex);
}

int trylock_mutex(struct ThreadHandle *handle) {
    assert(handle != NULL);
    return pthread_mutex_trylock(&handle->mutex);
}

int unlock_mutex(struct ThreadHandle *handle) {
    assert(handle != NULL);
    return pthread_mutex_unlock(&handle->mutex);
}
