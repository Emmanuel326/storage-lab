/*
 * 01_bare_ring.c — bare io_uring ring setup, raw syscalls only
 *
 * WHAT THIS FILE TEACHES:
 *   - io_uring lives in two shared memory regions: the SQ ring and the CQ ring
 *   - The kernel and userspace communicate through these rings WITHOUT syscalls
 *     (after setup)
 *   - io_uring_setup() returns a file descriptor + fills params about ring geometry
 *   - You mmap() three regions:
 *       1. SQ ring  — submission queue metadata + index array
 *       2. CQ ring  — completion queue metadata + CQE array
 *       3. SQEs     — the actual submission queue entry structs (separate mmap)
 *
 * NO liburing. Everything is raw syscalls and manual mmap.
 *
 * COMPILE:
 *   gcc -o 01_bare_ring 01_bare_ring.c
 *
 * RUN:
 *   ./01_bare_ring
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/io_uring.h>   /* io_uring_params, IORING_OFF_* constants */

/* ── raw syscall wrappers ────────────────────────────────────────────────────
 * glibc does not expose io_uring syscalls directly (as of when this was
 * written), so we invoke them ourselves via syscall(2).
 */
static int io_uring_setup(unsigned entries, struct io_uring_params *p)
{
    return (int)syscall(__NR_io_uring_setup, entries, p);
}

/* ── ring geometry helper ────────────────────────────────────────────────────
 * Everything the caller needs to talk to one ring.
 * We keep SQ and CQ in the same struct for clarity.
 */
struct app_ring {
    /* SQ ring */
    void        *sq_ring_ptr;   /* mmap base of SQ ring */
    size_t       sq_ring_sz;
    unsigned    *sq_head;       /* kernel advances head when it consumes */
    unsigned    *sq_tail;       /* we advance tail when we submit        */
    unsigned    *sq_ring_mask;  /* index = tail & mask                   */
    unsigned    *sq_array;      /* maps SQ slot → SQE index              */

    /* SQEs — a separate mmap region */
    struct io_uring_sqe *sqes;
    size_t               sqes_sz;

    /* CQ ring */
    void        *cq_ring_ptr;
    size_t       cq_ring_sz;
    unsigned    *cq_head;
    unsigned    *cq_tail;
    unsigned    *cq_ring_mask;
    struct io_uring_cqe *cqes;  /* the actual completion entries */

    int ring_fd;
};

/* ── setup ───────────────────────────────────────────────────────────────────
 * Call io_uring_setup, then mmap the three regions.
 */
static int ring_setup(struct app_ring *r, unsigned queue_depth)
{
    struct io_uring_params p;
    memset(&p, 0, sizeof(p));
    /* p.flags = 0 means the simplest mode: interrupt-driven, no polling */

    r->ring_fd = io_uring_setup(queue_depth, &p);
    if (r->ring_fd < 0) {
        perror("io_uring_setup");
        return -1;
    }

    printf("=== io_uring ring created ===\n");
    printf("  ring_fd          : %d\n",   r->ring_fd);
    printf("  sq entries       : %u\n",   p.sq_entries);
    printf("  cq entries       : %u\n",   p.cq_entries);
    printf("  features         : 0x%x\n", p.features);
    printf("  sq_off.head      : %u\n",   p.sq_off.head);
    printf("  sq_off.tail      : %u\n",   p.sq_off.tail);
    printf("  sq_off.ring_mask : %u\n",   p.sq_off.ring_mask);
    printf("  sq_off.array     : %u\n",   p.sq_off.array);
    printf("  cq_off.head      : %u\n",   p.cq_off.head);
    printf("  cq_off.tail      : %u\n",   p.cq_off.tail);
    printf("  cq_off.cqes      : %u\n",   p.cq_off.cqes);
    printf("\n");

    /*
     * mmap #1 — SQ ring
     * IORING_OFF_SQ_RING is the magic offset the kernel recognises on
     * the ring_fd to hand us the submission ring metadata page(s).
     */
    r->sq_ring_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    r->sq_ring_ptr = mmap(NULL, r->sq_ring_sz,
                          PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_POPULATE,
                          r->ring_fd, IORING_OFF_SQ_RING);
    if (r->sq_ring_ptr == MAP_FAILED) {
        perror("mmap SQ ring");
        return -1;
    }

    /* Pull pointers out of the mapped region using the offsets the kernel
     * told us about in p.sq_off. This is the handshake: the kernel owns
     * this memory, we just point into it. */
    r->sq_head      = r->sq_ring_ptr + p.sq_off.head;
    r->sq_tail      = r->sq_ring_ptr + p.sq_off.tail;
    r->sq_ring_mask = r->sq_ring_ptr + p.sq_off.ring_mask;
    r->sq_array     = r->sq_ring_ptr + p.sq_off.array;

    /*
     * mmap #2 — SQEs (submission queue entries)
     * These are the actual structs we fill in with work.
     * They live at a DIFFERENT offset (IORING_OFF_SQES) from the ring
     * metadata, hence a separate mmap call.
     */
    r->sqes_sz = p.sq_entries * sizeof(struct io_uring_sqe);
    r->sqes = mmap(NULL, r->sqes_sz,
                   PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_POPULATE,
                   r->ring_fd, IORING_OFF_SQES);
    if (r->sqes == MAP_FAILED) {
        perror("mmap SQEs");
        return -1;
    }

    /*
     * mmap #3 — CQ ring
     * On modern kernels (IORING_FEAT_SINGLE_MMAP) SQ and CQ share one
     * mapping, but we do them separately here for clarity.
     */
    r->cq_ring_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    r->cq_ring_ptr = mmap(NULL, r->cq_ring_sz,
                          PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_POPULATE,
                          r->ring_fd, IORING_OFF_CQ_RING);
    if (r->cq_ring_ptr == MAP_FAILED) {
        perror("mmap CQ ring");
        return -1;
    }

    r->cq_head      = r->cq_ring_ptr + p.cq_off.head;
    r->cq_tail      = r->cq_ring_ptr + p.cq_off.tail;
    r->cq_ring_mask = r->cq_ring_ptr + p.cq_off.ring_mask;
    r->cqes         = r->cq_ring_ptr + p.cq_off.cqes;

    return 0;
}

/* ── teardown ────────────────────────────────────────────────────────────────
 * Unmap everything and close the ring fd.
 * The kernel tears down the ring when the fd is closed.
 */
static void ring_teardown(struct app_ring *r)
{
    munmap(r->sq_ring_ptr, r->sq_ring_sz);
    munmap(r->sqes,        r->sqes_sz);
    munmap(r->cq_ring_ptr, r->cq_ring_sz);
    close(r->ring_fd);
    printf("ring torn down cleanly.\n");
}

/* ── main ────────────────────────────────────────────────────────────────────
 * Queue depth of 4: minimum meaningful ring, easy to reason about.
 */
int main(void)
{
    struct app_ring r;
    memset(&r, 0, sizeof(r));

    if (ring_setup(&r, 4) < 0)
        return 1;

    printf("SQ ring mapped at : %p  (%zu bytes)\n", r.sq_ring_ptr, r.sq_ring_sz);
    printf("SQEs   mapped at  : %p  (%zu bytes)\n", (void*)r.sqes,  r.sqes_sz);
    printf("CQ ring mapped at : %p  (%zu bytes)\n", r.cq_ring_ptr, r.cq_ring_sz);
    printf("\n");
    printf("SQ head=%u  tail=%u  mask=%u\n",
           *r.sq_head, *r.sq_tail, *r.sq_ring_mask);
    printf("CQ head=%u  tail=%u  mask=%u\n",
           *r.cq_head, *r.cq_tail, *r.cq_ring_mask);
    printf("\n");
    printf("Nothing submitted yet — ring is idle.\n");
    printf("Next: 02_submit_nop.c will put the first SQE on this ring.\n");

    ring_teardown(&r);
    return 0;
}
