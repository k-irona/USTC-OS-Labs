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
        if (req->owner_pid == owner_pid) return req;
        return 0;
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
    if (p == 0 || p->pagetable == 0 || reqid_out == 0) {
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
    struct ai_request *req = 0;
    while (true) {
        if (!aisvc.worker_online) {
            release(&aisvc.lock);
            return -1;
        } // check worker registered
        req = ai_find_slot_locked();
        if (req != 0) break; // found a free slot
        sleep(&aisvc, &aisvc.lock); // sleep until aisvc was waken
    }

    req->id = aisvc.next_id++;
    req->owner_pid = p->pid;
    req->state = AIREQ_NEW;
    req->err = 0;
    req->token_count = token_count;
    req->predict_count = predict_count;
    req->result_len = 0;
    memmove(req->tokens, tokens, (uint64)token_count * sizeof(uint32));
    req->result[0] = '\0';
    req->state = AIREQ_READY; // initialize

    int slot_index = (int)(req - &aisvc.reqs[0]);
    aisvc.q[aisvc.qtail] = slot_index;
    aisvc.qtail = (aisvc.qtail + 1) % AI_NREQ;
    aisvc.qcount++; // push to the queue

    wakeup(&aisvc.qcount); // wake the worker sleeping on qcount
    *reqid_out = req->id;
    release(&aisvc.lock);
    return req->id;
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

    if (aisvc.worker_online == 0) {
        aisvc.worker_pid = p->pid;
        aisvc.worker_online = 1;
        release(&aisvc.lock);
        return 0;
    } //first time register

    if (aisvc.worker_pid == p->pid) {
        release(&aisvc.lock);
        return 0;
    } //repeat register

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
    if (!aisvc.worker_online || p->pid != aisvc.worker_pid) {
        release(&aisvc.lock);
        return -1;
    } // check the caller is registered worker
    while (aisvc.qcount <= 0) {
        sleep(&aisvc.qcount, &aisvc.lock);  // sleep while queue is empty
    }

    int slot_index = aisvc.q[aisvc.qhead];
    aisvc.q[aisvc.qhead] = -1;
    aisvc.qhead = (aisvc.qhead + 1) % AI_NREQ;
    aisvc.qcount--; // pop slot index from the queue

    if (slot_index < 0 || slot_index >= AI_NREQ) {
        release(&aisvc.lock);
        return -1;
    }

    struct ai_request *req = &aisvc.reqs[slot_index];
    if (req->state != AIREQ_READY) {
        release(&aisvc.lock);
        return -1;
    }
    req->state = AIREQ_RUNNING; // mark the req RUNNING

    int reqid_k = req->id;
    int token_count_k = req->token_count;
    int predict_count_k = req->predict_count;
    uint32 tokens_k[AI_MAX_TOKENS];
    memset(tokens_k, 0, sizeof(tokens_k));
    memmove(tokens_k, req->tokens, (uint64)token_count_k * sizeof(uint32)); // copy into kernal buffers

    if (token_cap < token_count_k) {
        req->err = -1;
        req->result_len = 0;
        req->state = AIREQ_FAILED;
        wakeup(req);
        release(&aisvc.lock);
        return -1;
    } // when token_cap is too small

    release(&aisvc.lock);

    if (copyout(p->pagetable, token_uva, (char *)tokens_k, (uint64)token_count_k * sizeof(uint32)) < 0 ||
        copyout(p->pagetable, reqid_uva, (char *)&reqid_k, sizeof(reqid_k)) < 0 ||
        copyout(p->pagetable, predict_uva, (char *)&predict_count_k, sizeof(predict_count_k)) < 0) {
        acquire(&aisvc.lock);
        struct ai_request *req2 = ai_find_req_by_id_locked(reqid_k);
        if (req2 != 0 && req2->state == AIREQ_RUNNING) {
            req2->err = -1;
            req2->result_len = 0;
            req2->state = AIREQ_FAILED;
            wakeup(req2);
        } // if copyout failed
        release(&aisvc.lock);
        return -1;
    }

    return token_count_k;
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
    if (!aisvc.worker_online || p->pid != aisvc.worker_pid) {
        release(&aisvc.lock);
        return -1;
    } // check the caller is registered worker

    struct ai_request *req = ai_find_req_by_id_locked(reqid);
    if (req == 0 || req->state != AIREQ_RUNNING) {
        release(&aisvc.lock);
        return -1;
    } // find RUNNING req

    release(&aisvc.lock);

    int request_failed = 0;
    int fail_err = -1;
    if (status != 0) {
        request_failed = 1;
        fail_err = status < 0 ? status : -1;
    } else if (out_len < 0 || out_len > AI_MAX_RESULT) {
        request_failed = 1;
    } else if (out_len > 0 && copyin(p->pagetable, result, out_uva, (uint64)out_len) < 0) {
        request_failed = 1;
    } // check status, length and copyin while unlocked

    acquire(&aisvc.lock); // reacquiring lock

    struct ai_request *req2 = ai_find_req_by_id_locked(reqid);
    if (req2 == 0 || req2->state != AIREQ_RUNNING) {
        release(&aisvc.lock);
        return -1;
    } // re-check req failed

    if (request_failed) {
        req2->err = fail_err;
        req2->result_len = 0;
        req2->result[0] = '\0';
        req2->state = AIREQ_FAILED;
        wakeup(req2);
        release(&aisvc.lock);
        return 0;
    } // previous fail

    req2->err = 0;
    req2->result_len = out_len;
    if (out_len > 0) {
        memmove(req2->result, result, (uint64)out_len);
    }
    req2->result[out_len] = '\0';
    req2->state = AIREQ_DONE;
    wakeup(req2);
    release(&aisvc.lock);
    return 0; // re-check success
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
    struct ai_status st;
    memset(&st, 0, sizeof(st));

    acquire(&aisvc.lock);
    struct ai_request *req = ai_find_req_locked(reqid, p->pid); // find req
    if (req == 0) {
        release(&aisvc.lock);
        return -1;
    } // refuse to expose status

    st.reqid = req->id;
    st.state = req->state;
    st.err = req->err;
    st.result_len = req->result_len;
    release(&aisvc.lock); // fill status

    if (copyout(p->pagetable, st_uva, (char *)&st, sizeof(st)) < 0) {
        return -1;
    } // copyout out status to st_uva
    return 0;
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
    char result[AI_MAX_RESULT + 1];
    int result_len = 0;
    memset(result, 0, sizeof(result));

    acquire(&aisvc.lock);

    struct ai_request *req = ai_find_req_locked(reqid, p->pid); // find req
    if (req == 0) {
        release(&aisvc.lock);
        return -1;
    }

    while (ai_req_busy(req)) {
        sleep(req, &aisvc.lock);
    } // sleep while req is busy

    if (req->state != AIREQ_DONE) {
        release(&aisvc.lock);
        return -1;
    } // reject failed req

    result_len = req->result_len;
    if (result_len < 0 || result_len > AI_MAX_RESULT || result_len + 1 > out_cap) {
        release(&aisvc.lock);
        return -1;
    }
    memmove(result, req->result, (uint64)result_len + 1); // copy req->result into kernal buffer
    release(&aisvc.lock);

    if (copyout(p->pagetable, out_uva, result, (uint64)result_len + 1) < 0) {
        return -1;
    } // copyout result to out_uva

    acquire(&aisvc.lock);
    req = ai_find_req_locked(reqid, p->pid);
    if (req != 0 && req->state == AIREQ_DONE) {
        ai_req_reset(req);
        wakeup(&aisvc);
    }
    release(&aisvc.lock);
    return result_len;
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
    int reqid = ai_service_submit(token_uva, token_count, predict_count);
    if (reqid < 0) {
        return -1;
    }
    return ai_service_wait(reqid, out_uva, out_cap);
}
