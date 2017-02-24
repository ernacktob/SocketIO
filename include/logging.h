#ifndef LOGGING_H
#define LOGGING_H

#ifdef DEBUG
void SOCKETIO_DEBUG(const char *prefixfmt, const char *func, const char *type, int nargs, ...);

#define VOIDARG				1, "%s", "void"
#define VOIDCALL			, "%s", "void"
#define ARG(fmt, name)			, #name" = "fmt, name
#define FUNC(fn)			, "function %s (%p)", #fn, fn

#define VOIDRET				"%s", "return"
#define RET(fmt, val)			"return "fmt, val

#define TOSTRING(x)			STRINGIFY(x)
#define STRINGIFY(x)			#x

#define SOCKETIO_DEBUG_ENTER(...)	SOCKETIO_DEBUG("{"__FILE__":"TOSTRING(__LINE__)" %s} ", __func__, "ENTER", __VA_ARGS__)
#define SOCKETIO_DEBUG_CALL(...)	SOCKETIO_DEBUG("{"__FILE__":"TOSTRING(__LINE__)" %s} ", __func__, "CALL", __VA_ARGS__)
#define SOCKETIO_DEBUG_RETURN(...)	SOCKETIO_DEBUG("{"__FILE__":"TOSTRING(__LINE__)" %s} ", __func__, "RETURN", 1, __VA_ARGS__)

#else
#define VOIDARG				"", ""	/* Must expand to two parameters */
#define VOIDCALL			""
#define ARG(fmt, name)			""	/* String concatenation in preprocessor will turn all ARG(x,y) into one big "" */
#define FUNC(fn)			""

#define VOIDRET
#define RET(fmt, val)

#define SOCKETIO_DEBUG_ENTER(x)
#define SOCKETIO_DEBUG_CALL(x)
#define SOCKETIO_DEBUG_RETURN(x)

#endif /* DEBUG */

void SOCKETIO_ERROR(const char *fmt, ...);
void SOCKETIO_SYSERROR(const char *s);

#endif
