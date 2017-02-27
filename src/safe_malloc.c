#include "safe_malloc.h"
#include "logging.h"

#if !defined(MALLOC_IS_THREAD_SAFE) || !defined(FREE_IS_THREAD_SAFE)
static pthread_mutex_t malloc_mtx = PTHREAD_MUTEX_INITIALIZER;
#endif

#ifndef MALLOC_IS_THREAD_SAFE
void *malloc_locked(size_t size)
{
	void *ptr;
	int rc;

	if ((rc = pthread_mutex_lock(&malloc_mtx)) != 0) {
		errno = rc;
		SOCKETIO_SYSERROR("pthread_mutex_lock");
		return NULL;
	}

	ptr = malloc(size);

	if (ptr == NULL)
		SOCKETIO_SYSERROR("malloc");

	if ((rc = pthread_mutex_unlock(&malloc_mtx) != 0)) {
		errno = rc;
		SOCKETIO_SYSERROR("pthread_mutex_unlock");
	}

	return ptr;
}

void *realloc_locked(void *ptr, size_t size)
{
	void *newptr;
	int rc;

	if ((rc = pthread_mutex_lock(&malloc_mtx)) != 0) {
		errno = rc;
		SOCKETIO_SYSERROR("pthread_mutex_lock");
		return NULL;
	}

	newptr = realloc(ptr, size);

	if (newptr == NULL)
		SOCKETIO_SYSERROR("realloc");

	if ((rc = pthread_mutex_unlock(&malloc_mtx) != 0)) {
		errno = rc;
		SOCKETIO_SYSERROR("pthread_mutex_unlock");
	}

	return newptr;
}
#endif

#ifndef FREE_IS_THREAD_SAFE
void free_locked(void *ptr)
{
	int rc;

	if ((rc = pthread_mutex_lock(&malloc_mtx)) != 0) {
		errno = rc;
		SOCKETIO_SYSERROR("pthread_mutex_lock");
		return;
	}

	free(ptr);

	if ((rc = pthread_mutex_unlock(&malloc_mtx)) != 0) {
		errno = rc;
		SOCKETIO_SYSERROR("pthread_mutex_unlock");
	}
}
#endif
