#ifndef __SSSP_H__
#define __SSSP_H__

#include <errno.h>
#include <stdint.h>
#include <X11/Xutil.h>

#define CPPSTR(s) #s
#define ISTEAMERROR(i, v) "ERROR: " #i " is NULL! " \
	"Check interface version " CPPSTR(v) " in libsteam_api.so."
#define UNUSED __attribute__((unused))
#define USE_OLD_USERSTATS 0


#ifndef _GNU_SOURCE
	#warning "GNU-extensions disabled, expect less features."

	#include <libgen.h>

	#define RTLD_NEXT ((void *) -1L);

	char *program_invocation_name = NULL;
	char *program_invocation_short_name = NULL;
#endif


/* Hook defines */
typedef int (*hookFunc)(void);
typedef int (*hookCPFunc)(const void *, ...);
typedef int (*hookPFunc)(void *, ...);
typedef void (*hookVPFunc)(void *, ...);
typedef void *(*hookPPFunc)(void *, ...);
typedef void *(*hookPCPFunc)(const void *, ...);


/* Logging */
#ifndef DFLT_LOG_LEVEL
#define DFLT_LOG_LEVEL 2
#endif

#define log(level, format, args...) { \
	do { \
		log_dolog(level, __FUNCTION__, __LINE__, \
				format, ##args ); \
	} while (0); \
}

enum LogLevel
{
	LOG_NONE,

	LOG_ERROR,
	LOG_WARN,
	LOG_NOTICE,
	LOG_INFO,
	LOG_DEBUG,

	LOG_MAX
};

extern enum LogLevel g_logLevel;

static inline Bool
log_check(enum LogLevel ll)
{
	return  ll > LOG_NONE && ll < LOG_MAX && ll <= g_logLevel;
}

extern void
log_dolog(enum LogLevel ll, const char *func,
		const uint32_t line, const char *format, ...);

#endif /* __SSSP_H__ */
