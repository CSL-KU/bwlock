/**
 * FILE		: bwlockmod.h
 * BRIEF	: Declarations and prototypes for BWLOCK kernel module
 *
 * Copyright (C) 2017  Heechul Yun <heechul.yun@ku.edu>
 *
 * This file is distributed under the University of Illinois Open Source
 * License. See LICENSE.TXT for details.
 *
 */

#ifndef __BWLOCKMOD_H__
#define __BWLOCKMOD_H__

/**************************************************************************
 * Include Files
 *************************************************************************/
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/smp.h>
#include <linux/irq_work.h>
#include <linux/hardirq.h>
#include <linux/perf_event.h>
#include <linux/delay.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <asm/atomic.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>
#include <linux/notifier.h>
#include <linux/kthread.h>
#include <linux/printk.h>
#include <linux/interrupt.h>
#include <linux/cpu.h>
#include <linux/sched.h>

#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 8, 0)
#include <linux/sched/rt.h>
#endif

/**************************************************************************
 * Conditional Compilation Options
 *************************************************************************/
#define USE_DEBUG  		1
#define USE_BWLOCK_DYNPRIO 	1

/**************************************************************************
 * Public Definitions
 *************************************************************************/
#define MAX_NCPUS		32
#define CACHE_LINE_SIZE		64

#define BUF_SIZE 		256
#define PREDICTOR 		1  		/* 0 - used, 1 - ewma(a=1/2), 2 - ewma(a=1/4) */

#define MAXPERF_EVENTS		163840  	/* 10000 MB/s */
#define THROTTLE_EVENTS		1638   		/*   100 MB/s */

#if USE_DEBUG
#  define DEBUG(x)
#  define DEBUG_RECLAIM(x)
#  define DEBUG_USER(x)
#  define DEBUG_BWLOCK(x)	x
#  define DEBUG_PROFILE(x)	x
#else
#  define DEBUG(x) 
#  define DEBUG_RECLAIM(x)
#  define DEBUG_USER(x)
#  define DEBUG_PROFILE(x)
#endif

#if USE_BWLOCK_DYNPRIO
#define MAX_DYNPRIO_TSKS	20
#define NICE_TO_PRIO(nice)	(MAX_RT_PRIO + (nice) + 20)
#define PRIO_TO_NICE(prio)	((prio) - MAX_RT_PRIO - 20)
#endif

/**************************************************************************
 * Public Types
 **************************************************************************/
struct memstat{
	u64 			used_budget;         		/* used budget*/
	u64 			assigned_budget;
	u64 			throttled_time_ns;   
	int 			throttled;           		/* throttled period count */
	u64 			throttled_error;     		/* throttled & error */
	int 			throttled_error_dist[10]; 	/* pct distribution */
	int 			exclusive;           		/* exclusive period count */
	u64 			exclusive_ns;        		/* exclusive mode real-time duration */
	u64 			exclusive_bw;        		/* exclusive mode used bandwidth */
};

#if USE_BWLOCK_DYNPRIO
struct dynprio {
	struct task_struct	*task;
	int			origprio;
	ktime_t 		start;
};
#endif

/* Per-CPU Information Control Block for Bandwidth Lock Kernel Module */
struct core_info {
	int 			budget;              		/* assigned budget */
	int 			limit;               		/* limit mode (exclusive to weight)*/
	int 			wsum;                		/* local copy of global->wsum */

#if USE_BWLOCK_DYNPRIO
	struct dynprio 		dprio[100];
	int 			dprio_cnt;
#endif

	int 			cur_budget;          		/* currently available budget */
	struct task_struct 	*throttled_task;
	ktime_t 		throttled_time;  		/* absolute time when throttled */
	u64 			old_val;             		/* hold previous counter value */
	int 			prev_throttle_error; 		/* check whether there was throttle error in 
						    		   the previous period */

	u64 			exclusive_vtime;     		/* exclusive mode vtime for scheduling */
	int 			exclusive_mode;      		/* 1 - if in exclusive mode */
	ktime_t 		exclusive_time;  		/* time when exclusive mode begins */
	struct irq_work		pending; 			/* delayed work for NMIs */
	struct perf_event 	*event;				/* performance counter i/f */

	struct task_struct 	*throttle_thread;  		/* forced throttle idle thread */
	wait_queue_head_t 	throttle_evt; 			/* throttle wait queue */

	struct memstat 		overall;  			/* stat for overall periods. reset by user */
	int 			used[3];             		/* EWMA memory load */
	long 			period_cnt;         		/* active periods count */
};

/* Global Information Control Block for Bandwidth Lock Kernel Module */
struct bwlockmod_info {
	int 			master;
	ktime_t 		period_in_ktime;
	int 			start_tick;
	int 			budget;              		/* reclaimed budget */
	long 			period_cnt;
	spinlock_t 		lock;

	cpumask_var_t 		throttle_mask;
	cpumask_var_t 		active_mask;
	atomic_t 		wsum;
	int 			bwlocked_cores;
	struct hrtimer 		hr_timer;
};

/**************************************************************************
 * External Function Prototypes
 **************************************************************************/
extern int 		idle_cpu 			(	int cpu				);
extern int 		nr_bwlocked_cores 		(	void				);

/**************************************************************************
 * Local Function Prototypes
 **************************************************************************/

/*
 * This function initializes the bandwidth lock kernel module
 */
int			init_module			(	void				);

/*
 * This function cleans up after the bandwidth lock kernel module
 */
void			cleanup_module			(	void				);

/*
 * This function handles the overflow event.
 *
 * TODO: Identify everything this function does
 */
static void		bwlockmod_process_overflow	(	struct irq_work *entry		);

/*
 * This function is triggered in reponse to the HR-timer expiration on the master
 * core. It calls the slave-callback functions on all other system cores
 */
enum hrtimer_restart	period_timer_callback_master	(	struct hrtimer *timer		);

/*
 * This is the per-core handler function for periodic timer interrupt. The
 * main purpose of this function is to unblock a core if it was previously
 * throttled and replenish the budget of a core if required
 *
 * TODO: Create simplified pseduo-code implementaion of this function
 */
static void		period_timer_callback_slave	(	void *info			);

/*
 * This function invokes the function 'func', passing it the parameter 'info' as its input
 * on each core which is specified by the cpu-mask 'mask'.
 * Apparently, this function should be called with IRQs disabled
 *
 * TODO: What is the purpose of the 'wait' parameter?
 */
static void		bwlockmod_on_each_cpu_mask 	(	const struct cpumask *mask,
								smp_call_func_t func,
								void *info,
								bool wait			);

/*
 * This is the IRQ handler associated with PMC overflow interrupt. This function invokes
 * the current event handling function on the core on which it executes
 */
static void		event_overflow_callback 	(	struct perf_event *event,
								struct perf_sample_data *data,
								struct pt_regs *regs 		);

/*
 * This is the per-core initializer function which setups the data-structures
 * and system resources reqired by bandwidth lock kernel module
 */
static void		__init_per_core			(	void *info			);

/*
 * This function initializes the PMC events to be used by bandwidth lock kernel
 * module
 */
struct perf_event	*init_counter			(	int cpu,
								int budget			);

/*
 * This function disables the PM-event counter on a particular core
 */
static void		__disable_counter		(	void *info			);

/*
 * This function disables PM-event counters on all cores
 */
static void		disable_counters		(	void				);

/*
 * This function starts the PM-event counter on a particular core
 */
static void		__start_counter			(	void *info			);

/*
 * This function starts PM-event counters on all cores
 */
static void		start_counters			(	void				);

/*
 * This function increments the global budget
 *
 * TODO: The working of this function is not quite clear.
 */
static int		donate_budget			(	long cur_period,
								int budget 			);

/*
 * This function reduces the global budget
 *
 * TODO: The working of this function is not quite clear.
 */
static int		reclaim_budget			(	long cur_period,
								int budget			);

/*
 * This function reclaims budget from the global budget
 *
 * TODO: The working of this function is not quite clear.
 */
static int		request_budget			(	struct core_info *cinfo		);

/*
 * This function converts memory bandwidth into LLC-events
 */
static inline u64	convert_mb_to_events		(	int mb  			);

/*
 * This function converts the LLC-miss events into memory bandwidth
 */
static inline int	convert_events_to_mb 		(	u64 events 			);

/*
 * This function calculates the number of performance monitoring events which have
 * been registered so far in the current period
 */
static inline u64	perf_event_count 		(	struct perf_event *event 	);

/*
 * This function calculates the number of PMC events the core has used so far since
 * the last period
 */
static inline u64	bwlockmod_event_used 		(	struct core_info *cinfo 	);

/*
 * This function calculates the number of cores which are currently holding bandwdith lock
 * and returns that number to the caller as an integer
 *
 * TODO: Why not use the nr_bwlocked_cores () function directly?
 */
static int		mg_nr_bwlocked_cores 		(	void 				);

static void		print_core_info 		(	int cpu, 
								struct core_info *cinfo		);
/*
 * This function prints the value of the current context (e.g. IRQ, NMI etc.) to the
 * trace buffer
 */
static inline void	print_current_context 		(	void 				);

/* 
 * This function prints the state of the CPU (online / offline) to the trace buffer. The
 * parameter 'nfb' is not used in the function body
 */
static int		bwlockmod_cpu_callback 		(	struct notifier_block *nfb,
								unsigned long action,
								void *hcpu 			);

/*
 * TODO: Identify the purpose of this function
 */
static int		bwlockmod_idle_notifier		(	struct notifier_block *nb,
								unsigned long val,
								void *data			);

void			update_statistics		(	struct core_info *cinfo		);
/*
 * This is the routine which describes the behavior of kthrottle thread which
 * is used to block a core which has consumed its bandwidth budget
 */
static int		throttle_thread			(	void *args			);

/*
 * This function kills the kthrottle thread which is used to block a core
 */
static void		__kill_throttlethread		(	void *info			);

/*
 * This function is used to unthrottle a currently blocked core
 */
static void		__unthrottle_core		(	void *info			);

/*
 * TODO: Identify what this function does.
 */
static void		__newperiod			(	void *info			);

/*
 * TODO: Identify the purpose of dynamic priority
 */
#if USE_BWLOCK_DYNPRIO
static void		set_load_weight			(	struct task_struct *p		);

void			intr_set_user_nice		(	struct task_struct *p,
								long nice			);
#endif

/* File IO Related Functions */
static ssize_t		bwlockmod_control_write		(	struct file *filp,
								const char __user *ubuf,
								size_t cnt,
								loff_t *ppos			);

static int		bwlockmod_control_show		(	struct seq_file *m,
								void *v				);

static int		bwlockmod_control_open		(	struct inode *inode,
								struct file *filp		);

static void		__update_budget			(	void *info			);

static ssize_t		bwlockmod_limit_write		(	struct file *filp,
								const char __user *ubuf,
								size_t cnt,
								loff_t *ppos			);

static int		bwlockmod_limit_show		(	struct seq_file *m,
								void *v				);

static int		bwlockmod_limit_open		(	struct inode *inode,
								struct file *filp		);

static int		bwlockmod_usage_show		(	struct seq_file *m,
								void *v				);

static int		bwlockmod_usage_open		(	struct inode *inode,
								struct file *filp		);

static void		__reset_stats			(	void *info			);

static ssize_t		bwlockmod_failcnt_write		(	struct file *filp,
								const char __user *ubuf,
								size_t cnt,
								loff_t *ppos			);

static int		bwlockmod_failcnt_show		(	struct seq_file *m,
								void *v				);

static int		bwlockmod_failcnt_open		(	struct inode *inode,
								struct file *filp		);

static int		bwlockmod_init_debugfs		(	void				);

/*
 * TODO: Identify the purpose of this function
 */
static void		test_ipi_cb			(	void *info			);

/*
 * TODO: Identify the purpose of this function
 */
enum hrtimer_restart	test_timer_cb			(	struct hrtimer *timer		);

/*
 * TODO: Identify the purpose of this function
 */
static int		self_test			(	void				);

#endif /* __BWLOCKMOD_H__ */
