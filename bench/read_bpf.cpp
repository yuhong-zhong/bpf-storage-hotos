#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <chrono>
#include <math.h>
#include <thread>
#include <fstream>
#include <sys/syscall.h>
#include <string.h>
#include <sched.h>

#define __NR_set_bpf_level 440

#define PAGE_SHIFT 12
#define PAGE_SIZE (1 << PAGE_SHIFT)

#define READ_SIZE_SHIFT 9
#define READ_SIZE (1 << READ_SIZE_SHIFT)

#define MAX_PAGE_INDEX  (1 << 23)

using namespace std::chrono;
using namespace std;


int num_thread;
int num_file;
int level;
long iteration;

long *latency_measure;
char **file_names;


long sys_bpf_set_level(int fd, int level) {
	return syscall(__NR_set_bpf_level, fd, level);
}

void read_key(int fd, long index, void *buffer) {
	off_t lseek_ret = lseek(fd, index << PAGE_SHIFT, SEEK_SET);
	if (lseek_ret != index << PAGE_SHIFT) {
		printf("lseek error, errno %d, ret: %ld\n", errno, lseek_ret);
		exit(1);
	}
	int read_ret = read(fd, buffer, READ_SIZE);
	if (read_ret != READ_SIZE) {
		printf("read error, errno %d, ret: %d\n", errno, read_ret);
		exit(1);
	}
}

void read_thread_fn(int thread_idx) {
	unsigned int seedp = thread_idx;
	void *buffer = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
	if (!buffer) {
		printf("cannot allocate buffer\n");
		exit(1);
	}
	memset(buffer, 0, PAGE_SIZE);

	int *fd_arr = (int *) malloc(num_file * sizeof(int));
	if (!fd_arr) {
		printf("cannot allocate fs array\n");
		exit(1);
	}
	for (int file_idx = 0; file_idx < num_file; ++file_idx) {
		fd_arr[file_idx] = open(file_names[file_idx], O_DIRECT | O_RDONLY);
		if (fd_arr[file_idx] < 0) {
			printf("cannot open file, errno: %d\n", errno);
			exit(1);
		}
		long sys_ret = sys_bpf_set_level(fd_arr[file_idx], level);
		if (sys_ret < 0) {
			printf("sys_bpf_set_level error, ret: %ld\n", sys_ret);
			exit(1);
		}
	}

	steady_clock::time_point *start_time_arr = new steady_clock::time_point[iteration];
	if (!start_time_arr) {
		printf("cannot allocate start_time_arr\n");
		exit(1);
	}
	steady_clock::time_point *end_time_arr = new steady_clock::time_point[iteration];
	if (!end_time_arr) {
		printf("cannot allocate end_time_arr\n");
		exit(1);
	}

	for (long i = 0; i < iteration; i++) {
		start_time_arr[i] = steady_clock::now();
		read_key(fd_arr[rand_r(&seedp) % num_file], rand_r(&seedp) % MAX_PAGE_INDEX, buffer);
		end_time_arr[i] = steady_clock::now();
	}

	for (long i = 0; i < iteration; i++) {
		auto duration = duration_cast<nanoseconds>(end_time_arr[i] - start_time_arr[i]);
		latency_measure[thread_idx * iteration + i] = duration.count();
	}
}

int main(int argc, char *argv[]) {
	if (argc < 5) {
		printf("Usage: %s <num_thread> <level> <iteration> <filenames>\n", argv[0]);
		exit(1);
	}
	sscanf(argv[1], "%d", &num_thread);
	sscanf(argv[2], "%d", &level);
	sscanf(argv[3], "%ld", &iteration);
	num_file = argc - 4;
	file_names = argv + 4;

	latency_measure = (long *) malloc(sizeof(long) * num_thread * iteration);
	if (!latency_measure) {
		printf("cannot allocate measurements\n");
		return 1;
	}
	memset(latency_measure, 0, sizeof(long) * num_thread * iteration);

	thread *read_threads = new thread[num_thread];
	for (int i = 0; i < num_thread; i++) {
		read_threads[i] = thread(read_thread_fn, i);
	}
	for (int i = 0; i < num_thread; i++) {
		read_threads[i].join();
	}

	for (long i = 0; i < num_thread * iteration; ++i) {
		printf("%ld\n", latency_measure[i]);
	}
}
