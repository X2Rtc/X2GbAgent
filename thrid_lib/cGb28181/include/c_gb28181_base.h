#ifndef C_GB28181_API_BASE_H
#define C_GB28181_API_BASE_H

#if defined(_WIN32) || defined(_WIN64)
#if defined(C_GB28181_API_EXPORT)
#define C_GB28181_API __declspec(dllexport)
#else
#define C_GB28181_API __declspec(dllimport)
#endif
#define C_GB28181_CALL __cdecl
#else
#if defined(__GNUC__) && __GNUC__ >= 4
#define C_GB28181_API __attribute__((visibility("default")))
#else
#define C_GB28181_API
#endif
#define C_GB28181_CALL
#endif

#endif
