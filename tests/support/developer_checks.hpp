#pragma once

// DEV_CHECK(expr) evaluates `expr` in a developer build and is true in a
// shipping build, where the diagnostics `expr` names do not exist. Use it for
// the diagnostic part of an assertion whose semantic part must run in both.

#ifndef REREVVED_DEVELOPER
#define REREVVED_DEVELOPER 0
#endif

#if REREVVED_DEVELOPER
#define DEV_CHECK(...) (__VA_ARGS__)
#else
#define DEV_CHECK(...) true
#endif

// The console variables a developer build adds exist only there: declare and set
// them through these, which are no-ops in a shipping build.
#if REREVVED_DEVELOPER
#define DEV_CVAR_DECLARE(type, name) REXCVAR_DECLARE(type, name)
#define DEV_CVAR_SET(name, value)    REXCVAR_SET(name, value)
#else
#define DEV_CVAR_DECLARE(type, name) static_assert(true)
#define DEV_CVAR_SET(name, value)    static_cast<void>(0)
#endif

// DEV_DO(statement) runs the statement in a developer build only.
#if REREVVED_DEVELOPER
#define DEV_DO(...) __VA_ARGS__
#else
#define DEV_DO(...) static_cast<void>(0)
#endif
