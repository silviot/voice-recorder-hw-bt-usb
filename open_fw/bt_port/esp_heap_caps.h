/* heap_caps_* -> single BT arena (FreeRTOS heap_5 in .rtos_heap). Capabilities are ignored. SPDX-License-Identifier: MIT */
#pragma once
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_8BIT 1
#define MALLOC_CAP_INTERNAL 2
#define MALLOC_CAP_SPIRAM 4
#define MALLOC_CAP_DEFAULT 8
void *heap_caps_malloc(size_t size, uint32_t caps);
void *heap_caps_calloc(size_t n, size_t size, uint32_t caps);
void *heap_caps_realloc(void *p, size_t size, uint32_t caps);
void heap_caps_free(void *p);
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
size_t heap_caps_get_allocated_size(void *p);
#define esp_get_free_heap_size() heap_caps_get_free_size(0)
