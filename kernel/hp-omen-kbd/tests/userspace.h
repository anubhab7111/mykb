/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Userspace stand-ins for the kernel headers core.h uses, so the core can be
 * unit-tested outside the kernel. Never shipped and never part of the module.
 */
#ifndef HOK_USERSPACE_H
#define HOK_USERSPACE_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t s64;

#endif /* HOK_USERSPACE_H */
