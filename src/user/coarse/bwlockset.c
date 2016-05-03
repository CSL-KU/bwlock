#define _GNU_SOURCE         /* See feature_test_macros(7) */

#include "bwlock.h"

void usage(char *argv[])
{
	printf("%s <pid> <bwlock_val>\n", argv[0]);
	exit(1);
}

int main(int argc, char *argv[])
{
	int pid, val;
	int status = BW_SUCCESS;

	if (argc != 3)
		usage(argv);

	pid = strtol(argv[1], NULL, 0);
	val = strtol(argv[2], NULL, 0);

	if (val == 0) {
		/* This is a call for releasing bandwidth lock */
		bw_unlock();
		bw_unregister();
	} else if (val == 1) {
		/* Application wants to set bandwidth lock */
		status = bw_register(pid);

		if (status == BW_FAILURE) {
			printf("Fatal Error : Bandwidth lock failed in memory-mapping stage\n");
			return -1;
		} else {
			printf("set pid=%d val=%d\n", pid, val);
			bw_lock();
		}
	} else {
		/* Application has provided invalid value for bandwidth lock */
		printf("Provided value (=%d) not recognized. Legal values are 0 <lock release> and 1 <lock set>\n", val);
		return -1;
	}
		
	return 0;
}
