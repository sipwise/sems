#pragma once

#include <log.h>
#include <sstream>

inline std::string facility()
{
#ifdef LOG_FACILITY
    return LOG_FACILITY;
#endif
    return "";
}

#define ENABLE_TRACE 0

#define LogDebug(args) { std::stringstream s; s << facility() << ": " args << std::endl; DBG("%s", s.str().c_str()); }
#define LogWarning(args) { std::stringstream s; s << facility() << ": " args << std::endl; WARN("%s", s.str().c_str()); }
#define LogInfo(args) { std::stringstream s; s << facility() << ": " args << std::endl; INFO("%s", s.str().c_str()); }
#define LogCritical(args) { std::stringstream s; s << facility() << ": " args << std::endl; CRIT("%s", s.str().c_str()); }
#define LogError(args) { std::stringstream s; s << facility() << ": " args << std::endl; ERROR("%s", s.str().c_str()); }
#define LogTrace(args) { if (ENABLE_TRACE) LogDebug(args); }


