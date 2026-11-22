#ifndef _XH_STDINT_H
#define _XH_STDINT_H
typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef short int16_t;
typedef unsigned short uint16_t;
typedef int int32_t;
typedef unsigned int uint32_t;
typedef long long int64_t;
typedef unsigned long long uint64_t;
typedef int intptr_t;
typedef unsigned int uintptr_t;
#define INT32_MAX 2147483647
#define UINT32_MAX 4294967295U
#define INT64_MAX 9223372036854775807LL
#define UINT64_MAX 18446744073709551615ULL
#define INT32_C(v) v
#define UINT32_C(v) v##U
#define INT64_C(v) v##LL
#define UINT64_C(v) v##ULL
#endif
