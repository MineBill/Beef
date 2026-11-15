#pragma once

#include <cstdarg>

namespace beflsp
{

// All diagnostic output goes to stderr. stdout is reserved for the JSON-RPC
// stream, so nothing else may be written there.
void Log(const char* fmt, ...);
void LogV(const char* fmt, va_list args);

}
