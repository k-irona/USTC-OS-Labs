#include "types.h"
#include "riscv.h"
#include "proc.h"
#include "defs.h"
#include "memlayout.h"
#include "cpu.h"
#include "param.h"
#include "log.h"

extern void kernelvec(void);
extern void virtio_disk_isr(void);
extern char trampoline[];
extern char uservec[];
extern char userret[];

static struct spinlock tickslock;
static uint ticks;

static void clearsip_ssip(void) {
    // Clear SSIP (bit 1). Other bits may be read-only.
    w_sip(r_sip() & ~2L);
}

static void on_timer_tick(void) {
    // Keep a single global tick source on hart 0.
    if (cpuid() != 0) {
        return;
    }
    acquire(&tickslock);
    ticks++;
    wakeup(&ticks);
    release(&tickslock);
}

uint ticks_get(void) {
    acquire(&tickslock);
    uint t = ticks;
    release(&tickslock);
    return t;
}

int ticks_sleep(uint n) {
    acquire(&tickslock);
    uint start = ticks;
    while ((uint)(ticks - start) < n) {
        struct proc *p = myproc();
        if (p && p->killed) {
            release(&tickslock);
            return -1;
        }
        sleep(&ticks, &tickslock);
    }
    release(&tickslock);
    return 0;
}

void trap_init(void) {
    if (cpuid() == 0) {
        static int once = 0;
        if (!once) {
            initlock(&tickslock, "ticks");
            ticks = 0;
            once = 1;
        }
    }
    w_stvec((uint64)kernelvec);
    clearsip_ssip();
    LOG_INFO("trap_init done on hart %d", cpuid());
    // Don't enable interrupts here; let the scheduler do it.
}

void usertrap(void) {
    struct proc *p = myproc();
    if (p == 0 || p->trapframe == 0) {
        panic("usertrap: no proc");
    }

    // Must have trapped from U-mode.
    if (r_sstatus() & SSTATUS_SPP) {
        panic("usertrap: not from user");
    }

    // Use the kernel trap vector while we're in the kernel.
    w_stvec((uint64)kernelvec);

    uint64 scause = r_scause();
    p->trapframe->epc = r_sepc();

    if (scause == SCAUSE_ECALL_U) {
        if (p->killed) {
            proc_exit(-1);
        }
        // Skip the ecall instruction.
        p->trapframe->epc += 4;
        // Allow interrupts while executing syscall handlers.
        intr_on();
        syscall();
    } else if (scause >> 63) {
        // Interrupt.
        uint64 code = scause & 0xfff;
        if (code == SCAUSE_SSI) {
            clearsip_ssip();
            on_timer_tick();
            if (p->state == RUNNING) {
                yield();
            }
        } else if (code == SCAUSE_SEI) {
            int irq = plic_claim();
            if (irq == UART0_IRQ) {
                uart_isr();
            } else if (irq == VIRTIO0_IRQ) {
                virtio_disk_isr();
            }
            if (irq) {
                plic_complete(irq);
            }
        } else {
            printf("[usertrap] unknown interrupt scause=%p\n", scause);
        }
    } else {
        // Exception handling.
        uint64 code = scause & 0xfff;
        if (code == LOAD_PAGE_FAULT || code == STORE_PAGE_FAULT) {
            // Lazy allocation / COW: classify user page faults and
            // dispatch them to the correct memory-management path.
            // Page Fault: code 13 = Load Page Fault, code 15 = Store/AMO Page Fault.
            uint64 fault_va = r_stval();

            int handled = 0;
#if COW_ALLOC
            if (code == STORE_PAGE_FAULT) {
                if (cow_handle_fault(p->pagetable, fault_va) == 0) {
                    handled = 1;
                }
            } // try COW handle first
#endif

            if (!handled) {
                if (proc_handle_page_fault(fault_va, code == STORE_PAGE_FAULT) == 0) {
                    handled = 1;
                }
            } // if not handled by COW

            if (!handled) {
                p->killed = 1;
            } // unable to fix, kill the process
        } else {
            printf("[usertrap] scause=%p sepc=%p stval=%p\n", scause, r_sepc(), r_stval());
            p->killed = 1;
        }
    }

    if (p->killed) {
        proc_exit(-1);
    }
    usertrapret();
}

void usertrapret(void) {
    struct proc *p = myproc();
    if (p == 0 || p->trapframe == 0 || p->pagetable == 0) {
        panic("usertrapret: no proc");
    }

    intr_off();

    // Set up trap vector for user mode.
    uint64 vec = TRAMPOLINE + (uint64)(uservec - trampoline);
    w_stvec(vec);

    // Set up trampoline info to get back into the kernel on the next trap.
    p->trapframe->kernel_satp = r_satp();
    p->trapframe->kernel_sp = p->kstack_top;
    p->trapframe->kernel_trap = (uint64)usertrap;
    p->trapframe->kernel_hartid = (uint64)cpuid();

    // Set up sstatus for sret to user mode.
    uint64 x = r_sstatus();
    x &= ~SSTATUS_SPP;   // clear SPP to return to U-mode
    x |= SSTATUS_SPIE;   // enable interrupts in U-mode
    x &= ~SSTATUS_FS_MASK;
    x |= SSTATUS_FS_DIRTY;
    w_sstatus(x);

    w_sepc(p->trapframe->epc);

    // Jump into the trampoline's userret.
    uint64 fn = TRAMPOLINE + (uint64)(userret - trampoline);
    uint64 user_satp = MAKE_SATP(p->pagetable);
    ((void (*)(uint64, uint64))fn)(TRAPFRAME, user_satp);

    panic("usertrapret");
}

void kerneltrap(struct trapframe_s *tf) {
    (void)tf;
    uint64 scause = r_scause();
    uint64 sepc = r_sepc();
    uint64 sstatus = r_sstatus();

    // 判断是否为中断 (最高位为1)
    if (scause >> 63) {
        // ... (原有的中断处理逻辑保持不变)
        uint64 code = scause & 0xfff;
        if (code == SCAUSE_SSI) {
            clearsip_ssip();
            on_timer_tick();
            struct proc *p = myproc();
            if (p && p->state == RUNNING) {
                yield(); 
            }
            w_sepc(sepc);
            w_sstatus(sstatus);
            return;
        } else if (code == SCAUSE_SEI) {
            int irq = plic_claim();
            if (irq == UART0_IRQ) {
                uart_isr();
            } else if (irq == VIRTIO0_IRQ) {
                virtio_disk_isr();
            } else if (irq) {
                printf("[kerneltrap] unexpected irq=%d\n", irq);
            }
            if (irq) {
                plic_complete(irq);
            }
            w_sepc(sepc);
            w_sstatus(sstatus);
            return;
        }
    } else {
        // 处理异常 (Exception)
        uint64 code = scause & 0xfff;
        
        // Kernel should never execute ECALL for syscalls.
        if (code == SCAUSE_ECALL_S) {
            panic("kerneltrap: ecall");
        }
    }

    printf("[trap] scause=%p sepc=%p stval=%p sip=%p\n", scause, r_sepc(), r_stval(), r_sip());
    panic("kerneltrap");
}
