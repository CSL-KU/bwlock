#define _GNU_SOURCE         /* See feature_test_macros(7) */

#include "bwlock.h"

#define ITER 1000000000

uint64_t get_elapsed(struct timespec *start, struct timespec *end)
{
	uint64_t dur;
	if (start->tv_nsec > end->tv_nsec)
		dur = (uint64_t)(end->tv_sec - 1 - start->tv_sec) * 1000000000 +
			(1000000000 + end->tv_nsec - start->tv_nsec);
	else
		dur = (uint64_t)(end->tv_sec - start->tv_sec) * 1000000000 +
			(end->tv_nsec - start->tv_nsec);

	return dur;
}

int main()
{
	uint64_t tmpdiff;
	struct timespec start, end;
	int seconds = 0;

	int			status = BW_SUCCESS;
	pid_t			my_pid;

	my_pid = getpid();

	status = bw_register(my_pid);

	if (status == BW_FAILURE) {
		/* Failed to acquire access to shared memory area */
		return -1;
	}
	
	printf("my_pid : %d\n", my_pid);

	printf("enter memory critical section\n");

	bw_lock();

	for (seconds = 0; seconds <= 10; seconds++) {

		clock_gettime(CLOCK_REALTIME, &start);	
repeat:
		/* Acquire the lock at 1 second */
		if (seconds == 1) bw_lock();

		clock_gettime(CLOCK_REALTIME, &end);
		tmpdiff = get_elapsed(&start, &end);

		/* Release the lock after 5 seconds */
		if (seconds == 5) bw_unlock();

		if (tmpdiff < 1000000000) goto repeat;
	}
		
	bw_unlock();

	printf("exit memory critical section\n");

	bw_unregister();

	return 0;
}
