/**
 * Bandwdith Lock User Library
 *
 * This program provides easy-to-integrate library calls for using
 * the bandwidth lock module. There are three steps invloved in this
 * process:
 * 1. User requests a free entry to gain access to the bandwidth
      lock module (get_mapped_buffer)
 * 2. User writes the requested lock-value (1 - lock | 0 - unlock)
      to the index it has been assigned (ctl_info->val = {1, 0})
 * 3. User releases the bandwidth lock module (release_mapped_buffer)
 * 
 * Author : Waqar Ali (wali@ku.edu)
 *
 */

#include "bwlock.h"

#if (USE_BWLOCK == 1)

/**************************************************************************************************
 * Local Variables
 *************************************************************************************************/
static int 		dev_fd;
static int 		my_index;
static void		*shm_area;
static int		active_open = 0;
struct control_info* 	ctl_info;
static int		nesting_count = 0;

/**************************************************************************************************
* Function Definitions
 *************************************************************************************************/

/*
 * bw_lock
 * Function to request bandwidth lock
 */
void bw_lock(void)
{
	if (active_open == 0) {
		printf("[ERR] The application does not have an active session with the bandwidth lock module.\n");
		return;
	}

	/* Keep track of the lock's current nesting level */
	nesting_count++;

	/* Unconditionally acquire the lock */
	ctl_info->val = 1;

	return;
}

/*
 * bw_unlock
 * Function to release the bandwidth lock
 */
void bw_unlock(void)
{
	if (active_open == 0) {
		printf("[ERR] The application does not have an active session with the bandwidth lock module.\n");
		return;
	}

	/* Ascertain whether the lock has been nested */
	if (nesting_count > 1) {
		/* Nested lock case : Only decrease the nesting count but don't release the lock */
		nesting_count--;
	} else if (nesting_count == 1) {
		/* Reached the outermost nesting level. Release the lock now */
		nesting_count = 0;
		ctl_info->val = 0;
	}

	return;
}

/*
 * bw_register
 * This function maps the kernel memory page for the target task to the task's memory space
 */
int bw_register(pid_t pid)
{
	dev_fd = open(DEVICE_FILE, O_RDONLY);

	USER_DEBUG(printf("[DBG] File Descriptor for BWLock Character Device : %d\n", dev_fd));

	shm_area = mmap(NULL, USER_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE, dev_fd, 0);

	if (shm_area < 0) {
		perror("bwlock_user");
		return BW_FAILURE;
	}

	my_index = ioctl(dev_fd, IOCTL_GET_INDEX, NULL);

	if (my_index < 0) {
		printf("[ERR] IOCTL with kernel failed : return value = %d\n", my_index); 
		return BW_FAILURE;
	}

	/* Calculate the return address based on the current index of the shared memory area */
	get_struct_at_index(ctl_info, shm_area, my_index);

	/* Populate the acquired structure with the values related to the requesting task */
	ctl_info->pid = pid;

	/* Mark this structure as taken */
	ctl_info->taken = 1;

	USER_DEBUG(printf("[DBG] Index assigned to this application : %d\n", my_index));
	USER_DEBUG(printf("[DBG] Application will be writing to the control_info structure at the address: %p\n", (void *)ctl_info));
	USER_DEBUG(printf("[DBG] pid : %d | val : %d\n", ctl_info->pid, ctl_info->val));

	/* Indicate an active session with the module */
	active_open = 1;

	return BW_SUCCESS; 
}

/* 
 * bw_unregister
 * This function releases the entry held by the calling process in the shared memory page of 
 * its core in the bandwidth lock kernel module
 */
void bw_unregister(void)
{
	/* Make sure that there is an open session with the driver */
	if (active_open == 0) {
		printf("[ERR] The application does not have an active session with the bandwidth lock module.\n");

		return;
	}

	get_struct_at_index(ctl_info, shm_area, my_index);

	/* Clear the 'taken' field in the entry occupied by this application 	in the shared-memory area */
	ctl_info->taken = 0;

	munmap(shm_area, USER_SIZE);

	close(dev_fd);

	/* Mark the session of this application with bwlock module as closed */
	active_open = 0;
	
	USER_DEBUG(printf("[DBG] Application has released the shared memory.\n"));

	return;
}

#endif /* (USE_BWLOCK == 1) */
