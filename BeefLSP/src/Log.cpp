#include "Log.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace beflsp
{

static std::mutex sLogMutex;

void LogV(const char* fmt, va_list args)
{
	std::lock_guard<std::mutex> lock(sLogMutex);
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
