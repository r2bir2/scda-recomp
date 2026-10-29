/* POSIX stand-in for <intrin.h> (SCDA ARM port, docs Update 677). */
#pragma once
#include "platform/xbox_winnt.h"
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#include <x86intrin.h>
#endif
