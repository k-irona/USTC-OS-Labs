#include "types.h"
#include "defs.h"
#include "proc.h"
#include "spinlock.h"

#define AI_NREQ 8
#define AI_MAX_TOKENS 255
#define AI_MAX_PREDICT 32
#define AI_MAX_RESULT 255

enum ai_req_state {
    AIREQ_UNUSED = 0, // 空闲
    AIREQ_NEW,        // 已创建
    AIREQ_READY,      // 准备就绪
    AIREQ_RUNNING,    // 正在执行
    AIREQ_DONE,       // 执行完成
    AIREQ_FAILED,     // 执行失败
};

struct ai_request {
    int id;                         // 请求编号
    int owner_pid;                  // 提交者 pid
    int state;                      // 请求状态
    int err;                        // 错误码
    int token_count;                // 输入 token 数量
    int predict_count;              // 生成 token 数量
    int result_len;                 // 结果长度
    uint32 tokens[AI_MAX_TOKENS];   // 输入 token 缓冲
    char result[AI_MAX_RESULT + 1]; // 输出结果缓冲
};

struct ai_service {
    struct spinlock lock;           // 服务锁
    int next_id;                    // 下一个请求编号
    int worker_pid;                 // worker pid
    int worker_online;              // worker 在线标记
    int q[AI_NREQ];                 // 请求队列，保存槽位下标
    int qhead;                      // 队头
    int qtail;                      // 队尾
    int qcount;                     // 队列长度
    struct ai_request reqs[AI_NREQ]; // 请求表
} aisvc;

/*
 * Student starter guide:
 *
 * This branch keeps the current AIOS architecture:
 *
 *   user -> ai_call/ai_submit/ai_wait/ai_query
 *        -> kernel ai_service
 *        -> ai_daemon via ai_worker_* syscalls
 *        -> llm runtime
 *
 * You do not need to change user/ai_daemon.c or the llm runtime for the lab.
 * Your main job is to complete the kernel control plane in this file.
 *
 * Suggested staging:
 *
 * - Starter sync smoke path:
 *   ai_service_call() is already provided so students can quickly check
 *   the environment and syscall path before touching the real service.
 *
 * - Part 1:
 *   Build the real producer/consumer service path:
 *     ai_service_enqueue_tokens()
 *     ai_service_worker_register()
 *     ai_service_worker_get()
 *     ai_service_worker_complete()
 *
 * - Part 2:
 *   Implement isolation and result semantics:
 *     ai_find_req_locked()
 *     ai_service_query()
 *     ai_service_wait()
 *   After those semantics are stable, do a final ai_call() cleanup:
 *     ai_service_call()
 *
 * The rest of the system is intentionally kept complete so you can focus on
 * OS concerns: request objects, sleep/wakeup, ownership, and one-shot result
 * consumption.
 */

static void ai_req_reset(struct ai_request *req) {
    memset(req, 0, sizeof(*req));
    req->state = AIREQ_UNUSED;
}

static __attribute__((unused)) int ai_req_busy(const struct ai_request *req) {
    return req->state == AIREQ_NEW || req->state == AIREQ_READY || req->state == AIREQ_RUNNING;
}

static __attribute__((unused)) struct ai_request *ai_find_slot_locked(void) {
    for (int i = 0; i < AI_NREQ; i++) {
        if (aisvc.reqs[i].state == AIREQ_UNUSED) {
            return &aisvc.reqs[i];
        }
    }
    return 0;
}

static __attribute__((unused)) struct ai_request *ai_find_req_locked(int reqid, int owner_pid) {
    for (int i = 0; i < AI_NREQ; i++) {
        struct ai_request *req = &aisvc.reqs[i];
        if (req->state == AIREQ_UNUSED || req->id != reqid) {
            continue;
        }

        /*
         * TODO(Part2):
         * Only return the request when owner_pid matches req->owner_pid.
         * This is the core ownership check used by query()/wait().
         */
        (void)owner_pid;
        return req;
    }
    return 0;
}

static __attribute__((unused)) struct ai_request *ai_find_req_by_id_locked(int reqid) {
    for (int i = 0; i < AI_NREQ; i++) {
        struct ai_request *req = &aisvc.reqs[i];
        if (req->state != AIREQ_UNUSED && req->id == reqid) {
            return req;
        }
    }
    return 0;
}

static int ai_append_text(char *out, int out_cap, int *pos, const char *text) {
    while (*text != '\0') {
        if (*pos >= out_cap - 1) {
            return -1;
        }
        out[*pos] = *text;
        (*pos)++;
        text++;
    }
    out[*pos] = '\0';
    return 0;
}

static int ai_append_u32(char *out, int out_cap, int *pos, uint32 value) {
    char digits[16];
    int nd = 0;

    do {
        digits[nd++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value != 0 && nd < (int)sizeof(digits));

    if (*pos + nd >= out_cap) {
        return -1;
    }
    for (int i = nd - 1; i >= 0; i--) {
        out[*pos] = digits[i];
        (*pos)++;
    }
    out[*pos] = '\0';
    return 0;
}

/*
 * Course-provided helper for the starter sync smoke path.
 *
 * This is intentionally not the real ai_daemon path. Students do not need to
 * discover or call it themselves: ai_service_call() uses it until the final
 * Part 2 cleanup turns ai_call() into a real submit + wait wrapper.
 */
static __attribute__((unused)) int ai_sync_smoke_placeholder(
    const uint32 *tokens,
    int token_count,
    int predict_count,
    char *out,
    int out_cap
) {
    int pos = 0;

    if (out_cap <= 0) {
        return -1;
    }
    out[0] = '\0';

    if (ai_append_text(out, out_cap, &pos, "sync-smoke ") < 0) {
        return -1;
    }
    if (ai_append_text(out, out_cap, &pos, "predict=") < 0) {
        return -1;
    }
    if (ai_append_u32(out, out_cap, &pos, (uint32)predict_count) < 0) {
        return -1;
    }
    if (ai_append_text(out, out_cap, &pos, " prompt=") < 0) {
        return -1;
    }

    for (int i = 0; i < token_count && i < 4; i++) {
        if (i > 0 && ai_append_text(out, out_cap, &pos, "-") < 0) {
            return -1;
        }
        if (ai_append_u32(out, out_cap, &pos, tokens[i]) < 0) {
            return -1;
        }
    }

    if (token_count > 4 && ai_append_text(out, out_cap, &pos, "-more") < 0) {
        return -1;
    }
    return pos;
}

void ai_service_init(void) {
    initlock(&aisvc.lock, "ai_service");
    aisvc.next_id = 1;
    aisvc.worker_pid = 0;
    aisvc.worker_online = 0;
    aisvc.qhead = 0;
    aisvc.qtail = 0;
    aisvc.qcount = 0;
    for (int i = 0; i < AI_NREQ; i++) {
        ai_req_reset(&aisvc.reqs[i]);
        aisvc.q[i] = -1;
    }
}

static int ai_service_enqueue_tokens(uint64 token_uva, int token_count, int predict_count, int *reqid_out) {
    struct proc *p = myproc();
    if (p == 0 || p->pagetable == 0) {
        return -1;
    }
    if (token_count <= 0 || token_count > AI_MAX_TOKENS || predict_count <= 0 || predict_count > AI_MAX_PREDICT) {
        return -1;
    }

    uint32 tokens[AI_MAX_TOKENS];
    memset(tokens, 0, sizeof(tokens));
    if (copyin(p->pagetable, (char *)tokens, token_uva, (uint64)token_count * sizeof(uint32)) < 0) {
        return -1;
    }

    acquire(&aisvc.lock);

    /*
     * TODO(Part1):
     * 1. Reject the submission if no ai_daemon worker has registered yet.
     * 2. Sleep until ai_find_slot_locked() finds a free request slot.
     * 3. Initialize a fresh request object:
     *      - id / owner_pid
     *      - token_count / predict_count
     *      - err / result_len
     *      - tokens[] contents
     *      - state transition NEW -> READY
     * 4. Push the request's slot index into the circular queue.
     * 5. Wake the worker sleeping on qcount and return the reqid.
     */

    release(&aisvc.lock);
    (void)reqid_out;
    return -1;
}

int ai_service_worker_register(void) {
    struct proc *p = myproc();
    if (p == 0 || p->pagetable == 0) {
        return -1;
    }

    acquire(&aisvc.lock);

    /*
     * TODO(Part1):
     * Register the calling process as the unique ai_daemon worker.
     * Reject the call if some other worker is already online.
     */

    release(&aisvc.lock);
    return -1;
}

int ai_service_worker_get(uint64 token_uva, int token_cap, uint64 reqid_uva, uint64 predict_uva) {
    struct proc *p = myproc();
    if (p == 0 || p->pagetable == 0 || token_cap <= 0 || token_cap > AI_MAX_TOKENS ||
        reqid_uva == 0 || predict_uva == 0) {
        return -1;
    }

    acquire(&aisvc.lock);

    /*
     * TODO(Part1):
     * 1. Check that the caller is the registered ai_daemon worker.
     * 2. Sleep while the request queue is empty.
     * 3. Pop one slot index from the circular queue.
     * 4. Mark that request RUNNING.
     * 5. Copy req->tokens, req->token_count, req->predict_count, req->id
     *    into local kernel buffers before releasing the lock.
     * 6. Handle token_cap being too small.
     * 7. copyout the token array, reqid, and predict_count to user space.
     * 8. If any copyout fails, transition the request to FAILED and wake waiters.
     * 9. Return token_count on success.
     */

    release(&aisvc.lock);
    return -1;
}

int ai_service_worker_complete(int reqid, uint64 out_uva, int out_len, int status) {
    struct proc *p = myproc();
    if (p == 0 || p->pagetable == 0 || reqid <= 0) {
        return -1;
    }

    char result[AI_MAX_RESULT + 1];
    memset(result, 0, sizeof(result));

    acquire(&aisvc.lock);

    /*
     * TODO(Part1):
     * 1. Check that the caller is the registered ai_daemon worker.
     * 2. Find the request by reqid and make sure it is RUNNING.
     * 3. If status == 0, copy the generated text from out_uva into result[].
     * 4. Re-check the request after reacquiring the lock.
     * 5. On success:
     *      - set err = 0
     *      - set result_len
     *      - copy result[] into req->result
     *      - move state to DONE
     * 6. On failure:
     *      - set err to a negative value
     *      - clear result_len
     *      - move state to FAILED
     * 7. Wake up any process sleeping in ai_wait().
     */

    release(&aisvc.lock);
    return -1;
}

void ai_service_proc_exit(int pid) {
    if (pid <= 0) {
        return;
    }

    acquire(&aisvc.lock);
    if (!aisvc.worker_online || aisvc.worker_pid != pid) {
        release(&aisvc.lock);
        return;
    }

    aisvc.worker_online = 0;
    aisvc.worker_pid = 0;
    for (int i = 0; i < AI_NREQ; i++) {
        struct ai_request *req = &aisvc.reqs[i];
        if (ai_req_busy(req)) {
            req->err = -1;
            req->result_len = 0;
            req->state = AIREQ_FAILED;
            wakeup(req);
        }
        aisvc.q[i] = -1;
    }
    aisvc.qhead = 0;
    aisvc.qtail = 0;
    aisvc.qcount = 0;
    wakeup(&aisvc);
    wakeup(&aisvc.qcount);
    release(&aisvc.lock);
}

int ai_service_submit(uint64 token_uva, int token_count, int predict_count) {
    int reqid = -1;
    if (ai_service_enqueue_tokens(token_uva, token_count, predict_count, &reqid) < 0) {
        return -1;
    }
    return reqid;
}

int ai_service_query(int reqid, uint64 st_uva) {
    struct proc *p = myproc();
    if (p == 0 || p->pagetable == 0 || reqid <= 0) {
        return -1;
    }

    /*
     * TODO(Part2):
     * 1. Find the request with ai_find_req_locked(reqid, p->pid).
     * 2. Refuse to expose status for a request owned by someone else.
     * 3. Fill a struct ai_status with reqid/state/err/result_len.
     * 4. copyout that status structure to st_uva.
     */
    (void)st_uva;
    return -1;
}

int ai_service_wait(int reqid, uint64 out_uva, int out_cap) {
    struct proc *p = myproc();
    if (p == 0 || p->pagetable == 0 || reqid <= 0 || out_cap <= 0 || out_cap > AI_MAX_RESULT + 1) {
        return -1;
    }

    /*
     * TODO(Part2):
     * 1. Find the request with ai_find_req_locked(reqid, p->pid).
     * 2. Sleep while ai_req_busy(req) is true.
     * 3. Reject failed requests cleanly.
     * 4. On success, copy req->result into a temporary kernel buffer.
     * 5. copyout that result to out_uva and return req->result_len.
     * 6. Recycle the request slot back to UNUSED after one successful wait.
     * 7. Make a second wait on the same reqid fail instead of returning stale data.
     */
    (void)out_uva;
    return -1;
}

int ai_service_call(uint64 token_uva, int token_count, int predict_count, uint64 out_uva, int out_cap) {
    /*
     * Starter sync smoke path:
     * - This path is provided by the course.
     * - It gives you a quick synchronous sanity check before they start
     *   the real async service work.
     *
     * Part 2 final check:
     * - After query/wait semantics are stable, remove this smoke path.
     * - Re-implement ai_service_call() as:
     *     reqid = ai_service_submit(...)
     *     if (reqid < 0) return -1;
     *     return ai_service_wait(reqid, out_uva, out_cap);
     */
    struct proc *p = myproc();
    if (p == 0 || p->pagetable == 0 || out_uva == 0) {
        return -1;
    }
    if (token_count <= 0 || token_count > AI_MAX_TOKENS || predict_count <= 0 || predict_count > AI_MAX_PREDICT ||
        out_cap <= 0 || out_cap > AI_MAX_RESULT + 1) {
        return -1;
    }

    uint32 tokens[AI_MAX_TOKENS];
    char result[AI_MAX_RESULT + 1];
    memset(tokens, 0, sizeof(tokens));
    memset(result, 0, sizeof(result));

    if (copyin(p->pagetable, (char *)tokens, token_uva, (uint64)token_count * sizeof(uint32)) < 0) {
        return -1;
    }

    acquire(&aisvc.lock);
    struct ai_request *req = ai_find_slot_locked();
    if (req == 0) {
        release(&aisvc.lock);
        return -1;
    }

    req->id = aisvc.next_id++;
    req->owner_pid = p->pid;
    req->state = AIREQ_NEW;
    req->err = 0;
    req->token_count = token_count;
    req->predict_count = predict_count;
    req->result_len = 0;
    memmove(req->tokens, tokens, (uint64)token_count * sizeof(uint32));
    req->state = AIREQ_READY;
    req->state = AIREQ_RUNNING;
    release(&aisvc.lock);

    int n = ai_sync_smoke_placeholder(req->tokens, req->token_count, req->predict_count, result, out_cap);

    acquire(&aisvc.lock);
    if (n < 0 || n > AI_MAX_RESULT) {
        req->err = -1;
        req->result_len = 0;
        req->state = AIREQ_FAILED;
        ai_req_reset(req);
        release(&aisvc.lock);
        return -1;
    }

    req->err = 0;
    req->result_len = n;
    memmove(req->result, result, (uint64)n + 1);
    req->state = AIREQ_DONE;
    release(&aisvc.lock);

    if (copyout(p->pagetable, out_uva, result, (uint64)n + 1) < 0) {
        acquire(&aisvc.lock);
        ai_req_reset(req);
        release(&aisvc.lock);
        return -1;
    }

    acquire(&aisvc.lock);
    ai_req_reset(req);
    release(&aisvc.lock);
    return n;
}
