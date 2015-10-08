#include <stdarg.h>
#include <stdio.h>

#include "sssp.h"

static const unsigned int truncatedLength = 12; //"---truncated"
void
log_dolog(enum LogLevel ll, const char *func,
		const uint32_t line, const char *format, ...)
{
	if (log_check(ll))
	{
		va_list ap;
		char str[1024];
		unsigned int len = 0;

		len = snprintf(str, sizeof(str), "[%s:%s@%u]: ",
			program_invocation_short_name, func, line);

		va_start(ap, format);
		if (len < sizeof(str) - truncatedLength)
		{
			len += vsnprintf(str + len, sizeof(str) - truncatedLength - len, format, ap);
		}
		va_end(ap);

		//if we are unable to print everything, then add sign for the reader
		if (len >= sizeof(str) - truncatedLength)
		{
			snprintf(str + len, sizeof(str) - len, "---truncated");
		}

		switch (ll)
		{
			case LOG_ERROR:
				fprintf(stderr, "ERR %s\n", str);
				break;
			default:
				fprintf(stdout, "DL%d %s\n", ll, str);
		}
	}
}
