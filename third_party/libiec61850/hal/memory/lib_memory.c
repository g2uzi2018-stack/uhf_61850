/*
 *  lib_memory.c
 *
 *  Copyright 2014-2021 Michael Zillgith
 *
 *  This file is part of Platform Abstraction Layer (libpal)
 *  for libiec61850, libmms, and lib60870.
 */

#include <stdlib.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include "lib_memory.h"
#include "stack_config.h"

#ifndef CONFIG_IEC61850_MEMORY_LIMIT_BYTES
#define CONFIG_IEC61850_MEMORY_LIMIT_BYTES 0
#endif

typedef struct {
    size_t size;
    max_align_t alignment;
} MemoryAllocationHeader;

static _Atomic size_t allocatedBytes = 0;

static MemoryExceptionHandler exceptionHandler = NULL;
static void* exceptionHandlerParameter = NULL;

static void
noMemoryAvailableHandler(void)
{
    if (exceptionHandler != NULL)
        exceptionHandler(exceptionHandlerParameter);
}

static bool
reserveMemory(size_t size)
{
#if (CONFIG_IEC61850_MEMORY_LIMIT_BYTES > 0)
    const size_t limit = (size_t) CONFIG_IEC61850_MEMORY_LIMIT_BYTES;
    if (size > limit)
        return false;

    size_t current = atomic_load_explicit(&allocatedBytes, memory_order_relaxed);
    do {
        if (current > limit - size)
            return false;
    } while (!atomic_compare_exchange_weak_explicit(
        &allocatedBytes,
        &current,
        current + size,
        memory_order_relaxed,
        memory_order_relaxed));
#else
    (void) size;
#endif
    return true;
}

static void
releaseMemory(size_t size)
{
#if (CONFIG_IEC61850_MEMORY_LIMIT_BYTES > 0)
    atomic_fetch_sub_explicit(&allocatedBytes, size, memory_order_relaxed);
#else
    (void) size;
#endif
}

static void*
allocateMemory(size_t size, bool clear)
{
    if (size == 0)
        size = 1;
    if (size > SIZE_MAX - sizeof(MemoryAllocationHeader)) {
        noMemoryAvailableHandler();
        return NULL;
    }

    const size_t allocationSize = sizeof(MemoryAllocationHeader) + size;
    if (!reserveMemory(allocationSize)) {
        noMemoryAvailableHandler();
        return NULL;
    }

    MemoryAllocationHeader* header = clear
        ? (MemoryAllocationHeader*) calloc(1, allocationSize)
        : (MemoryAllocationHeader*) malloc(allocationSize);
    if (header == NULL) {
        releaseMemory(allocationSize);
        noMemoryAvailableHandler();
        return NULL;
    }

    header->size = allocationSize;
    return (void*) (header + 1);
}

void
Memory_installExceptionHandler(MemoryExceptionHandler handler, void* parameter)
{
    exceptionHandler = handler;
    exceptionHandlerParameter = parameter;
}

void*
Memory_malloc(size_t size)
{
    return allocateMemory(size, false);
}

void*
Memory_calloc(size_t nmemb, size_t size)
{
    if (nmemb != 0 && size > SIZE_MAX / nmemb) {
        noMemoryAvailableHandler();
        return NULL;
    }
    return allocateMemory(nmemb * size, true);
}

void *
Memory_realloc(void *ptr, size_t size)
{
    if (ptr == NULL)
        return Memory_malloc(size);
    if (size == 0) {
        Memory_free(ptr);
        return NULL;
    }
    if (size > SIZE_MAX - sizeof(MemoryAllocationHeader)) {
        noMemoryAvailableHandler();
        return NULL;
    }

    MemoryAllocationHeader* header = ((MemoryAllocationHeader*) ptr) - 1;
    const size_t oldSize = header->size;
    const size_t newSize = sizeof(MemoryAllocationHeader) + size;

    if (newSize > oldSize) {
        const size_t increase = newSize - oldSize;
        if (!reserveMemory(increase)) {
            noMemoryAvailableHandler();
            return NULL;
        }
        MemoryAllocationHeader* resized = (MemoryAllocationHeader*) realloc(header, newSize);
        if (resized == NULL) {
            releaseMemory(increase);
            noMemoryAvailableHandler();
            return NULL;
        }
        resized->size = newSize;
        return (void*) (resized + 1);
    }

    MemoryAllocationHeader* resized = (MemoryAllocationHeader*) realloc(header, newSize);
    if (resized == NULL)
    {
        noMemoryAvailableHandler();
        return NULL;
    }
    resized->size = newSize;
    releaseMemory(oldSize - newSize);
    return (void*) (resized + 1);
}

void
Memory_free(void* memb)
{
    if (memb != NULL) {
        MemoryAllocationHeader* header = ((MemoryAllocationHeader*) memb) - 1;
        const size_t size = header->size;
        free(header);
        releaseMemory(size);
    }
}
