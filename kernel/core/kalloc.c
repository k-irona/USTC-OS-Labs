#include "types.h"
#include "riscv.h"
#include "spinlock.h"
#include "memlayout.h"
#include "defs.h"
#include "param.h"
#include "log.h"

// Physical page allocator.
//
// Frees physical memory between the end of the kernel and PHYSTOP, and
// hands out 4096-byte pages.

extern char end[]; // provided by kernel.ld

struct run {
    struct run *next;
};

static struct {
    struct spinlock lock;
    struct run *freelist;
} kmem;

// COW reference-counting state. Students may keep this representation or
// replace it with an equivalent one.
#if COW_ALLOC
static struct spinlock ref_lock;
static int ref_cnt[PHYSTOP / PGSIZE];

static int pa_index(void *pa) {
    return (int)((uint64)pa / PGSIZE);
}

// Increase the reference count for a physical page.
void kaddref(void *pa) {
    acquire(&ref_lock);
    ref_cnt[pa_index(pa)]++;
    release(&ref_lock);
}

// Get the reference count for a physical page.
int kgetref(void *pa) {
    acquire(&ref_lock);
    int n = ref_cnt[pa_index(pa)];
    release(&ref_lock);
    return n;
}
#else
// Stubs when COW is disabled.
void kaddref(void *pa) { (void)pa; }
int kgetref(void *pa) { (void)pa; return 1; }
#endif

void kfree(void *pa) {
    uint64 a = (uint64)pa;
    if ((a % PGSIZE) != 0) {
        panic("kfree: not aligned");
    }
    if (a < (uint64)end || a >= PHYSTOP) {
        panic("kfree: bad pa");
    }

#if COW_ALLOC
    acquire(&ref_lock);
    int idx = pa_index(pa);
    if (ref_cnt[idx] < 1) {
        release(&ref_lock);
        panic("kfree: ref");
    }
    ref_cnt[idx]--;
    int refs = ref_cnt[idx];
    release(&ref_lock);
    if (refs > 0) {
        return;
    } // only the last release continue to the normal free path below
#endif

    // Fill with junk to catch dangling refs.
    memset(pa, 1, PGSIZE);

    struct run *r = (struct run *)pa;

    acquire(&kmem.lock);
    r->next = kmem.freelist;
    kmem.freelist = r;
    release(&kmem.lock);
}

void kinit(void) {
    initlock(&kmem.lock, "kmem");
    kmem.freelist = 0;

#if COW_ALLOC
    // COW metadata must be initialized before physical pages are handed out.
    initlock(&ref_lock, "ref");
    for (int i = 0; i < PHYSTOP / PGSIZE; i++) {
        ref_cnt[i] = 0;
    }
#endif

    // Free every page after the kernel.
    uint64 p = PGROUNDUP((uint64)end);
    for (; p + PGSIZE <= PHYSTOP; p += PGSIZE) {
#if COW_ALLOC
        ref_cnt[pa_index((void *)p)] = 1;
#endif //avoid panic
        kfree((void *)p);
    }
}

void *kalloc(void) {
    acquire(&kmem.lock);
    struct run *r = kmem.freelist;
    if (r) {
        kmem.freelist = r->next;
    }
    release(&kmem.lock);

    if (r) {
        // Fill with junk to help spot uninitialized use.
        memset((void *)r, 5, PGSIZE);
#if COW_ALLOC
        acquire(&ref_lock);
        ref_cnt[pa_index((void *)r)] = 1;
        release(&ref_lock);
#endif
        LOG_DEBUG("Allocated physical page at %p", r); // [埋点]
    } else {
        LOG_ERROR("kalloc out of memory!");
    }
    return (void *)r;
}

int kfreepage_count(void) {
    int n = 0;

    acquire(&kmem.lock);
    for (struct run *r = kmem.freelist; r != 0; r = r->next) {
        n++;
    }
    release(&kmem.lock);

    return n;
}

int ktotalpage_count(void) {
    uint64 first = PGROUNDUP((uint64)end);
    return (int)((PHYSTOP - first) / PGSIZE);
}
