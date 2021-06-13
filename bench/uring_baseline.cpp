/* built upon io_uring codebase https://github.com/shuveb/io_uring-by-example */
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <linux/fs.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <iostream>
#include <ctime>
#include <ratio>
#include <chrono>

/* If your compilation fails because the header file below is missing,
* your kernel is probably too old to support io_uring.
* */
#include <linux/io_uring.h>

#define READ_SIZE   512
#define PAGE_SHIFT  12
#define PAGE_SIZE   (1 << PAGE_SHIFT)
#define MAX_PAGE_INDEX  (1 << 23)

/* This is x86 specific */
#define read_barrier()  __asm__ __volatile__("":::"memory")
#define write_barrier() __asm__ __volatile__("":::"memory")

using namespace std::chrono;

struct app_io_sq_ring {
	unsigned *head;
	unsigned *tail;
	unsigned *ring_mask;
	unsigned *ring_entries;
	unsigned *flags;
	unsigned *array;
};

struct app_io_cq_ring {
	unsigned *head;
	unsigned *tail;
	unsigned *ring_mask;
	unsigned *ring_entries;
	struct io_uring_cqe *cqes;
};

struct entry {
	steady_clock::time_point start_time;
	long total_time;

	int cur_level;
};

struct submitter {
	int ring_fd;
	struct app_io_sq_ring sq_ring;
	struct io_uring_sqe *sqes;
	struct app_io_cq_ring cq_ring;

	int batch_size;
	int num_file;
	int *fd_arr;

	void *buffer;
	struct iovec *iovecs;

	struct entry *entry_arr;
	long *completion_arr;
	long finished_op;
};

/*
* This code is written in the days when io_uring-related system calls are not
* part of standard C libraries. So, we roll our own system call wrapper
* functions.
* */

int io_uring_setup(unsigned entries, struct io_uring_params *p) {
	return (int) syscall(__NR_io_uring_setup, entries, p);
}

int io_uring_enter(int ring_fd, unsigned int to_submit,
		   unsigned int min_complete, unsigned int flags) {
	return (int) syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete,
			     flags, NULL, 0);
}

int io_uring_register(int fd, unsigned int opcode, void *arg,
		      unsigned int nr_args) {
	return (int) syscall(__NR_io_uring_register, fd, opcode, arg, nr_args);
}

/*
* io_uring requires a lot of setup which looks pretty hairy, but isn't all
* that difficult to understand. Because of all this boilerplate code,
* io_uring's author has created liburing, which is relatively easy to use.
* However, you should take your time and understand this code. It is always
* good to know how it all works underneath. Apart from bragging rights,
* it does offer you a certain strange geeky peace.
* */

int app_setup_uring(struct submitter *s) {
	struct app_io_sq_ring *sring = &s->sq_ring;
	struct app_io_cq_ring *cring = &s->cq_ring;
	struct io_uring_params p;
	void *sq_ptr, *cq_ptr;

	/*
	* We need to pass in the io_uring_params structure to the io_uring_setup()
	* call zeroed out. We could set any flags if we need to, but for this
	* example, we don't.
	* */
	memset(&p, 0, sizeof(p));
	s->ring_fd = io_uring_setup(s->batch_size, &p);
	if (s->ring_fd < 0) {
		perror("io_uring_setup");
		return 1;
	}

	/*
	* io_uring communication happens via 2 shared kernel-user space ring buffers,
	* which can be jointly mapped with a single mmap() call in recent kernels.
	* While the completion queue is directly manipulated, the submission queue
	* has an indirection array in between. We map that in as well.
	* */

	int sring_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
	int cring_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);

	/* In kernel version 5.4 and above, it is possible to map the submission and
	* completion buffers with a single mmap() call. Rather than check for kernel
	* versions, the recommended way is to just check the features field of the
	* io_uring_params structure, which is a bit mask. If the
	* IORING_FEAT_SINGLE_MMAP is set, then we can do away with the second mmap()
	* call to map the completion ring.
	* */
	if (p.features & IORING_FEAT_SINGLE_MMAP) {
		if (cring_sz > sring_sz) {
			sring_sz = cring_sz;
		}
		cring_sz = sring_sz;
	}

	/* Map in the submission and completion queue ring buffers.
	* Older kernels only map in the submission queue, though.
	* */
	sq_ptr = mmap(0, sring_sz, PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_POPULATE,
		      s->ring_fd, IORING_OFF_SQ_RING);
	if (sq_ptr == MAP_FAILED) {
		perror("mmap");
		return 1;
	}

	if (p.features & IORING_FEAT_SINGLE_MMAP) {
		cq_ptr = sq_ptr;
	} else {
		/* Map in the completion queue ring buffer in older kernels separately */
		cq_ptr = mmap(0, cring_sz, PROT_READ | PROT_WRITE,
			      MAP_SHARED | MAP_POPULATE,
			      s->ring_fd, IORING_OFF_CQ_RING);
		if (cq_ptr == MAP_FAILED) {
			perror("mmap");
			return 1;
		}
	}
	/* Save useful fields in a global app_io_sq_ring struct for later
	* easy reference */
	sring->head = sq_ptr + p.sq_off.head;
	sring->tail = sq_ptr + p.sq_off.tail;
	sring->ring_mask = sq_ptr + p.sq_off.ring_mask;
	sring->ring_entries = sq_ptr + p.sq_off.ring_entries;
	sring->flags = sq_ptr + p.sq_off.flags;
	sring->array = sq_ptr + p.sq_off.array;

	/* Map in the submission queue entries array */
	s->sqes = mmap(0, p.sq_entries * sizeof(struct io_uring_sqe),
		       PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
		       s->ring_fd, IORING_OFF_SQES);
	if (s->sqes == MAP_FAILED) {
		perror("mmap");
		return 1;
	}

	/* Save useful fields in a global app_io_cq_ring struct for later
	* easy reference */
	cring->head = cq_ptr + p.cq_off.head;
	cring->tail = cq_ptr + p.cq_off.tail;
	cring->ring_mask = cq_ptr + p.cq_off.ring_mask;
	cring->ring_entries = cq_ptr + p.cq_off.ring_entries;
	cring->cqes = cq_ptr + p.cq_off.cqes;

	return 0;
}

/*
* Read from completion queue.
* In this function, we read completion events from the completion queue, get
* the data buffer that will have the file data and print it to the console.
* */
int poll_from_cq(struct submitter *s) {
	struct app_io_cq_ring *cring = &s->cq_ring;
	struct io_uring_cqe *cqe;
	unsigned head;
	int reaped = 0;

	head = *cring->head;
	read_barrier();

	while (true) {
		/*
		* Remember, this is a ring buffer. If head == tail, it means that the
		* buffer is empty.
		* */
		if (head == *cring->tail) {
			break;
		}
		read_barrier();

		/* Get the entry */
		cqe = &cring->cqes[head & *s->cq_ring.ring_mask];
		if (cqe->res != READ_SIZE) {
			printf("read_from_cq error, ret: %d\n", cqe->res);
			exit(1);
		}
		s->completion_arr[reaped++] = cqe->user_data;
		head++;
	}

	*cring->head = head;
	write_barrier();

	return reaped;
}

/*
* Submit to submission queue.
* In this function, we submit requests to the submission queue. You can submit
* many types of requests. Ours is going to be the readv() request, which we
* specify via IORING_OP_READV.
*
* */
void submit_to_sq(struct submitter *s, unsigned long long user_data, void *addr, bool with_barrier) {
	struct app_io_sq_ring *sring = &s->sq_ring;
	unsigned index = 0, tail = 0, next_tail = 0;

	/* Add our submission queue entry to the tail of the SQE ring buffer */
	next_tail = tail = *sring->tail;
	next_tail++;
	index = tail & *s->sq_ring.ring_mask;
	struct io_uring_sqe *sqe = &s->sqes[index];
	sqe->fd = rand() % s->num_file;  /* randomly choose a device */
	sqe->flags = IOSQE_FIXED_FILE;
	sqe->opcode = IORING_OP_READV;
	sqe->addr = (unsigned long) addr;
	sqe->len = 1;
	sqe->off = ((long) (rand() % MAX_PAGE_INDEX)) << PAGE_SHIFT;  /* randomly choose an offset */
	sqe->user_data = user_data;
	sring->array[index] = index;
	tail = next_tail;

	if (with_barrier) {
		write_barrier();
	}

	/* Update the tail so the kernel can see it. */
	if (*sring->tail != tail) {
		*sring->tail = tail;
		if (with_barrier) {
			write_barrier();
		}
	}
}

int main(int argc, char *argv[]) {
	struct submitter *s;

	if (argc < 5) {
		fprintf(stderr, "Usage: %s <batch_size> <level> <iteration> <filenames>\n", argv[0]);
		return 1;
	}
	int batch_size;
	int level;
	long iteration;
	int num_file = argc - 4;
	sscanf(argv[1], "%d", &batch_size);
	sscanf(argv[2], "%d", &level);
	sscanf(argv[3], "%ld", &iteration);

	s = (struct submitter *) malloc(sizeof(*s));
	if (!s) {
		perror("malloc");
		return 1;
	}
	memset(s, 0, sizeof(*s));
	s->batch_size = batch_size;
	s->num_file = num_file;

	if (app_setup_uring(s)) {
		fprintf(stderr, "Unable to setup uring!\n");
		return 1;
	}

	s->fd_arr = (int *) malloc(sizeof(int) * num_file);
	if (!s->fd_arr) {
		perror("s->fd_arr");
		return 1;
	}

	for (int i = 0; i < num_file; ++i) {
		s->fd_arr[i] = open(argv[4 + i], O_RDONLY | O_DIRECT);
		if (s->fd_arr[i] < 0) {
			perror("open");
			exit(1);
		}
	}
	int ret = io_uring_register(s->ring_fd, IORING_REGISTER_FILES, s->fd_arr, s->num_file);
	if (ret) {
		perror("io_uring_register");
		exit(1);
	}

	if (posix_memalign(&s->buffer, READ_SIZE, READ_SIZE)) {
		perror("posix_memalign");
		return 1;
	}
	s->iovecs = (struct iovec *) malloc(batch_size * sizeof(struct iovec));
	if (!s->iovecs) {
		perror("s->iovecs");
		exit(1);
	}
	for (int i = 0; i < batch_size; ++i) {
		s->iovecs[i].iov_base = s->buffer;
		s->iovecs[i].iov_len = READ_SIZE;
	}

	s->entry_arr = (struct entry *) malloc(iteration * batch_size * sizeof(*s->entry_arr));
	if (!s->entry_arr) {
		perror("s->entry_arr");
		exit(1);
	}
	memset(s->entry_arr, 0, iteration * batch_size * sizeof(*s->entry_arr));
	s->completion_arr = (long *) malloc(batch_size * sizeof(long));
	if (!s->completion_arr) {
		perror("s->completion_arr");
		exit(1);
	}
	memset(s->completion_arr, 0, batch_size * sizeof(long));
	s->finished_op = 0;

	/* send the first batch */
	for (int i = 0; i < batch_size; ++i) {
		s->entry_arr[i].start_time = steady_clock::now();
		submit_to_sq(s, i, &s->iovecs[i], false);
	}
	write_barrier();
	int enter_ret = io_uring_enter(s->ring_fd, s->batch_size, 0, IORING_ENTER_GETEVENTS);
	if (enter_ret < 0) {
		perror("io_uring_enter");
		return 1;
	}

	while (s->finished_op < iteration * batch_size * level) {
		int reaped = poll_from_cq(s);
		int submitted = 0;
		for (int i = 0; i < reaped; ++i) {
			long index = s->completion_arr[i];
			long batch_index = index / batch_size;
			long sub_index = index % batch_size;
			struct entry *e = &s->entry_arr[index];
			e->total_time += duration_cast<nanoseconds>(steady_clock::now() - e->start_time).count();
			++e->cur_level;

			if (e->cur_level == level && batch_index == iteration - 1) {
				/* no further request */
				continue;
			}
			long next_batch_index = (e->cur_level == level) ? batch_index + 1 : batch_index;
			long next_index = next_batch_index * batch_size + sub_index;
			struct entry *next_e = &s->entry_arr[next_index];
			next_e->start_time = steady_clock::now();
			submit_to_sq(s, next_index, &s->iovecs[sub_index], false);
			++submitted;
		}
		s->finished_op += reaped;
		write_barrier();
		if (submitted == 0) {
			continue;
		}
		int enter_ret = io_uring_enter(s->ring_fd, submitted, 0, IORING_ENTER_GETEVENTS);
		if (enter_ret < 0) {
			perror("io_uring_enter");
			return 1;
		}
	}

	for (long i = 0; i < iteration * batch_size; ++i) {
		std::cout << s->entry_arr[i].total_time << std::endl;
	}

	return 0;
}
