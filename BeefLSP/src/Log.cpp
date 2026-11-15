#include "Log.h"

#include <cstdarg>
#include <cstdio>

namespace beflsp
{

void LogV(const char* fmt, va_list args)
{
	fputs("[beeflsp] ", stderr);
	vfprintf(stderr, fmt, args);
	fputc('\n', stderr);
	fflush(stderr);
}

void Log(const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	LogV(fmt, args);
	va_end(args);
}

}
