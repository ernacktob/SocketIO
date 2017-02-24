#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include <pthread.h>
#include <sys/errno.h>

#include "logging.h"

/* PROTOTYPES */
static void print_thread(pthread_t thread);
/* END PROTOTYPES */

/* XXX Maybe have a thread whose job is to print to terminal?? */

static void print_thread(pthread_t thread)
{
	unsigned char *p;
	size_t i;

	p = (unsigned char *)&thread;

	for (i = 0; i < sizeof thread; i++) {
		fprintf(stderr, "%02x", *p);
		++p;
	}
}

#ifdef DEBUG
void SOCKETIO_DEBUG(const char *prefixfmt, const char *func, const char *type, int nargs, ...)
{
	va_list args;
	const char *fmt;
	int i;

	flockfile(stderr);
	fprintf(stderr, "[SOCKETIO_DEBUG] ");
	fprintf(stderr, "{thread: ");
	print_thread(pthread_self());
	fprintf(stderr, "} ");
	fprintf(stderr, prefixfmt, func);
	fprintf(stderr, "%s: ", type);
	va_start(args, nargs);

	/* This should increment over each pair of fmt, value in args */
	for (i = 0; i < nargs - 1; i++) {
		fmt = va_arg(args, const char *);
		vfprintf(stderr, fmt, args);
		fprintf(stderr, ", ");
	}

	fmt = va_arg(args, const char *);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fprintf(stderr, "\n");
	fflush(stderr);
	funlockfile(stderr);
}
#endif

void default_error_func(const char *fmt, va_list args)
{
	/* This will show up as a prefix. */
	flockfile(stderr);
	fprintf(stderr, "[SOCKETIO_ERROR] {thread: ");
	print_thread(pthread_self());
	fprintf(stderr, "} ");
	vfprintf(stderr, fmt, args);
	fflush(stderr);
	funlockfile(stderr);
}

void default_syserror_func(const char *s, int errnum)
{
	flockfile(stderr);
	fprintf(stderr, "[SOCKETIO_SYSERROR] {thread: ");
	print_thread(pthread_self());
	fprintf(stderr, "} ");
	fprintf(stderr, "%s: (%s)\n", s, strerror(errnum));
	fflush(stderr);
	funlockfile(stderr);
}

void SOCKETIO_ERROR(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	default_error_func(fmt, args);
	va_end(args);
}

void SOCKETIO_SYSERROR(const char *s)
{
	default_syserror_func(s, errno);
}
