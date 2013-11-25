#ifndef SHARED_HPP__
#define SHARED_HPP__

#if defined(_MSC_VER) || defined(EMSCRIPTEN)
#include <memory>
#ifdef _MSC_VER
#define snprintf _snprintf
#endif
#else
#include <tr1/memory>
#endif

#if defined(__QNX__) || defined(__CELLOS_LV2__) || defined(IOS)
namespace std1 = compat;
#elif !defined(EMSCRIPTEN)
namespace std1 = std::tr1;
#endif

void retro_stderr(const char *str);
void retro_stderr_print(const char *fmt, ...);

#endif

