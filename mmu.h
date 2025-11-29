#ifndef MEMORY_H
#define MEMORY_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <stdint.h>
#include <unistd.h>

/* CONFIG */
#define POOL_SIZE       4096
#define MAGIC_ALLOC     0xDEADBEEF
#define MAGIC_FREE      0xFEE1DEAD
#define MIN_BLOCK_SIZE  32
#define BUDDY_MAX_ORDER 12   // 1 << 12 == 4096

/* HEADER & METADATA IN-BLOCK */
typedef struct header {
    size_t size;       /* user payload size */
    uint32_t magic;    /* MAGIC_ALLOC / MAGIC_FREE */
    uint8_t is_free;   /* 1 if free */
} Header;

/* Free metadata placed immediately after header in free blocks.
   Separate pointers for address-sorted list and for buddy lists to avoid conflicts.
*/
typedef struct free_meta {
    /* Address-sorted doubly-linked list pointers */
    struct free_meta *addr_prev;
    struct free_meta *addr_next;

    /* Buddy singly-linked list pointer (for buddy-managed blocks only) */
    struct free_meta *buddy_next;

    /* Buddy order if this block is buddy-managed; -1 if not buddy */
    int order;

    /* reserved */
    void *reserved1;
    void *reserved2;
} FreeMeta;

/* Helper conversions */
static inline Header* header_from_user(void *p) {
    return (Header*)((char*)p - sizeof(Header));
}
static inline void* user_from_header(Header *h) {
    return (void*)((char*)h + sizeof(Header));
}
static inline FreeMeta* meta_from_header(Header *h) {
    return (FreeMeta*)((char*)h + sizeof(Header));
}
static inline Header* header_from_meta(FreeMeta *m) {
    return (Header*)((char*)m - sizeof(Header));
}
static inline void* block_end(Header *h) {
    return (char*)h + sizeof(Header) + h->size;
}

/* Globals */
static void *pool_base = NULL;
static int pool_initialized = 0;

/* Address-sorted free list head */
static FreeMeta *free_head = NULL;
/* Next-fit cursor */
static FreeMeta *next_fit_cursor = NULL;

/* Buddy free lists (index = order), uses buddy_next */
static FreeMeta *buddy_free_lists[BUDDY_MAX_ORDER + 1] = { NULL };

/* ---------- Initialization ---------- */
static void init_pool(void) {
    if (pool_initialized) return;

    void *p = mmap(NULL, POOL_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    pool_base = p;
    pool_initialized = 1;

    /* create a single free block occupying entire pool */
    Header *h = (Header*)pool_base;
    h->size = POOL_SIZE - sizeof(Header) - sizeof(FreeMeta);
    h->is_free = 1;
    h->magic = MAGIC_FREE;

    FreeMeta *fm = meta_from_header(h);
    fm->addr_prev = fm->addr_next = NULL;
    fm->buddy_next = NULL;
    fm->order = BUDDY_MAX_ORDER; /* whole pool is buddy order 12 */
    fm->reserved1 = fm->reserved2 = NULL;

    free_head = fm;
    next_fit_cursor = fm;

    /* init buddy free lists*/
    for (int i = 0; i <= BUDDY_MAX_ORDER; ++i) buddy_free_lists[i] = NULL;
    buddy_free_lists[BUDDY_MAX_ORDER] = fm;
}

//helpers for address sorted free list
static void insert_by_address(FreeMeta *fm) {
    if (!free_head) {
        fm->addr_prev = fm->addr_next = NULL;
        free_head = fm;
        return;
    }
    FreeMeta *cur = free_head;
    FreeMeta *prev = NULL;
    while (cur && cur < fm) {
        prev = cur;
        cur = cur->addr_next;
    }
    fm->addr_next = cur;
    fm->addr_prev = prev;
    if (prev) prev->addr_next = fm; else free_head = fm;
    if (cur) cur->addr_prev = fm;
}

static void remove_from_list(FreeMeta *fm) {
    if (!fm) return;
    if (fm->addr_prev) fm->addr_prev->addr_next = fm->addr_next;
    else free_head = fm->addr_next;
    if (fm->addr_next) fm->addr_next->addr_prev = fm->addr_prev;
    fm->addr_prev = fm->addr_next = NULL;
}

// Coalescing
static FreeMeta* coalesce(FreeMeta *fm) {
    if (!fm) return NULL;
    Header *h = header_from_meta(fm);

    /* Merge with previous if physically adjacent */
    if (fm->addr_prev) {
        FreeMeta *prev = fm->addr_prev;
        Header *ph = header_from_meta(prev);
        if ((char*)block_end(ph) == (char*)h) {
            /* extend prev to include fm */
            ph->size += sizeof(Header) + sizeof(FreeMeta) + h->size;
            remove_from_list(fm);
            fm = prev;
            h = ph;
        }
    }

    /* Merge with next if physically adjacent */
    if (fm->addr_next) {
        FreeMeta *next = fm->addr_next;
        Header *nh = header_from_meta(next);
        if ((char*)block_end(h) == (char*)nh) {
            /* extend h to include next */
            h->size += sizeof(Header) + sizeof(FreeMeta) + nh->size;
            remove_from_list(next);
        }
    }

    /* ensure fm meta field is valid */
    FreeMeta *res = meta_from_header(h);
    if (!res->addr_prev) res->addr_prev = fm->addr_prev; 
    return res;
}

static void split_block(Header *h, size_t req) {
    if (!h) return;
    size_t min_rem = sizeof(Header) + sizeof(FreeMeta) + MIN_BLOCK_SIZE;
    if (h->size < req + min_rem) return; /* too small to split */

    size_t remain = h->size - req - sizeof(Header) - sizeof(FreeMeta);
    h->size = req;

    Header *newh = (Header*)block_end(h);
    newh->size = remain;
    newh->is_free = 1;
    newh->magic = MAGIC_FREE;
    FreeMeta *fm = meta_from_header(newh);
    fm->addr_prev = fm->addr_next = NULL;
    fm->buddy_next = NULL;
    fm->order = -1; /* not buddy-managed unless created by buddy allocator */
    fm->reserved1 = fm->reserved2 = NULL;

    insert_by_address(fm);
}

void* malloc_first_fit(size_t size) {
    if (!pool_initialized) init_pool();
    FreeMeta *cur = free_head;
    while (cur) {
        Header *h = header_from_meta(cur);
        if (h->is_free && h->size >= size) {
            /* remove and allocate */
            remove_from_list(cur);
            split_block(h, size);
            h->is_free = 0;
            h->magic = MAGIC_ALLOC;
            /* mark meta as non-buddy */
            FreeMeta *fm = meta_from_header(h);
            fm->order = -1;
            return user_from_header(h);
        }
        cur = cur->addr_next;
    }
    return NULL;
}

void* malloc_next_fit(size_t size) {
    if (!pool_initialized) init_pool();
    if (!next_fit_cursor) next_fit_cursor = free_head;
    FreeMeta *start = next_fit_cursor ? next_fit_cursor : free_head;
    if (!start) return NULL;
    FreeMeta *cur = start;
    do {
        Header *h = header_from_meta(cur);
        if (h->is_free && h->size >= size) {
            remove_from_list(cur);
            split_block(h, size);
            h->is_free = 0;
            h->magic = MAGIC_ALLOC;
            FreeMeta *fm = meta_from_header(h);
            fm->order = -1;
            next_fit_cursor = cur->addr_next ? cur->addr_next : free_head;
            return user_from_header(h);
        }
        cur = cur->addr_next ? cur->addr_next : free_head;
    } while (cur != start);
    return NULL;
}

void* malloc_best_fit(size_t size) {
    if (!pool_initialized) init_pool();
    FreeMeta *cur = free_head;
    FreeMeta *best = NULL;
    while (cur) {
        Header *h = header_from_meta(cur);
        if (h->is_free && h->size >= size) {
            if (!best || h->size < header_from_meta(best)->size) best = cur;
        }
        cur = cur->addr_next;
    }
    if (!best) return NULL;
    Header *bh = header_from_meta(best);
    remove_from_list(best);
    split_block(bh, size);
    bh->is_free = 0;
    bh->magic = MAGIC_ALLOC;
    FreeMeta *fm = meta_from_header(bh);
    fm->order = -1;
    return user_from_header(bh);
}

void* malloc_worst_fit(size_t size) {
    if (!pool_initialized) init_pool();
    FreeMeta *cur = free_head;
    FreeMeta *worst = NULL;
    while (cur) {
        Header *h = header_from_meta(cur);
        if (h->is_free && h->size >= size) {
            if (!worst || h->size > header_from_meta(worst)->size) worst = cur;
        }
        cur = cur->addr_next;
    }
    if (!worst) return NULL;
    Header *wh = header_from_meta(worst);
    remove_from_list(worst);
    split_block(wh, size);
    wh->is_free = 0;
    wh->magic = MAGIC_ALLOC;
    FreeMeta *fm = meta_from_header(wh);
    fm->order = -1;
    return user_from_header(wh);
}

//  ---------- Buddy allocator helpers ---------- 
//  offset in bytes from pool_base 
static inline size_t header_offset(Header *h) {
    return (size_t)((char*)h - (char*)pool_base);
}
static inline Header* header_from_offset(size_t off) {
    return (Header*)((char*)pool_base + off);
}

/* find minimal order that fits payload + header + meta */
static inline int order_for_size_buddy(size_t payload) {
    size_t need = payload + sizeof(Header) + sizeof(FreeMeta);
    int order = 0;
    size_t sz = 1u;
    while (sz < need && order < BUDDY_MAX_ORDER) { sz <<= 1; order++; }
    if (sz < need) return -1;
    return order;
}

/* buddy list helpers */
static void buddy_push(size_t off, int order) {
    FreeMeta *m = meta_from_header(header_from_offset(off));
    m->buddy_next = buddy_free_lists[order];
    buddy_free_lists[order] = m;
}
static size_t buddy_pop(int order) {
    FreeMeta *m = buddy_free_lists[order];
    if (!m) return (size_t)-1;
    buddy_free_lists[order] = m->buddy_next;
    m->buddy_next = NULL;
    Header *h = header_from_meta(m);
    return header_offset(h);
}
static int buddy_remove_offset(int order, size_t off) {
    FreeMeta *cur = buddy_free_lists[order];
    FreeMeta *prev = NULL;
    while (cur) {
        Header *ch = header_from_meta(cur);
        if (header_offset(ch) == off) {
            if (prev) prev->buddy_next = cur->buddy_next;
            else buddy_free_lists[order] = cur->buddy_next;
            cur->buddy_next = NULL;
            return 1;
        }
        prev = cur;
        cur = cur->buddy_next;
    }
    return 0;
}

/* ---------- Buddy allocation ---------- */
void* malloc_buddy_alloc(size_t size) {
    if (!pool_initialized) init_pool();
    int order = order_for_size_buddy(size);
    if (order < 0 || order > BUDDY_MAX_ORDER) return NULL;

    /* finding available block at order j >= order */
    int j = order;
    while (j <= BUDDY_MAX_ORDER && buddy_free_lists[j] == NULL) j++;
    if (j > BUDDY_MAX_ORDER) return NULL;

    size_t off = buddy_pop(j);
    if (off == (size_t)-1) return NULL;

    while (j > order) {
        j--;
        size_t half = (size_t)1 << j; /* new block size in bytes */
        size_t right_off = off + half;
        /* initializing left and right headers */
        Header *left_h = header_from_offset(off);
        Header *right_h = header_from_offset(right_off);

        left_h->size = half - sizeof(Header) - sizeof(FreeMeta);
        left_h->is_free = 1;
        left_h->magic = MAGIC_FREE;
        FreeMeta *mleft = meta_from_header(left_h);
        mleft->addr_prev = mleft->addr_next = NULL;
        mleft->buddy_next = NULL;
        mleft->order = j;

        right_h->size = half - sizeof(Header) - sizeof(FreeMeta);
        right_h->is_free = 1;
        right_h->magic = MAGIC_FREE;
        FreeMeta *mright = meta_from_header(right_h);
        mright->addr_prev = mright->addr_next = NULL;
        mright->buddy_next = NULL;
        mright->order = j;
        buddy_push(right_off, j);
    }

    /* allocating final block at off */
    Header *h = header_from_offset(off);
    h->is_free = 0;
    h->magic = MAGIC_ALLOC;
    FreeMeta *fm = meta_from_header(h);
    fm->order = order;
    fm->addr_prev = fm->addr_next = NULL; /* not in address-sorted free list while allocated */
    fm->buddy_next = NULL;
    return user_from_header(h);
}

/* ---------- Buddy free/merge ---------- */
static void buddy_free(Header *h) {
    size_t off = header_offset(h);
    int order = meta_from_header(h)->order;
    if (order < 0 || order > BUDDY_MAX_ORDER) {
        /* not buddy-managed */
        return;
    }

    /* trying to merge upwards */
    while (order < BUDDY_MAX_ORDER) {
        size_t buddy_off = off ^ ((size_t)1 << order);
        /* if buddy is free (present in buddy_free_lists[order]) removing it and merging */
        if (!buddy_remove_offset(order, buddy_off)) break;
        /* merged block offset is min(off, buddy_off) */
        off = (off < buddy_off) ? off : buddy_off;
        order++;
        /* initializing merged header for next iteration */
        Header *merged = header_from_offset(off);
        merged->size = ((size_t)1 << order) - sizeof(Header) - sizeof(FreeMeta);
        merged->is_free = 1;
        merged->magic = MAGIC_FREE;
        FreeMeta *m = meta_from_header(merged);
        m->addr_prev = m->addr_next = NULL;
        m->buddy_next = NULL;
        m->order = order;
    }

    /* pushing the final merged (or original) block into buddy list */
    Header *final_h = header_from_offset(off);
    final_h->is_free = 1;
    final_h->magic = MAGIC_FREE;
    FreeMeta *fm = meta_from_header(final_h);
    fm->order = order;
    fm->addr_prev = fm->addr_next = NULL;
    fm->buddy_next = NULL;
    buddy_push(off, order);
}

/* ---------- Public free (detecting buddy vs general) ---------- */
void my_free(void *ptr) {
    if (!ptr) return;
    Header *h = header_from_user(ptr);
    if (!h) return;
    if (h->magic != MAGIC_ALLOC || h->is_free) {
        fprintf(stderr, "Invalid or double free\n");
        return;
    }

    /* mark free */
    h->is_free = 1;
    h->magic = MAGIC_FREE;

    FreeMeta *fm = meta_from_header(h);

    /* if this block was allocated by buddy allocator (order >=0), doing buddy free */
    if (fm->order >= 0 && fm->order <= BUDDY_MAX_ORDER) {
        buddy_free(h);
        return;
    }

    /* otherwise non-buddy, inserting into address list and coalesce */
    fm->addr_prev = fm->addr_next = NULL;
    fm->buddy_next = NULL;
    fm->order = -1;
    insert_by_address(fm);
    fm = coalesce(fm);
    (void)fm;
}

#endif 


