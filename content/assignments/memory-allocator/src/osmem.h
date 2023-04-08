/* SPDX-License-Identifier: BSD-3-Clause */

#pragma once

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include "printf.h"

#define ALIGNMENT 8
#define ALIGN(size) (((size) + (ALIGNMENT-1)) & ~(ALIGNMENT-1))
#define BLOCK_META_SIZE ALIGN(sizeof(struct block_meta))
#define MMAP_THRESHOLD (128 * 1024)

void *os_malloc(size_t size);
void os_free(void *ptr);
void *os_calloc(size_t nmemb, size_t size);
void *os_realloc(void *ptr, size_t size);
