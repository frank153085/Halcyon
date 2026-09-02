#pragma once

// Tracy is optional so the renderer remains buildable on machines that do
// not initialize the Tracy submodule.  Keeping the macro in a tiny project
// header lets call sites remain identical in instrumented and non-instrumented
// builds.

#ifndef HALCYON_ENABLE_TRACY
#define HALCYON_ENABLE_TRACY 0
#endif

#if HALCYON_ENABLE_TRACY
#include <tracy/Tracy.hpp>

#define HALCYON_PROFILE_SCOPE(name) ZoneScopedN(name)
#define HALCYON_PROFILE_FRAME() FrameMark
#define HALCYON_PROFILE_MESSAGE(text) TracyMessageL(text)
#else

#define HALCYON_PROFILE_SCOPE(name) ((void)0)
#define HALCYON_PROFILE_FRAME() ((void)0)
#define HALCYON_PROFILE_MESSAGE(text) ((void)0)
#endif
