#include "types.h"
#include "riscv.h"
#include "memlayout.h"
#include "defs.h"
#include "proc.h"
#include "param.h"

// Kernel page table (shared by all harts).
pagetable_t kernel_pagetable;

extern char etext[]; // kernel.ld provides this (end of kernel text).
extern char trampoline[]; // kernel.ld provides this (start of trampoline page).

// Forward declarations (avoid -Werror=implicit-function-declaration).
pte_t *walk(pagetable_t pagetable, uint64 va, int alloc);
int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm);
uint64 uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz);

static void kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm) {
    // va/pa/size must be page-aligned for mappages().
    if ((va % PGSIZE) || (pa % PGSIZE) || (sz % PGSIZE)) {
        panic("kvmmap: not aligned");
    }
    if (mappages(kpgtbl, va, sz, pa, perm) != 0) {
        panic("kvmmap: mappages");
    }
}

static pagetable_t kvmmake(void) {
    pagetable_t kpgtbl = (pagetable_t)kalloc();
    if (kpgtbl == 0) {
        panic("kvmmake: kalloc");
    }
    memset(kpgtbl, 0, PGSIZE);

    // Devices.
    kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);
    kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);
    kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

    // Kernel text is R-X.
    kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);
    // Kernel data and the physical RAM we'll make use of is R-W.
    kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext, PTE_R | PTE_W);

    // Trampoline code (mapped at a high virtual address).
    kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

    return kpgtbl;
}

void kvminit(void) {
    kernel_pagetable = kvmmake();
}

void kvminithart(void) {
    // Wait for any previous writes to the page-table memory to finish.
    sfence_vma();

    w_satp(MAKE_SATP(kernel_pagetable));

    // Flush stale entries from the TLB.
    sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
pte_t *walk(pagetable_t pagetable, uint64 va, int alloc) {
    if (va >= MAXVA) {
        panic("walk");
    }

    for (int level = 2; level > 0; level--) {
        pte_t *pte = &pagetable[PX(level, va)];
        if (*pte & PTE_V) {
            // find the next level page table.
            pagetable = (pagetable_t)PTE2PA(*pte);
        } else {
            if (!alloc) {
                return 0;
            }
            pagetable_t newpt = (pagetable_t)kalloc();
            if (newpt == 0) {
                return 0;
            }
            memset(newpt, 0, PGSIZE);
            *pte = PA2PTE(newpt) | PTE_V;
            pagetable = newpt;
        }
    }
    return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64 walkaddr(pagetable_t pagetable, uint64 va) {
    if (va >= MAXVA) {
        return 0;
    }
    pte_t *pte = walk(pagetable, va, 0);
    if (pte == 0) {
        return 0;
    }
    if ((*pte & PTE_V) == 0) {
        return 0;
    }
    if ((*pte & PTE_U) == 0) {
        return 0;
    }
    return PTE2PA(*pte);
}

static int should_lazy_alloc(struct proc *p, pagetable_t pagetable, uint64 va) {
    if (p == 0 || pagetable == 0 || p->trapframe == 0) {
        return 0;
    } // invalid p

    if (va >= MAXVA || va >= p->sz || va >= proc_mmap_limit(p)) {
        return 0;
    }
    uint64 guard = PGROUNDDOWN(p->trapframe->sp - 1) - PGSIZE;
    if (va == guard) {
        return 0;
    } // invalid address

    pte_t *pte = walk(pagetable, va, 0);
    if (pte != 0 && (*pte & PTE_V)) {
        return 0;
    } // exist valid PTE
    return 1;
}

int user_lazy_alloc(struct proc *p, pagetable_t pagetable, uint64 va) {
    uint64 page = PGROUNDDOWN(va);
    if (!should_lazy_alloc(p, pagetable, page)) {
        return -1;
    } // invalid lazy allocation
    void *mem = kalloc(); // allocate physical page
    if (mem == 0) {
        return -1;
    }
    memset(mem, 0, PGSIZE); // initialize
    if (mappages(pagetable, page, PGSIZE, (uint64)mem, PTE_R | PTE_W | PTE_U) != 0) {
        kfree(mem); // release any temporary resources on failure
        return -1;
    } // map page and give permissions
    return 0;
}


extern int lazy_alloc_enabled;
static uint64 lazy_alloc_walkaddr(pagetable_t pagetable, uint64 va, int write) {
    uint64 pa = walkaddr(pagetable, va);
    if (pa != 0 || !lazy_alloc_enabled) {
        return pa;
    } // walkaddr succeed

    struct proc *p = myproc();
    if (p == 0 || p->pagetable != pagetable) {
        return 0;
    }
    if (user_lazy_alloc(p, pagetable, va) < 0) {
        return 0;
    } // lazy allocation
    pa = walkaddr(pagetable, va);
    if (pa == 0) {
        return 0;
    }
    return pa;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm) {
    if ((va % PGSIZE) != 0) {
        panic("mappages: va not aligned");
    }
    if ((size % PGSIZE) != 0) {
        panic("mappages: size not aligned");
    }
    if (size == 0) {
        panic("mappages: size");
    }

    uint64 a = va;
    uint64 last = va + size - PGSIZE;
    for (;;) {
        pte_t *pte = walk(pagetable, a, 1);
        if (pte == 0) {
            return -1;
        }
        // Check if the pte is valid.
        if (*pte & PTE_V) {
            panic("mappages: remap");
        }
        *pte = PA2PTE(pa) | perm | PTE_V;
        if (a == last) {
            break;
        }
        a += PGSIZE;
        pa += PGSIZE;
    }
    return 0;
}

// Create an empty user page table.
// Returns 0 if out of memory.
pagetable_t uvmcreate(void) {
    pagetable_t pagetable = (pagetable_t)kalloc();
    if (pagetable == 0) {
        return 0;
    }
    memset(pagetable, 0, PGSIZE);
    return pagetable;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory.
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free) {
    if ((va % PGSIZE) != 0) {
        panic("uvmunmap: not aligned");
    }

    for (uint64 a = va; a < va + npages * PGSIZE; a += PGSIZE) {
        pte_t *pte = walk(pagetable, a, 0);
        if (pte == 0) {
            continue;
        }
        if ((*pte & PTE_V) == 0) {
            continue;
        }
        if (do_free) {
            uint64 pa = PTE2PA(*pte);
            kfree((void *)pa);
        }
        *pte = 0;
    }
}

// Allocate PTEs and physical memory to grow a process from oldsz to
// newsz, which need not be page aligned. Returns new size or 0 on error.
uint64 uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm) {
    if (newsz < oldsz) {
        return oldsz;
    }

    uint64 a = PGROUNDUP(oldsz);
    for (; a < newsz; a += PGSIZE) {
        void *mem = kalloc();
        if (mem == 0) {
            uvmdealloc(pagetable, a, oldsz);
            return 0;
        }
        memset(mem, 0, PGSIZE);
        if (mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R | PTE_U | xperm) != 0) {
            kfree(mem);
            uvmdealloc(pagetable, a, oldsz);
            return 0;
        }
    }
    return newsz;
}

// Deallocate user pages to bring the process size from oldsz to newsz.
// Returns the new process size.
uint64 uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz) {
    if (newsz >= oldsz) {
        return oldsz;
    }
    if (PGROUNDUP(newsz) < PGROUNDUP(oldsz)) {
        uint64 npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
        uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
    }
    return newsz;
}

int uvmmapped_pages(pagetable_t pagetable, uint64 sz) {
    int n = 0;

    for (uint64 va = 0; va < sz; va += PGSIZE) {
        pte_t *pte = walk(pagetable, va, 0);
        if (pte != 0 && (*pte & PTE_V) && (*pte & PTE_U)) {
            n++;
        }
    }

    return n;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
static void freewalk(pagetable_t pagetable) {
    for (int i = 0; i < 512; i++) {
        pte_t pte = pagetable[i];
        if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
            // This PTE points to a lower-level page table.
            uint64 child = PTE2PA(pte);
            freewalk((pagetable_t)child);
            pagetable[i] = 0;
        } else if (pte & PTE_V) {
            panic("freewalk: leaf");
        }
    }
    kfree((void *)pagetable);
}

// Free user memory pages, then free page-table pages.
void uvmfree(pagetable_t pagetable, uint64 sz) {
    if (sz > 0) {
        uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
    }
    freewalk(pagetable);
}

// Given a parent process's page table, copy its memory into a child's page table.
// With COW enabled: share physical pages read-only, mark with PTE_COW.
// Without COW: copy both page table and physical memory (original behavior).
// Returns 0 on success, -1 on failure (and frees any allocated pages).
int uvmcopy(pagetable_t old, pagetable_t new, uint64 sz) {
    for (uint64 i = 0; i < sz; i += PGSIZE) {
        pte_t *pte = walk(old, i, 0);
        if (pte == 0) {
            continue;
        }
        if ((*pte & PTE_V) == 0) {
            continue;
        }

        uint64 pa = PTE2PA(*pte);
        uint flags = (uint)PTE_FLAGS(*pte);

// If COW_ALLOC is enabled, fork should avoid unnecessary physical-page copies.
#if COW_ALLOC
        kaddref((void *)pa);
        if ((flags & PTE_W) || (flags & PTE_COW)) {
            uint cow_flags = (flags | PTE_COW) & ~PTE_W;
            *pte = PA2PTE(pa) | cow_flags;
            if (mappages(new, i, PGSIZE, pa, cow_flags) != 0) {
                kfree((void *)pa);
                goto err;
            }
        } // clear PTE_W and set PTE_COW
        else {
            if (mappages(new, i, PGSIZE, pa, flags) != 0) {
                goto err;
            }
        }
#else
        // Eager copy: allocate new page and copy data.
        void *mem = kalloc();
        if (mem == 0) {
            goto err;
        }
        memmove(mem, (void *)pa, PGSIZE);
        if (mappages(new, i, PGSIZE, (uint64)mem, flags) != 0) {
            kfree(mem);
            goto err;
        }
#endif
    }
    return 0;

err:
    uvmunmap(new, 0, PGROUNDUP(sz) / PGSIZE, 1);
    return -1;
}

int uvmcopyrange(pagetable_t old, pagetable_t new, uint64 start, uint64 len) {
    if ((start % PGSIZE) != 0 || (len % PGSIZE) != 0) {
        panic("uvmcopyrange: not aligned");
    }

    for (uint64 i = 0; i < len; i += PGSIZE) {
        uint64 va = start + i;
        pte_t *pte = walk(old, va, 0);
        if (pte == 0 || (*pte & PTE_V) == 0) {
            continue;
        }

        uint64 pa = PTE2PA(*pte);
        uint flags = (uint)PTE_FLAGS(*pte);
        void *mem = kalloc();
        if (mem == 0) {
            goto err;
        }
        memmove(mem, (void *)pa, PGSIZE);
        if (mappages(new, va, PGSIZE, (uint64)mem, flags) != 0) {
            kfree(mem);
            goto err;
        }
    }
    return 0;

err:
    uvmunmap(new, start, len / PGSIZE, 1);
    return -1;
}

// Clear user-accessible bit for the PTE at va.
// Used to create a guard page under the user stack.
void uvmclear(pagetable_t pagetable, uint64 va) {
    pte_t *pte = walk(pagetable, va, 0);
    if (pte == 0) {
        panic("uvmclear");
    }
    *pte &= ~PTE_U;
}

// Handle a COW page fault: allocate a new page, copy the old data,
// update the PTE. Returns 0 on success, -1 on failure.
// This is used by both usertrap (Store Page Fault) and copyout.
#if COW_ALLOC
// [COW] Handle a write fault by either restoring write permission when
// this process is the only owner, or copying data into a private page.
int cow_handle_fault(pagetable_t pagetable, uint64 va) {
    uint64 page = PGROUNDDOWN(va);
    pte_t *pte = walk(pagetable, page, 0);
    if (pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0) {
        return -1;
    }
    if ((*pte & PTE_COW) == 0 || (*pte & PTE_W) != 0) {
        return -1;
    } // check PTE labels

    uint64 pa = PTE2PA(*pte);
    uint flags = (uint)PTE_FLAGS(*pte);
    if (kgetref((void *)pa) == 1) {
        *pte = PA2PTE(pa) | ((flags | PTE_W) & ~PTE_COW);
        return 0;
    } // don't need to copy

    void *mem = kalloc();
    if (mem == 0) {
        return -1;
    }
    memmove(mem, (void *)pa, PGSIZE); // copy to new page
    *pte = PA2PTE(mem) | ((flags | PTE_W) & ~PTE_COW);
    kfree((void *)pa); // child don't need oldpa
    return 0;
}
#endif

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len) {
    while (len > 0) {
        uint64 va0 = PGROUNDDOWN(dstva);
        if (va0 >= MAXVA) {
            return -1;
        }
        uint64 pa0 = lazy_alloc_walkaddr(pagetable, va0, 1);
        if (pa0 == 0) {
            return -1;
        }
        pte_t *pte = walk(pagetable, va0, 0);
        if (pte == 0) {
            return -1;
        }
#if COW_ALLOC
        if ((*pte & PTE_COW) != 0) { // COW page
            if (cow_handle_fault(pagetable, va0) < 0) {
                return -1;
            }
            pa0 = walkaddr(pagetable, va0);
            pte = walk(pagetable, va0, 0);
            if (pa0 == 0 || pte == 0) {
                return -1;
            } // reget pa & pte
        }
#endif
        if ((*pte & PTE_W) == 0) {
            return -1;
        }

        uint64 n = PGSIZE - (dstva - va0);
        if (n > len) {
            n = len;
        }
        memmove((void *)(pa0 + (dstva - va0)), src, (uint)n);

        len -= n;
        src += n;
        dstva = va0 + PGSIZE;
    }
    return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len) {
    while (len > 0) {
        uint64 va0 = PGROUNDDOWN(srcva);
        uint64 pa0 = lazy_alloc_walkaddr(pagetable, va0, 0);
        if (pa0 == 0) {
            return -1;
        }

        uint64 n = PGSIZE - (srcva - va0);
        if (n > len) {
            n = len;
        }
        memmove(dst, (void *)(pa0 + (srcva - va0)), (uint)n);

        len -= n;
        dst += n;
        srcva = va0 + PGSIZE;
    }
    return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max) {
    int got_null = 0;

    while (got_null == 0 && max > 0) {
        uint64 va0 = PGROUNDDOWN(srcva);
        uint64 pa0 = lazy_alloc_walkaddr(pagetable, va0, 0);
        if (pa0 == 0) {
            return -1;
        }

        uint64 n = PGSIZE - (srcva - va0);
        if (n > max) {
            n = max;
        }

        char *p = (char *)(pa0 + (srcva - va0));
        while (n > 0) {
            if (*p == '\0') {
                *dst = '\0';
                got_null = 1;
                break;
            }
            *dst = *p;
            dst++;
            p++;
            n--;
            max--;
        }

        srcva = va0 + PGSIZE;
    }

    return got_null ? 0 : -1;
}
