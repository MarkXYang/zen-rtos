// ZenRTOS 192 KB 16-Byte-Aligned Free-List Heap Allocator for ESP32-C6 Wi-Fi
// Thread-safe & ISR-safe via rtos_irq_save() / rtos_irq_restore().

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "rtos.h"

#ifndef WIFI_HEAP_SIZE_BYTES
#define WIFI_HEAP_SIZE_BYTES (192U * 1024U)
#endif

#define HEAP_ALIGN 16U
#define HEAP_MAGIC 0x5A454E48UL // "ZENH"

typedef struct heap_block {
    uint32_t           magic;
    uint32_t           size; // Payload size in bytes (excluding header)
    struct heap_block *next;
    uint32_t           used; // 1 = allocated, 0 = free
} heap_block_t;

static uint8_t g_wifi_heap_arena[WIFI_HEAP_SIZE_BYTES] __attribute__((aligned(HEAP_ALIGN)));
static heap_block_t *g_heap_head = NULL;

static size_t align_up(size_t n) {
    return (n + (HEAP_ALIGN - 1U)) & ~(size_t)(HEAP_ALIGN - 1U);
}

static void heap_ensure_init(void) {
    if (g_heap_head != NULL) return;
    g_heap_head = (heap_block_t *)g_wifi_heap_arena;
    g_heap_head->magic = HEAP_MAGIC;
    g_heap_head->size  = WIFI_HEAP_SIZE_BYTES - sizeof(heap_block_t);
    g_heap_head->next  = NULL;
    g_heap_head->used  = 0;
}

void *wifi_heap_malloc(size_t size) {
    if (size == 0) size = 1;
    size_t req = align_up(size);

    uint32_t flags = rtos_irq_save();
    heap_ensure_init();

    heap_block_t *curr = g_heap_head;
    while (curr != NULL) {
        if (!curr->used && curr->size >= req) {
            if (curr->size >= req + sizeof(heap_block_t) + HEAP_ALIGN) {
                heap_block_t *split = (heap_block_t *)((uint8_t *)(curr + 1) + req);
                split->magic = HEAP_MAGIC;
                split->size  = curr->size - (uint32_t)req - (uint32_t)sizeof(heap_block_t);
                split->next  = curr->next;
                split->used  = 0;
                curr->size   = (uint32_t)req;
                curr->next   = split;
            }
            curr->used = 1;
            rtos_irq_restore(flags);
            return (void *)(curr + 1);
        }
        curr = curr->next;
    }

    rtos_irq_restore(flags);
    return NULL;
}

void *wifi_heap_calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    if (nmemb != 0 && total / nmemb != size) return NULL;
    void *ptr = wifi_heap_malloc(total);
    if (ptr != NULL) {
        uint8_t *p = (uint8_t *)ptr;
        for (size_t i = 0; i < total; i++) {
            p[i] = 0;
        }
    }
    return ptr;
}

void wifi_heap_free(void *ptr) {
    if (ptr == NULL) return;
    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t base = (uintptr_t)g_wifi_heap_arena;
    if (addr < base + sizeof(heap_block_t) || addr >= base + WIFI_HEAP_SIZE_BYTES) {
        return;
    }

    uint32_t flags = rtos_irq_save();
    heap_block_t *blk = ((heap_block_t *)ptr) - 1;
    if (blk->magic != HEAP_MAGIC) {
        rtos_irq_restore(flags);
        return;
    }
    blk->used = 0;

    // Coalesce adjacent free blocks across the list
    heap_block_t *curr = g_heap_head;
    while (curr != NULL && curr->next != NULL) {
        if (!curr->used && !curr->next->used) {
            curr->size += (uint32_t)sizeof(heap_block_t) + curr->next->size;
            curr->next = curr->next->next;
        } else {
            curr = curr->next;
        }
    }
    rtos_irq_restore(flags);
}

void *wifi_heap_realloc(void *ptr, size_t size) {
    if (ptr == NULL) return wifi_heap_malloc(size);
    if (size == 0) {
        wifi_heap_free(ptr);
        return NULL;
    }

    heap_block_t *blk = ((heap_block_t *)ptr) - 1;
    if (blk->magic != HEAP_MAGIC) return NULL;
    if (blk->size >= size) return ptr;

    void *new_ptr = wifi_heap_malloc(size);
    if (!new_ptr) return NULL;

    uint8_t *dst = (uint8_t *)new_ptr;
    const uint8_t *src = (const uint8_t *)ptr;
    for (uint32_t i = 0; i < blk->size; i++) {
        dst[i] = src[i];
    }
    wifi_heap_free(ptr);
    return new_ptr;
}

uint32_t wifi_heap_free_bytes(void) {
    uint32_t flags = rtos_irq_save();
    heap_ensure_init();
    uint32_t total = 0;
    for (heap_block_t *curr = g_heap_head; curr != NULL; curr = curr->next) {
        if (!curr->used) {
            total += curr->size;
        }
    }
    rtos_irq_restore(flags);
    return total;
}
