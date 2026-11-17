#pragma once
// Single source for the application, CMake, resources and release packaging.
#define SPEEECIAL_VERSION "1.0.11"

#define SPEEECIAL_WIDEN_IMPL(value) L##value
#define SPEEECIAL_WIDEN(value) SPEEECIAL_WIDEN_IMPL(value)
#define SPEEECIAL_VERSION_W SPEEECIAL_WIDEN(SPEEECIAL_VERSION)
