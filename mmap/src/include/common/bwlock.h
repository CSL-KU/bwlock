/**
 * BwLock: memory bandwidth lock.
 *
 * Copyright (C) 2014  Heechul Yun <heechul.yun@ku.edu> 
 *
 * This file is distributed under the GPLv2 License. 
 */ 

#ifndef BWLOCK_H
#define BWLOCK_H

#ifndef __KERNEL__

/******************************************************************************* 
 * NOTE : Definitions and includes for user-space bandwidth lock library only 
 ******************************************************************************/

#include <unistd.h>
#include <sys/syscall.h>   /* For SYS_xxx definitions */
#include <time.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <sched.h>

#define USER_SIZE			sizeof(struct control_info)

#define USE_BWLOCK 			1

#define USER_DEBUG(x)			

#define BW_SUCCESS			0
#define BW_FAILURE			-1

#if USE_BWLOCK
extern struct control_info* ctl_info;

int  bw_register(pid_t pid);
void bw_unregister(void);
void bw_lock(void);
void bw_unlock(void);

#else

#define bw_register()			BW_FAILURE
#define bw_unregister()
#define bw_lock()    
#define bw_unlock()  

#endif /* USE_BWLOCK */

#else

/******************************************************************************* 
 * NOTE : Definitions for kernel-space bandwidth lock module only 
 ******************************************************************************/

#define TBL_SIZE			100
#define MMT_BUF_SIZE			8192

#endif /* !__KERNEL__  */

/******************************************************************************* 
 * NOTE : Definitions common for user-space and kernel-space
 *******************************************************************************/

#define DEVICE_NAME			"bwlockdev"
#define DEVICE_FILE			"/dev/bwlockdev"

#define IOCTL_GET_INDEX 		1
#define PARAM_NOT_RECOGNIZED		-1
#define NO_PENDING_TRANSACTIONS 	-2

#define get_struct_at_index(return_struct, start_address, index)				\
		do {										\
			return_struct = (struct control_info *)(start_address +			\
					sizeof(struct control_info) * index);			\
		} while(0);


/* This is the structure that the user-space writes to and the kernel-space
   reads from */
struct control_info {
	pid_t	pid;
	int	val;
	int	taken;
};

#endif /* BWLOCK_H */
