/*
 * i will submit a NOP operation, harvest completion
 *
 * WHAT THIS FILE IS ABOUT:-
 * *** THE SQE/CQE lifecycle end to end
 *  1. grab an SQE slot
 *  2. fil it in 
 *  3. advance the SQ tail
 *  4. call io_uring enter()
 *  5. spin the CQ tail
 *  6. read the CQE
 *  7. advace CQ head 
 *
 *  this file does nothing, it is completely for a test case to see whether things work end to end without worrying about fd  or buffers 
 *
 *  you can compile with gcc/clang
 *  clang -o 02_submit_nop 02_submit_nop.c -Wall 
 *  */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/io_uring.h>


//IO_URING syscall wrapper 

static int io_uring_setup(unsigned entries, struct io_uring_params *p)
{
	return(int)syscall(__NR_io_uring_setup, entries, p);
}

/*
 * io_uring_enter  is the syscall that actually kickes the kernel
 * to_submit:- how many SQEs to consume from the SQ 
 * min_complete : block until at least this many CQEs are ready 
 * flags :  IORING_ENTER_GETEVENTS means  "block for completions"
 */

static int io_uring_enter(int fd, unsigned to_submit,
		           unsigned min_complete,
			   unsigned flags)
{
	return (int)syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags, NULL,0);
}


/*ring struct*/

struct app_ring{
	void  *sq_ring_ptr;
	size_t  sq_ring_sz;
	unsigned *sq_head;
	unsigned *sq_tail;
	unsigned  *sq_ring_mask;
	unsigned  *sq_array;


	struct io_uring_sqe *sqes;
	size_t   sqes_sz;

	void  *cq_ring_ptr;
	size_t  cq_ring_sz;
	unsigned *cq_head;
	unsigned  *cq_tail;
	unsigned  *cq_ring_mask;
	struct io_uring_cqe *cqes;

	int ring_fd;
};

static int ring_setup(struct app_ring *r, unsigned depth)
{
	struct io_uring_params p;
	memset(&p, 0, sizeof(p));

	r->ring_fd = io_uring_setup(depth, &p);
	if(r->ring_fd <0) {perror("io_uring_setup"); return -1;}

	r->sq_ring_sz =p.sq_off.array+ p.sq_entries * sizeof(unsigned);
	r->sq_ring_ptr = mmap(NULL, r->sq_ring_sz,
			      PROT_READ |PROT_WRITE,
			      MAP_SHARED | MAP_POPULATE,
			      r->ring_fd, IORING_OFF_SQ_RING);
	if(r->sq_ring_ptr == MAP_FAILED){perror("mmap SQ"); return -1;}

	r->sq_head    = r->sq_ring_ptr + p.sq_off.head;
	r->sq_tail    = r->sq_ring_ptr + p.sq_off.tail;
	r->sq_ring_mask    = r->sq_ring_ptr + p.sq_off.ring_mask;
	r->sq_array    = r->sq_ring_ptr + p.sq_off.array;


	
    r->sqes_sz = p.sq_entries * sizeof(struct io_uring_sqe);
    r->sqes    = mmap(NULL, r->sqes_sz,
                      PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_POPULATE,
                      r->ring_fd, IORING_OFF_SQES);
    if (r->sqes == MAP_FAILED) { perror("mmap SQEs"); return -1; }
 
    r->cq_ring_sz  = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    r->cq_ring_ptr = mmap(NULL, r->cq_ring_sz,
                          PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_POPULATE,
                          r->ring_fd, IORING_OFF_CQ_RING);
    if (r->cq_ring_ptr == MAP_FAILED) { perror("mmap CQ"); return -1; }
 
    r->cq_head      = r->cq_ring_ptr + p.cq_off.head;
    r->cq_tail      = r->cq_ring_ptr + p.cq_off.tail;
    r->cq_ring_mask = r->cq_ring_ptr + p.cq_off.ring_mask;
    r->cqes         = r->cq_ring_ptr + p.cq_off.cqes;
 
    return 0;

}


static void ring_teardown(struct app_ring *r)
{
	munmap(r->sq_ring_ptr, r->sq_ring_sz);
	munmap(r->sqes,  r->sqes_sz);
	munmap(r->cq_ring_ptr,  r->cq_ring_sz);
	close(r-> ring_fd);

}


/*submit one NOP*/
static void submit_nop(struct app_ring *r, __u64 user_data)
{
	/*find the next free slot
	 * tail & mask gives the index in to the sqes array
	 * we do not advance tail yet, the kernel must not see an incomplete SQE.
	 * */
	unsigned tail = *r->sq_tail;
	unsigned index = tail & *r->sq_ring_mask;

	printf("[SQ] before submit: head=%u  tail=%u\n", *r->sq_head, *r->sq_tail);

	//fill the SQE
	struct io_uring_sqe *sqe = &r ->sqes[index];
	memset(sqe,0,sizeof(*sqe));
	sqe->opcode  = IORING_OP_NOP; //do nothing
        sqe->user_data  = user_data;


	/*
	 * publish the SQE by advancing the tail and writing its index into ssq_array
	 * qs_array[slot] = sqe_index telles kernel which sqe to look at foe this slot for siple cases they are the same value but for fixed_buffere/file modes can differ*/
	r->sq_array[index] = index;

	//memory barrier,  you make sure sqe write is visible
	//before we bump the tail
	__sync_synchronize();
	*r->sq_tail = tail +1;
	__sync_synchronize();

	printf("[SQ] after publish: head=%u tail=%u", *r->sq_head, *r->sq_tail);


//step 4 io_uring_enter(ti_submit=1,min_complete=1)
//consumene 1 SQE from the ring
//block until at least one  CQE is ready
//IORING_ENTER_GETEVENTS: actually wait for completions 
//
//this is the one syscall for the entire operation


int ret = io_uring_enter(r->ring_fd,1,1, IORING_ENTER_GETEVENTS);
if(ret < 0){perror("io_uring_enter"); exit(1);}

printf("[SQ] after enter: head=%u  tail=%u  (enter  ret=%d)\n",
		*r->sq_head, *r->sq_tail,ret);
}

//harvest this one queue

static void harvest_cqe(struct app_ring *r)
{
	printf("\n[CQ] before arvest: head=%u tail=%u\n",
			*r->cq_head, *r->cq_tail);
	unsigned head = *r->cq_head;

	//read the cqe
	struct io_uring_cqe *cqe = &r->cqes[head & *r->cq_ring_mask];
	printf("[CQ]  CQE: user_data=%llu  res=%d  flags=0x%x\n",
			(unsigned long long)cqe->user_data,
			cqe->res,
			cqe->flags);
	__sync_synchronize();
	*r->cq_head=head +1;
	__sync_synchronize();

	printf("[CQ] after harvest: head=%u tail=%u\n",
			*r->cq_head, *r->cq_tail);

}


int main(void)
{
	struct app_ring r;
	memset(&r,0,sizeof(r));

	if(ring_setup(&r,4)<0)
		return 1;

	printf("=== submitting NOP with user_data=0xDEAD ===\n\n");
	submit_nop(&r, 0xDEAD);
	harvest_cqe(&r);


	
    printf("\n=== submitting a second NOP to see counters keep moving ===\n\n");
    submit_nop(&r, 0xBEEF);
    harvest_cqe(&r);


    printf("\nDone. Notice how head and tail kept incrementing\n");
    printf("they never wrap to zero, the mask handles the wrap.\n");

    ring_teardown(&r);
    return 0;

}
