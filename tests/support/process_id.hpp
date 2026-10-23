#pragma once

// The calling process's id, for unique scratch names, without <windows.h>.

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace rerevved::tests
{

inline unsigned long ProcessId()
{
#if defined(_WIN32)
    return static_cast<unsigned long>(_getpid());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

} // namespace rerevved::tests
