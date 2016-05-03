/**
 * Memory bandwidth controller for multi-core systems
 *
 * Copyright (C) 2014  Heechul Yun <heechul.yun@ku.edu>
 *
 * This file is distributed under the University of Illinois Open Source
 * License. See LICENSE.TXT for details.
 *
 */

/**************************************************************************
 * Conditional Compilation Options
 **************************************************************************/
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#define USE_DEBUG  1
#define USE_BWLOCK_DYNPRIO 1

/**************************************************************************
 * Included Files
 **************************************************************************/
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/smp.h> /* IPI calls */
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
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 8, 0)
#  include <linux/sched/rt.h>
#endif
#include <linux/cpu.h>
#include <asm/idle.h>
#include <linux/sched.h>

#include <linux/fs.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/pid.h>

#include "bwlock.h"

/**************************************************************************
 * Public Definitions
 **************************************************************************/
#define MAX_NCPUS 32
#define CACHE_LINE_SIZE 64

#if USE_DEBUG
#  define DEBUG(x)
#  define DEBUG_RECLAIM(x)
#  define DEBUG_USER(x)
#  define DEBUG_BWLOCK(x) 
#  define DEBUG_PROFILE(x) 

#  define DEV_DEBUG(x) 
#  define DEV_DEBUGX(x) x

#else
#  define DEBUG(x) 
#  define DEBUG_RECLAIM(x)
#  define DEBUG_USER(x)
#  define DEBUG_BWLOCK(x)
#  define DEBUG_PROFILE(x)

#  define DEV_DEBUG(x) 
#  define DEV_DEBUGX(x) 

#endif

#define BUF_SIZE 256
#define PREDICTOR 1  /* 0 - used, 1 - ewma(a=1/2), 2 - ewma(a=1/4) */

#define field32_get_mask(field32, bit_index)		(field32 & (1 << bit_index))
#define field32_get_index(bit_index, array_index)	(bit_index + array_index * 32)
#define field32_set_bit(bit_index)			(1 << bit_index)
#define field32_clear_bit(bit_index)			(~(1 << bit_index))

#if USE_DEBUG
#define DEBUG_DECLARE_VARS(level)												\
	static int	lock_count = 0;												\
	static int 	set_count = 0;												\
	static int 	clear_count = 0;											\
	static char 	entry_class[] = level;											\
	static int	old_val	= 0;												\

#define DEBUG_RESET_VARS()													\
	lock_count = 0;														\
	set_count = 0;														\
	clear_count = 0;													\
	old_val = 0;														

#define DEBUG_PRINT_ENTRY_INFO(ctl_info, bit_index, array_index)								\
	do {															\
		lock_count += ctl_info->val;											\
		if (ctl_info->val != old_val) {											\
			old_val = ctl_info->val;										\
			set_count += old_val;											\
			clear_count += !(old_val);										\
			trace_printk("%s Table-Entry    : %d\n", entry_class, field32_get_index(bit_index, array_index));	\
			trace_printk("Entry Address     : %p\n", (void *)ctl_info);						\
			trace_printk("Entry->pid        : %05d\n", (int)ctl_info->pid);						\
			trace_printk("Entry->val        : %05d\n", ctl_info->val);						\
			trace_printk("Set Count         : %05d\n", set_count);							\
			trace_printk("Clear Count       : %05d\n", clear_count);						\
			trace_printk("Locked Intervals  : %05d\n\n", lock_count);						\
		}														\
	} while (0);
#else
#define DEBUG_DECLARE_VARS(level)
#define DEBUG_RESET_VARS()
#define DEBUG_PRINT_ENTRY_INFO(ctl_info, bit_index, array_index)
#endif						


/**************************************************************************
 * Public Types
 **************************************************************************/
struct memstat{
	u64 used_budget;         /* used budget*/
	u64 assigned_budget;
	u64 throttled_time_ns;   
	int throttled;           /* throttled period count */
	u64 throttled_error;     /* throttled & error */
	int throttled_error_dist[10]; /* pct distribution */
	int exclusive;           /* exclusive period count */
	u64 exclusive_ns;        /* exclusive mode real-time duration */
	u64 exclusive_bw;        /* exclusive mode used bandwidth */
};

#if USE_BWLOCK_DYNPRIO

#define MAX_DYNPRIO_TSKS 20

struct dynprio {
	struct task_struct *task;
	int origprio;
	ktime_t start;
};
#endif

/* percpu info */
struct core_info {
	/* user configurations */
	int budget;              /* assigned budget */

	int limit;               /* limit mode (exclusive to weight)*/
	int wsum;                /* local copy of global->wsum */
#if USE_BWLOCK_DYNPRIO
	struct dynprio dprio[100];
	int dprio_cnt;
#endif
	/* for control logic */
	int cur_budget;          /* currently available budget */

	struct task_struct * throttled_task;
	
	ktime_t throttled_time;  /* absolute time when throttled */

	u64 old_val;             /* hold previous counter value */
	int prev_throttle_error; /* check whether there was throttle error in 
				    the previous period */

	u64 exclusive_vtime;     /* exclusive mode vtime for scheduling */

	int exclusive_mode;      /* 1 - if in exclusive mode */
	ktime_t exclusive_time;  /* time when exclusive mode begins */

	struct irq_work	pending; /* delayed work for NMIs */
	struct perf_event *event;/* performance counter i/f */

	struct task_struct *throttle_thread;  /* forced throttle idle thread */
	wait_queue_head_t throttle_evt; /* throttle wait queue */

	/* statistics */
	struct memstat overall;  /* stat for overall periods. reset by user */
	int used[3];             /* EWMA memory load */
	long period_cnt;         /* active periods count */

	spinlock_t core_lock;    /* spinlock to protect the shared-memory access from
				    intra-core interference */
	int index;		
	int free_index;
	int trans_pending;
	int valid[4];	
};

/* global info */
struct bwlockmod_info {
	int master;
	ktime_t period_in_ktime;
	int start_tick;
	int budget;              /* reclaimed budget */
	long period_cnt;
	spinlock_t lock;

	cpumask_var_t throttle_mask;
	cpumask_var_t active_mask;
	atomic_t wsum;
	int bwlocked_cores;
	struct hrtimer hr_timer;
};


/**************************************************************************
 * Global Variables
 **************************************************************************/
static struct bwlockmod_info 		bwlockmod_info;
static struct core_info __percpu	*core_info;

static void __percpu			*mmap_buffer;
static dev_t				first;
static struct cdev			c_dev;
static int				major;
static struct class			*c1;
static int				dev_open = 0;

static char *g_hw_type = "";
static int g_period_us = 1000;
static int g_use_reclaim = 0; /* minimum remaining time to reclaim */
static int g_use_bwlock = 1;
static int g_use_exclusive = 0;
static int g_budget_min_value = 1000;

static struct dentry *bwlockmod_dir;

static int g_test = 0;

/* copied from kernel/sched/sched.h */
static const int prio_to_weight[40] = {
 /* -20 */     88761,     71755,     56483,     46273,     36291,
 /* -15 */     29154,     23254,     18705,     14949,     11916,
 /* -10 */      9548,      7620,      6100,      4904,      3906,
 /*  -5 */      3121,      2501,      1991,      1586,      1277,
 /*   0 */      1024,       820,       655,       526,       423,
 /*   5 */       335,       272,       215,       172,       137,
 /*  10 */       110,        87,        70,        56,        45,
 /*  15 */        36,        29,        23,        18,        15,
};

static const u32 prio_to_wmult[40] = {
 /* -20 */     48388,     59856,     76040,     92818,    118348,
 /* -15 */    147320,    184698,    229616,    287308,    360437,
 /* -10 */    449829,    563644,    704093,    875809,   1099582,
 /*  -5 */   1376151,   1717300,   2157191,   2708050,   3363326,
 /*   0 */   4194304,   5237765,   6557202,   8165337,  10153587,
 /*   5 */  12820798,  15790321,  19976592,  24970740,  31350126,
 /*  10 */  39045157,  49367440,  61356676,  76695844,  95443717,
 /*  15 */ 119304647, 148102320, 186737708, 238609294, 286331153,
};

/* should be defined in the scheduler code */
static int sysctl_maxperf_bw_mb = 10000;
static int sysctl_throttle_bw_mb = 100;

/**************************************************************************
 * External Function Prototypes
 **************************************************************************/
extern int idle_cpu(int cpu);
extern int nr_bwlocked_cores(void);

/**************************************************************************
 * Local Function Prototypes
 **************************************************************************/
static int self_test(void);
static void __reset_stats(void *info);
static void period_timer_callback_slave(void *info);
enum hrtimer_restart period_timer_callback_master(struct hrtimer *timer);
static void bwlockmod_process_overflow(struct irq_work *entry);
static int throttle_thread(void *arg);
static void bwlockmod_on_each_cpu_mask(const struct cpumask *mask,
				      smp_call_func_t func,
				      void *info, bool wait);

/*
 * Tentative functions for communication with user-space programs via mmap
 */
static int device_open(struct inode *inode, struct file *filp);
static int device_release(struct inode *inode, struct file *filp);
static int device_mmap(struct file *filp, struct vm_area_struct *vma);
long device_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);

/**************************************************************************
 * Module parameters
 **************************************************************************/

module_param(g_test, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(g_test, "number of test iterations");

module_param(g_hw_type, charp,  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(g_hw_type, "hardware type");

module_param(g_use_reclaim, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(g_use_reclaim, "enable/disable reclaim");

module_param(g_use_bwlock, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(g_use_bwlock, "enable/disable reclaim");

module_param(g_period_us, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(g_period_us, "throttling period in usec");


/**************************************************************************
 * Module main code
 **************************************************************************/

static void mg_cp_user_data(void *target)
{
	struct core_info *cinfo 	= this_cpu_ptr(core_info);
	void *mm_buffer 		= this_cpu_ptr(mmap_buffer);
	struct task_struct *lock_target = (struct task_struct *)target;
	int valid_field 		= 0;
	int field_index 		= 0;
	int index_value 		= 0;
	struct control_info *ctl_info 	= NULL;
	int i;

	DEBUG_DECLARE_VARS("VM")

	/* This processing should take place under lock */
	spin_lock(&cinfo->core_lock);

	for (i = 0; i <= (TBL_SIZE/32); i++) {
		valid_field = cinfo->valid[i];

		field_index = 0;

		while (valid_field) {
			index_value = field32_get_mask(valid_field, field_index);
			
			if (index_value) {
				get_struct_at_index(ctl_info, mm_buffer, field32_get_index(field_index, i));

				if(lock_target->pid == ctl_info->pid) {
					DEBUG_PRINT_ENTRY_INFO(ctl_info, field_index, i);
					
					if(ctl_info->taken == 0) {
						/* User-Space has released this entry */
						lock_target->bwlock_val = 0;
					} else {
						/* Copy the requisite value from control structure to task control block */
						lock_target->bwlock_val = ctl_info->val;
					} 
				}

				/* This means that the user-space has released this buffer.
				   This is here (and not inside the last if)  to deal with the 
				   special case when the user-space program exits immediately  
				   after writing to the shared memory. In this case we won't 
				   have a valid pid in the control structure but the entry for 
				   that structure may still show as occupied in the valid fields.
				   However, this needs to happen on only one core which can be
				   any one of the system cores */
				if (ctl_info->taken == 0) {
					trace_printk("Entry is being released\n");
					ctl_info->val = 0;
					DEBUG_PRINT_ENTRY_INFO(ctl_info, field_index, i);
					cinfo->valid[i] &= field32_clear_bit(field_index);
					cinfo->index--;
					DEBUG_RESET_VARS()
				}
			}

			valid_field &= field32_clear_bit(field_index);
			field_index++;
			
			if (field_index > 31) {
				DEV_DEBUGX(trace_printk("[ERR] Problem - The loop should have stopped!\n"));
				break;
			}
		}
	}

	/* Release the spinlock */
	spin_unlock(&cinfo->core_lock);

	return;
}


static int mg_nr_bwlocked_cores(void)
{
	int i = 0;
	int nr = 0;
	struct bwlockmod_info 	*global = &bwlockmod_info;

	nr = nr_bwlocked_cores(); 
	
	for_each_cpu(i, global->throttle_mask) {
		struct task_struct *t = per_cpu_ptr(core_info, i)->throttled_task;
		if (t && t->bwlock_val > 0)
			nr += t->bwlock_val;
	}	

	return nr;
}


/* similar to on_each_cpu_mask(), but this must be called with IRQ disabled */
static void bwlockmod_on_each_cpu_mask(const struct cpumask *mask, 
				      smp_call_func_t func,
				      void *info, bool wait)
{
	int cpu = smp_processor_id();
	smp_call_function_many(mask, func, info, wait);
	if (cpumask_test_cpu(cpu, mask)) {
		func(info);
	}
}

static int bwlockmod_cpu_callback(struct notifier_block *nfb,
					 unsigned long action, void *hcpu)
{
	unsigned int cpu = (unsigned long)hcpu;
	
	switch (action) {
	case CPU_ONLINE:
	case CPU_ONLINE_FROZEN:
		trace_printk("CPU%d is online\n", cpu);
		break;
	case CPU_DEAD:
	case CPU_DEAD_FROZEN:
		trace_printk("CPU%d is offline\n", cpu);
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block bwlockmod_cpu_notifier =
{
	.notifier_call = bwlockmod_cpu_callback,
};


/** convert MB/s to #of events (i.e., LLC miss counts) per 1ms */
static inline u64 convert_mb_to_events(int mb)
{
	return div64_u64((u64)mb*1024*1024,
			 CACHE_LINE_SIZE* (1000000/g_period_us));
}
static inline int convert_events_to_mb(u64 events)
{
	int divisor = g_period_us*1024*1024;
	int mb = div64_u64(events*CACHE_LINE_SIZE*1000000 + (divisor-1), divisor);
	return mb;
}

static inline void print_current_context(void)
{
	DEBUG(trace_printk("in_interrupt(%ld)(hard(%ld),softirq(%d)"
		     ",in_nmi(%d)),irqs_disabled(%d)\n",
		     in_interrupt(), in_irq(), (int)in_softirq(),
		     (int)in_nmi(), (int)irqs_disabled()));
}

/** read current counter value. */
static inline u64 perf_event_count(struct perf_event *event)
{
	return local64_read(&event->count) + 
		atomic64_read(&event->child_count);
}

/** return used event in the current period */
static inline u64 bwlockmod_event_used(struct core_info *cinfo)
{
	return perf_event_count(cinfo->event) - cinfo->old_val;
}

static void print_core_info(int cpu, struct core_info *cinfo)
{
	pr_info("CPU%d: budget: %d, cur_budget: %d, period: %ld\n", 
	       cpu, cinfo->budget, cinfo->cur_budget, cinfo->period_cnt);
}

/**
 * update per-core usage statistics
 */
void update_statistics(struct core_info *cinfo)
{
	/* counter must be stopped by now. */
	s64 new;
	int used;
	u64 exclusive_vtime = 0;

	new = perf_event_count(cinfo->event);
	used = (int)(new - cinfo->old_val); 

	cinfo->old_val = new;
	cinfo->overall.used_budget += used;
	cinfo->overall.assigned_budget += cinfo->budget;

	/* EWMA filtered per-core usage statistics */
	cinfo->used[0] = used;
	cinfo->used[1] = (cinfo->used[1] * (2-1) + used) >> 1; 
	/* used[1]_k = 1/2 used[1]_k-1 + 1/2 used */
	cinfo->used[2] = (cinfo->used[2] * (4-1) + used) >> 2; 
	/* used[2]_k = 3/4 used[2]_k-1 + 1/4 used */

	/* core is currently throttled. */
	if (cinfo->throttled_task) {
		cinfo->overall.throttled_time_ns +=
			(ktime_get().tv64 - cinfo->throttled_time.tv64);
		cinfo->overall.throttled++;
	}

	/* throttling error condition:
	   I was too aggressive in giving up "unsed" budget */
	if (cinfo->prev_throttle_error && used < cinfo->budget) {
		int diff = cinfo->budget - used;
		int idx;
		cinfo->overall.throttled_error ++; // += diff;
		BUG_ON(cinfo->budget == 0);
		idx = (int)(diff * 10 / cinfo->budget);
		cinfo->overall.throttled_error_dist[idx]++;
		trace_printk("ERR: throttled_error: %d < %d\n", used, cinfo->budget);
		/* compensation for error to catch-up*/
		cinfo->used[PREDICTOR] = cinfo->budget + diff;
	}
	cinfo->prev_throttle_error = 0;

	/* I was the lucky guy who used the DRAM exclusively */
	if (cinfo->exclusive_mode) {
		u64 exclusive_ns, exclusive_bw;

		/* used time */
		exclusive_ns = (ktime_get().tv64 - cinfo->exclusive_time.tv64);
		
		/* used bw */
		exclusive_bw = (cinfo->used[0] - cinfo->budget);

		cinfo->exclusive_vtime += exclusive_vtime;

		cinfo->overall.exclusive_ns += exclusive_ns;
		cinfo->overall.exclusive_bw += exclusive_bw;
		cinfo->exclusive_mode = 0;
		cinfo->overall.exclusive++;
	}
	DEBUG_PROFILE(trace_printk("%lld %d %p CPU%d org: %d cur: %d period: %ld\n",
			   new, used, cinfo->throttled_task,
			   smp_processor_id(), 
			   cinfo->budget,
			   cinfo->cur_budget,
			   cinfo->period_cnt));
}


/**
 * budget is used up. PMU generate an interrupt
 * this run in hardirq, nmi context with irq disabled
 */
static void event_overflow_callback(struct perf_event *event,
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 2, 0)
				    int nmi,
#endif
				    struct perf_sample_data *data,
				    struct pt_regs *regs)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	BUG_ON(!cinfo);
	irq_work_queue(&cinfo->pending);
}

/* must be in hardirq context */
static int donate_budget(long cur_period, int budget)
{
	struct bwlockmod_info *global = &bwlockmod_info;
	spin_lock(&global->lock);
	if (global->period_cnt == cur_period) {
		global->budget += budget;
	}
	spin_unlock(&global->lock);
	return global->budget;
}

/* must be in hardirq context */
static int reclaim_budget(long cur_period, int budget)
{
	struct bwlockmod_info *global = &bwlockmod_info;
	int reclaimed = 0;
	spin_lock(&global->lock);
	if (global->period_cnt == cur_period) {
		reclaimed = min(budget, global->budget);
		global->budget -= reclaimed;
	}
	spin_unlock(&global->lock);
	return reclaimed;
}

/**
 * reclaim local budget from global budget pool
 */
static int request_budget(struct core_info *cinfo)
{
	int amount = 0;
	int budget_used = bwlockmod_event_used(cinfo);

	BUG_ON(!cinfo);

	if (budget_used < cinfo->budget && current->policy == SCHED_NORMAL) {
		/* didn't used up my original budget */
		amount = cinfo->budget - budget_used;
	} else {
		/* I'm requesting more than I originall assigned */
		amount = g_budget_min_value;
	}

	if (amount > 0) {
		/* successfully reclaim my budget */
		amount = reclaim_budget(cinfo->period_cnt, amount);
	}
	return amount;
}

/**
 * called by process_overflow
 */
static void __unthrottle_core(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	if (cinfo->throttled_task) {
		cinfo->exclusive_mode = 1;
		cinfo->exclusive_time = ktime_get();

		cinfo->throttled_task = NULL;
		smp_wmb();
		DEBUG_RECLAIM(trace_printk("exclusive mode begin\n"));
	}
}

static void __newperiod(void *info)
{
	long period = (long)info;
	ktime_t start = ktime_get();
	struct bwlockmod_info *global = &bwlockmod_info;
	
	spin_lock(&global->lock);
	if (period == global->period_cnt) {
		ktime_t new_expire = ktime_add(start, global->period_in_ktime);
		long new_period = ++global->period_cnt;
		global->budget = 0;
		spin_unlock(&global->lock);

		/* arrived before timer interrupt is called */
		hrtimer_start_range_ns(&global->hr_timer, new_expire,
				       0, HRTIMER_MODE_ABS_PINNED);
		DEBUG(trace_printk("begin new period\n"));

		on_each_cpu(period_timer_callback_slave, (void *)new_period, 0);
	} else
		spin_unlock(&global->lock);
}

#ifdef USE_BWLOCK_DYNPRIO
static void set_load_weight(struct task_struct *p)
{
	int prio = p->static_prio - MAX_RT_PRIO;
	struct load_weight *load = &p->se.load;
	load->weight = prio_to_weight[prio];
	load->inv_weight = prio_to_wmult[prio];
}

void intr_set_user_nice(struct task_struct *p, long nice)
{
	if (task_nice(p) == nice || nice < -20 || nice > 19)
		return;

	if (p->policy != SCHED_NORMAL)
		return;

	p->static_prio = NICE_TO_PRIO(nice);
	set_load_weight(p);
	p->prio = p->static_prio;
}
#endif

/**
 * memory overflow handler.
 * must not be executed in NMI context. but in hard irq context
 */
static void bwlockmod_process_overflow(struct irq_work *entry)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	struct bwlockmod_info *global = &bwlockmod_info;
	int i;
	int amount = 0;
	ktime_t start = ktime_get();
	s64 budget_used;
	BUG_ON(in_nmi() || !in_irq());

	spin_lock(&global->lock);
	if (!cpumask_test_cpu(smp_processor_id(), global->active_mask)) {
		spin_unlock(&global->lock);
		trace_printk("ERR: not active\n");
		return;
	} else if (global->period_cnt != cinfo->period_cnt) {
		trace_printk("ERR: global(%ld) != local(%ld) period mismatch\n",
			     global->period_cnt, cinfo->period_cnt);
		spin_unlock(&global->lock);
		return;
	}
	spin_unlock(&global->lock);

	budget_used = bwlockmod_event_used(cinfo);

	/* erroneous overflow, that could have happend before period timer
	   stop the pmu */
	if (budget_used < cinfo->cur_budget) {
		trace_printk("ERR: overflow in timer. used %lld < cur_budget %d. ignore\n",
			     budget_used, cinfo->cur_budget);
		return;
	}

	/* try to reclaim budget from the global pool */
	amount = request_budget(cinfo);
	if (amount > 0) {
		cinfo->cur_budget += amount;
		local64_set(&cinfo->event->hw.period_left, amount);
		DEBUG_RECLAIM(trace_printk("successfully reclaimed %d\n", amount));
		return;
	}

	/* no more overflow interrupt */
	local64_set(&cinfo->event->hw.period_left, 0xfffffff);

	/* check if we donated too much */
	if (budget_used < cinfo->budget) {
		trace_printk("ERR: throttling error\n");
		cinfo->prev_throttle_error = 1;
	}

	/* we are going to be throttled */
	spin_lock(&global->lock);
	cpumask_set_cpu(smp_processor_id(), global->throttle_mask);
	if (cpumask_equal(global->throttle_mask, global->active_mask)) {
		/* all other cores are alreay throttled */
		spin_unlock(&global->lock);
		if (g_use_exclusive == 1) {
			/* algorithm 1: last one get all remaining time */
			cinfo->exclusive_mode = 1;
			cinfo->exclusive_time = ktime_get();
			DEBUG_RECLAIM(trace_printk("exclusive mode begin\n"));
			return;
		} else if (g_use_exclusive == 2) {
			/* algorithm 2: wakeup all (i.e., non regulation) */
			smp_call_function(__unthrottle_core, NULL, 0);
			cinfo->exclusive_mode = 1;
			cinfo->exclusive_time = ktime_get();
			DEBUG_RECLAIM(trace_printk("exclusive mode begin\n"));
			return;
		} else if (g_use_exclusive == 5) {
			smp_call_function_single(global->master, __newperiod, 
					  (void *)cinfo->period_cnt, 0);
			return;
		} else if (g_use_exclusive > 5) {
			trace_printk("ERR: Unsupported exclusive mode %d\n", 
				     g_use_exclusive);
			return;
		} else if (g_use_exclusive != 0 &&
			   cpumask_weight(global->active_mask) == 1) {
			trace_printk("ERR: don't throttle one active core\n");
			return;
		}
	} else
		spin_unlock(&global->lock);
	

	if (cinfo->prev_throttle_error)
		return;
	/*
	 * fail to reclaim. now throttle this core
	 */
	DEBUG_RECLAIM(trace_printk("fail to reclaim after %lld nsec.\n",
				   ktime_get().tv64 - start.tv64));

	/* wake-up throttle task */
	cinfo->throttled_task = current;
	cinfo->throttled_time = start;

#ifdef USE_BWLOCK_DYNPRIO
	/* dynamically lower the throttled task's priority */
	// TBD: register the 'current' task
	for (i = 0; i < cinfo->dprio_cnt; i++) {
		if (cinfo->dprio[i].task == current)
			break;
	}
	if (i == cinfo->dprio_cnt && i < MAX_DYNPRIO_TSKS) {
		cinfo->dprio[i].task = current;
		cinfo->dprio[i].start = start;
		cinfo->dprio[i].origprio = task_nice(current);
		cinfo->dprio_cnt++;
	}
	intr_set_user_nice(current, 19);
#endif
	WARN_ON_ONCE(!strncmp(current->comm, "swapper", 7));
	smp_mb();
	wake_up_interruptible(&cinfo->throttle_evt);
}

/**
 * per-core period processing
 *
 * called by scheduler tick to replenish budget and unthrottle if needed
 * run in interrupt context (irq disabled)
 */

/*
 * period_timer algorithm:
 *	excess = 0;
 *	if predict < budget:
 *	   excess = budget - predict;
 *	   global += excess
 *	set interrupt at (budget - excess)
 */
static void period_timer_callback_slave(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	struct bwlockmod_info *global = &bwlockmod_info;
	struct task_struct *target;
	long new_period = (long)info;
	int cpu = smp_processor_id();
	int i;

	/* must be irq disabled. hard irq */
	BUG_ON(!irqs_disabled());
	WARN_ON_ONCE(!in_irq());

	if (new_period <= cinfo->period_cnt) {
		trace_printk("ERR: new_period(%ld) <= cinfo->period_cnt(%ld)\n",
			     new_period, cinfo->period_cnt);
		return;
	}


	/* assign local period */
	cinfo->period_cnt = new_period;

	/* stop counter */
	cinfo->event->pmu->stop(cinfo->event, PERF_EF_UPDATE);

	/* I'm actively participating */
	spin_lock(&global->lock);
	cpumask_clear_cpu(cpu, global->throttle_mask);
	cpumask_set_cpu(cpu, global->active_mask);
	spin_unlock(&global->lock);


	/* unthrottle tasks (if any) */
	if (cinfo->throttled_task)
		target = (struct task_struct *)cinfo->throttled_task;
	else
		target = current;
	cinfo->throttled_task = NULL;

	mg_cp_user_data((void *)target);

	/* bwlock check */
	if (g_use_bwlock) {
			//DEV_DEBUGX(trace_printk("[DBG-8-C%d] bwlocked_cores : %d | target : %p | target->bwlock_val : %d\n", cpu, global->bwlocked_cores, target, target->bwlock_val));
		if (global->bwlocked_cores > 0) {

			DEV_DEBUG(trace_printk("[DBG-8-C%d] bwlocked_cores : %d | target : %p | target->bwlock_val : %d\n", cpu, global->bwlocked_cores, target, target->bwlock_val));

			if (target->bwlock_val > 0)
				cinfo->limit = convert_mb_to_events(sysctl_maxperf_bw_mb);
			else
				cinfo->limit = convert_mb_to_events(sysctl_throttle_bw_mb);
		} else {
			cinfo->limit = convert_mb_to_events(sysctl_maxperf_bw_mb);
#if USE_BWLOCK_DYNPRIO
			/* TBD: if there was deprioritized tasks, restore their priorities */
			for (i = 0; i < cinfo->dprio_cnt; i++) {
				int oprio = cinfo->dprio[i].origprio;
				intr_set_user_nice(cinfo->dprio[i].task, oprio);
			}
			cinfo->dprio_cnt = 0;
#endif
		}
	}

	DEBUG_BWLOCK(trace_printk("%s|bwlock_val %d|g->bwlocked_cores %d\n", 
				  (current)?current->comm:"null", 
				  current->bwlock_val,global->bwlocked_cores));
	
	/* FIXME: cinfo->throttled_task below is always NULL because of Line-708 */
	DEBUG(trace_printk("%p|New period %ld. global->budget=%d\n",
			   cinfo->throttled_task,
			   cinfo->period_cnt, global->budget));
	
	/* update statistics. */
	update_statistics(cinfo);

	/* new budget assignment from user */
	spin_lock(&global->lock);

	if (cinfo->limit > 0) {
		/* limit mode */
		cinfo->budget = cinfo->limit;
	} else {
		WARN_ON_ONCE(1);
		trace_printk("ERR: both limit and weight = 0");
	}


	spin_unlock(&global->lock);

	/* budget can't be zero? */
	cinfo->budget = max(cinfo->budget, 1);

	if (cinfo->event->hw.sample_period != cinfo->budget) {
		/* new budget is assigned */
		DEBUG(trace_printk("MSG: new budget %d is assigned\n", 
				   cinfo->budget));
		cinfo->event->hw.sample_period = cinfo->budget;
	}


	/* per-task donation policy */
	if (!g_use_reclaim || rt_task(target)) {
		cinfo->cur_budget = cinfo->budget;
		DEBUG(trace_printk("HRT or !g_use_reclaim: don't donate\n"));
	} else if (target->policy == SCHED_BATCH || 
		   target->policy == SCHED_IDLE) {
		/* Non rt task: donate all */
		donate_budget(cinfo->period_cnt, cinfo->budget);
		cinfo->cur_budget = 0;
		DEBUG(trace_printk("NonRT: donate all %d\n", cinfo->budget));
	} else if (target->policy == SCHED_NORMAL) {
		BUG_ON(rt_task(target));
		if (cinfo->used[PREDICTOR] < cinfo->budget) {
			/* donate 'expected surplus' ahead of time. */
			int surplus = max(cinfo->budget - cinfo->used[PREDICTOR], 0);
			donate_budget(cinfo->period_cnt, surplus);
			cinfo->cur_budget = cinfo->budget - surplus;
			DEBUG(trace_printk("SRT: surplus: %d, budget: %d\n", surplus, 
					   cinfo->budget));
		} else {
			cinfo->cur_budget = cinfo->budget;
			DEBUG(trace_printk("SRT: don't donate\n"));
		}
	}

	/* setup an interrupt */
	cinfo->cur_budget = max(1, cinfo->cur_budget);
	local64_set(&cinfo->event->hw.period_left, cinfo->cur_budget);

	/* enable performance counter */
	cinfo->event->pmu->start(cinfo->event, PERF_EF_RELOAD);
}

static void __init_per_core(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	void *mm_buffer 	= this_cpu_ptr(mmap_buffer);
	memset(cinfo, 0, sizeof(struct core_info));

	/* Allocate the shared memory-space for processes running on this core */
	mm_buffer = (void *)get_zeroed_page(GFP_KERNEL);

	/* Initialize spinlock for accessing the shared memory */
	spin_lock_init(&cinfo->core_lock);

	smp_rmb();

	/* initialize per_event structure */
	cinfo->event = (struct perf_event *)info;

	/* initialize budget */
	cinfo->budget = cinfo->limit = cinfo->event->hw.sample_period;

	/* create idle threads */
	cinfo->throttled_task = NULL;

	init_waitqueue_head(&cinfo->throttle_evt);

	/* initialize statistics */
	__reset_stats(cinfo);

	print_core_info(smp_processor_id(), cinfo);

	smp_wmb();

	/* initialize nmi irq_work_queue */
	init_irq_work(&cinfo->pending, bwlockmod_process_overflow);
}

/**
 *   called while cpu_base->lock is held by hrtimer_interrupt()
 */
enum hrtimer_restart period_timer_callback_master(struct hrtimer *timer)
{
	struct bwlockmod_info *global = &bwlockmod_info;

	ktime_t now;
	int orun;
	long new_period;
	cpumask_var_t active_mask;

	now = timer->base->get_time();

        DEBUG(trace_printk("master begin\n"));
	BUG_ON(smp_processor_id() != global->master);

	orun = hrtimer_forward(timer, now, global->period_in_ktime);
	if (orun == 0)
		return HRTIMER_RESTART;

	spin_lock(&global->lock);
	global->period_cnt += orun;
	global->budget = 0;
	new_period = global->period_cnt;
	cpumask_copy(active_mask, global->active_mask);
	spin_unlock(&global->lock);

	DEBUG(trace_printk("spinlock end\n"));
	if (orun > 1)
		trace_printk("ERR: timer overrun %d at period %ld\n",
			    orun, new_period);

	global->bwlocked_cores = mg_nr_bwlocked_cores();

	bwlockmod_on_each_cpu_mask(active_mask,
		period_timer_callback_slave, (void *)new_period, 0);

	DEBUG(trace_printk("master end\n"));
	return HRTIMER_RESTART;
}

static struct perf_event *init_counter(int cpu, int budget)
{
	struct perf_event *event = NULL;
	struct perf_event_attr sched_perf_hw_attr = {
		/* use generalized hardware abstraction */
		.type           = PERF_TYPE_HARDWARE,
		.config         = PERF_COUNT_HW_CACHE_MISSES,
		.size		= sizeof(struct perf_event_attr),
		.pinned		= 1,
		.disabled	= 1,
		.exclude_kernel = 1,   /* TODO: 1 mean, no kernel mode counting */
	};

	if (!strcmp(g_hw_type, "core2")) {
		sched_perf_hw_attr.type           = PERF_TYPE_RAW;
		sched_perf_hw_attr.config         = 0x7024; /* 7024 - incl. prefetch 
							       5024 - only prefetch
							       4024 - excl. prefetch */
	} else if (!strcmp(g_hw_type, "snb")) {
		sched_perf_hw_attr.type           = PERF_TYPE_RAW;
		sched_perf_hw_attr.config         = 0x08b0; /* 08b0 - incl. prefetch */
	} else if (!strcmp(g_hw_type, "soft")) {
		sched_perf_hw_attr.type           = PERF_TYPE_SOFTWARE;
		sched_perf_hw_attr.config         = PERF_COUNT_SW_CPU_CLOCK;
	}

	/* select based on requested event type */
	sched_perf_hw_attr.sample_period = budget;

	/* Try to register using hardware perf events */
	event = perf_event_create_kernel_counter(
		&sched_perf_hw_attr,
		cpu, NULL,
		event_overflow_callback
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 2, 0)
		,NULL
#endif
		);

	if (!event)
		return NULL;

	if (IS_ERR(event)) {
		/* vary the KERN level based on the returned errno */
		if (PTR_ERR(event) == -EOPNOTSUPP)
			pr_info("cpu%d. not supported\n", cpu);
		else if (PTR_ERR(event) == -ENOENT)
			pr_info("cpu%d. not h/w event\n", cpu);
		else
			pr_err("cpu%d. unable to create perf event: %ld\n",
			       cpu, PTR_ERR(event));
		return NULL;
	}

	/* success path */
	pr_info("cpu%d enabled counter.\n", cpu);

	smp_wmb();

	return event;
}

static void __kill_throttlethread(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	pr_info("Stopping kthrottle/%d\n", smp_processor_id());
	cinfo->throttled_task = NULL;
	smp_mb();
}

static void __disable_counter(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	BUG_ON(!cinfo->event);

	/* stop the counter */
	cinfo->event->pmu->stop(cinfo->event, PERF_EF_UPDATE);
	cinfo->event->pmu->del(cinfo->event, 0);

	pr_info("LLC bandwidth throttling disabled\n");
}

static void disable_counters(void)
{
	on_each_cpu(__disable_counter, NULL, 0);
}


static void __start_counter(void* info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	cinfo->event->pmu->add(cinfo->event, PERF_EF_START);
}

static void start_counters(void)
{
	on_each_cpu(__start_counter, NULL, 0);
}

/**************************************************************************
 * Local Functions
 **************************************************************************/

static ssize_t bwlockmod_control_write(struct file *filp,
				    const char __user *ubuf,
				    size_t cnt, loff_t *ppos)
{
	char buf[BUF_SIZE];
	char *p = buf;

	if (copy_from_user(&buf, ubuf, (cnt > BUF_SIZE) ? BUF_SIZE: cnt) != 0)
		return 0;

	if (!strncmp(p, "reclaim ", 8))
		sscanf(p+8, "%d", &g_use_reclaim);
	else if (!strncmp(p, "exclusive ", 10))
		sscanf(p+10, "%d", &g_use_exclusive);
	else
		pr_info("ERROR: %s\n", p);
	smp_mb();
	return cnt;
}

static int bwlockmod_control_show(struct seq_file *m, void *v)
{
	char buf[64];
	struct bwlockmod_info *global = &bwlockmod_info;

	seq_printf(m, "reclaim: %d\n", g_use_reclaim);
	seq_printf(m, "exclusive: %d\n", g_use_exclusive);
	cpulist_scnprintf(buf, 64, global->active_mask);
	seq_printf(m, "active: %s\n", buf);
	cpulist_scnprintf(buf, 64, global->throttle_mask);
	seq_printf(m, "throttle: %s\n", buf);
	return 0;
}

static int bwlockmod_control_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, bwlockmod_control_show, NULL);
}

static const struct file_operations bwlockmod_control_fops = {
	.open		= bwlockmod_control_open,
	.write          = bwlockmod_control_write,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};


static void __update_budget(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);

	if ((unsigned long)info == 0) {
		pr_info("ERR: Requested budget is zero\n");
		return;
	}
	cinfo->limit = (unsigned long)info;
	smp_mb();
	DEBUG_USER(trace_printk("MSG: New budget of Core%d is %d\n",
				smp_processor_id(), cinfo->budget));

}


#define MAXPERF_EVENTS 163840  /* 10000 MB/s */
#define THROTTLE_EVENTS 1638   /*   100 MB/s */

static ssize_t bwlockmod_limit_write(struct file *filp,
				    const char __user *ubuf,
				    size_t cnt, loff_t *ppos)
{
	char buf[BUF_SIZE];
	char *p = buf;
	int i;
	int use_mb = 0;

	if (copy_from_user(&buf, ubuf, (cnt > BUF_SIZE) ? BUF_SIZE: cnt) != 0) 
		return 0;

	if (!strncmp(p, "mb ", 3)) {
		use_mb = 1;
		p+=3;
	}
	for_each_online_cpu(i) {
		int input;
		unsigned long events;
		sscanf(p, "%d", &input);
		if (input == 0) {
			pr_err("ERR: CPU%d: input is zero: %s.\n",i, p);
			continue;
		}
		if (!use_mb)
			input = sysctl_maxperf_bw_mb*100/input;
		events = (unsigned long)convert_mb_to_events(input);
		pr_info("CPU%d: New budget=%ld (%d %s)\n", i, 
			events, input, (use_mb)?"MB/s": "pct");
		smp_call_function_single(i, __update_budget,
					 (void *)events, 0);
		
		p = strchr(p, ' ');
		if (!p) break;
		p++;
	}

	smp_mb();
	return cnt;
}

static int bwlockmod_limit_show(struct seq_file *m, void *v)
{
	int i, j, cpu;
	struct bwlockmod_info *global = &bwlockmod_info;
	cpu = get_cpu();

	smp_mb();
	seq_printf(m, "cpu  |budget (MB/s)\n");
	seq_printf(m, "-------------------------------\n");

	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		int budget = 0;
		if (cinfo->limit > 0) {
			budget = cinfo->limit;
		}
	
		seq_printf(m, "CPU%d: %d (%dMB/s)\n", i, budget,
			   convert_events_to_mb(budget));
#if USE_BWLOCK_DYNPRIO
		for (j = 0; j < cinfo->dprio_cnt; j++) {
			seq_printf(m, "\t%12s  %3d\n", (cinfo->dprio[j].task)->comm, 
				   cinfo->dprio[j].origprio);
		}
#endif
	}
	seq_printf(m, "bwlocked_core: %d\n", global->bwlocked_cores);

	put_cpu();
	return 0;
}

static int bwlockmod_limit_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, bwlockmod_limit_show, NULL);
}

static const struct file_operations bwlockmod_limit_fops = {
	.open		= bwlockmod_limit_open,
	.write          = bwlockmod_limit_write,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};


/**
 * Display usage statistics
 *
 * TODO: use IPI
 */
static int bwlockmod_usage_show(struct seq_file *m, void *v)
{
	int i, j;

	smp_mb();

	/* current utilization */
	for (j = 0; j < 3; j++) {
		for_each_online_cpu(i) {
			struct core_info *cinfo = per_cpu_ptr(core_info, i);
			u64 budget, used, util;

			budget = cinfo->budget;
			used = cinfo->used[j];
			util = div64_u64(used * 100, (budget) ? budget : 1);
			seq_printf(m, "%llu ", util);
		}
		seq_printf(m, "\n");
	}
	seq_printf(m, "<overall>----\n");

	/* overall utilization
	   WARN: assume budget did not changed */
	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		u64 total_budget, total_used, result;

		total_budget = cinfo->overall.assigned_budget;
		total_used   = cinfo->overall.used_budget;
		result       = div64_u64(total_used * 100, 
					 (total_budget) ? total_budget : 1 );
		seq_printf(m, "%lld ", result);
	}
	return 0;
}

static int bwlockmod_usage_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, bwlockmod_usage_show, NULL);
}

static const struct file_operations bwlockmod_usage_fops = {
	.open		= bwlockmod_usage_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static void __reset_stats(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	DEBUG_USER(trace_printk("CPU%d\n", smp_processor_id()));

	/* update local period information */
	cinfo->period_cnt = 0;
	cinfo->used[0] = cinfo->used[1] = cinfo->used[2] =
		cinfo->budget; /* initial condition */
	cinfo->cur_budget = cinfo->budget;
	cinfo->overall.used_budget = 0;
	cinfo->overall.assigned_budget = 0;
	cinfo->overall.throttled_time_ns = 0;
	cinfo->overall.throttled = 0;
	cinfo->overall.throttled_error = 0;

	memset(cinfo->overall.throttled_error_dist, 0, sizeof(int)*10);
	cinfo->throttled_time = ktime_set(0,0);
	smp_mb();

	DEBUG_USER(trace_printk("MSG: Clear statistics of Core%d\n",
				smp_processor_id()));
}


static ssize_t bwlockmod_failcnt_write(struct file *filp,
				    const char __user *ubuf,
				    size_t cnt, loff_t *ppos)
{
	/* reset local statistics */
	struct bwlockmod_info *global = &bwlockmod_info;

	spin_lock(&global->lock);
	global->budget = global->period_cnt = 0;
	global->start_tick = jiffies;
	spin_unlock(&global->lock);

	smp_mb();
	on_each_cpu(__reset_stats, NULL, 0);
	return cnt;
}

static int bwlockmod_failcnt_show(struct seq_file *m, void *v)
{
	int i;

	smp_mb();
	/* total #of throttled periods */
	seq_printf(m, "throttled: ");
	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		seq_printf(m, "%d ", cinfo->overall.throttled);
	}
	seq_printf(m, "\nthrottle_error: ");
	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		seq_printf(m, "%lld ", cinfo->overall.throttled_error);
	}

	seq_printf(m, "\ncore-pct   10    20    30    40    50    60    70    80    90    100\n");
	seq_printf(m, "--------------------------------------------------------------------");
	for_each_online_cpu(i) {
		int idx;
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		seq_printf(m, "\n%4d    ", i);
		for (idx = 0; idx < 10; idx++)
			seq_printf(m, "%5d ",
				cinfo->overall.throttled_error_dist[idx]);
	}

	/* total #of exclusive mode periods */
	seq_printf(m, "\nexclusive: ");
	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		seq_printf(m, "%d(%lld ms|%lld MB) ", cinfo->overall.exclusive,
			   cinfo->overall.exclusive_ns >> 20, 
			   (cinfo->overall.exclusive_bw * CACHE_LINE_SIZE) >> 20);
	}

	/* out of total periods */
	seq_printf(m, "\ntotal_periods: ");
	for_each_online_cpu(i)
		seq_printf(m, "%ld ", per_cpu_ptr(core_info, i)->period_cnt);
	return 0;
}

static int bwlockmod_failcnt_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, bwlockmod_failcnt_show, NULL);
}

static const struct file_operations bwlockmod_failcnt_fops = {
	.open		= bwlockmod_failcnt_open,
	.write          = bwlockmod_failcnt_write,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int bwlockmod_init_debugfs(void)
{

	bwlockmod_dir = debugfs_create_dir("bwlockmod", NULL);
	BUG_ON(!bwlockmod_dir);
	debugfs_create_file("control", 0444, bwlockmod_dir, NULL,
			    &bwlockmod_control_fops);

	debugfs_create_file("limit", 0444, bwlockmod_dir, NULL,
			    &bwlockmod_limit_fops);

	debugfs_create_file("usage", 0666, bwlockmod_dir, NULL,
			    &bwlockmod_usage_fops);

	debugfs_create_file("failcnt", 0644, bwlockmod_dir, NULL,
			    &bwlockmod_failcnt_fops);
	return 0;
}

static int throttle_thread(void *arg)
{
	int cpunr = (unsigned long)arg;
	struct core_info *cinfo = per_cpu_ptr(core_info, cpunr);

	static const struct sched_param param = {
		.sched_priority = MAX_USER_RT_PRIO/2,
	};

	sched_setscheduler(current, SCHED_FIFO, &param);

	while (!kthread_should_stop() && cpu_online(cpunr)) {

		DEBUG(trace_printk("wait an event\n"));
		wait_event_interruptible(cinfo->throttle_evt,
					 cinfo->throttled_task ||
					 kthread_should_stop());

		DEBUG(trace_printk("got an event\n"));

		if (kthread_should_stop())
			break;

		smp_mb();
		while (cinfo->throttled_task && !kthread_should_stop())
		{
			cpu_relax();
			/* TODO: mwait */
			smp_mb();
		}
	}

	DEBUG(trace_printk("exit\n"));
	return 0;
}

/* Idle notifier to look at idle CPUs */
static int bwlockmod_idle_notifier(struct notifier_block *nb, unsigned long val,
				void *data)
{
	struct bwlockmod_info *global = &bwlockmod_info;
	unsigned long flags;

	DEBUG(trace_printk("idle state update: %ld\n", val));

	spin_lock_irqsave(&global->lock, flags);
	if (val == IDLE_START) {
		cpumask_clear_cpu(smp_processor_id(), global->active_mask);
		DEBUG(if (cpumask_equal(global->throttle_mask, global->active_mask))
			      trace_printk("DBG: last idle\n"););
	} else
		cpumask_set_cpu(smp_processor_id(), global->active_mask);
	spin_unlock_irqrestore(&global->lock, flags);
	return 0;
}

static struct notifier_block bwlockmod_idle_nb = {
	.notifier_call = bwlockmod_idle_notifier,
};

/*
 * TENTATIVE: character device operations for this module
 */

static int device_open(struct inode *inode, struct file *filp)
{
	dev_open++;

	try_module_get(THIS_MODULE);

	DEV_DEBUG(trace_printk("[DBG-01] Module Acquired - Reference : \t%d\n", dev_open));

	return 0;
}

static int device_release(struct inode *inode, struct file *filp)
{
	dev_open--;

	DEV_DEBUG(trace_printk("[DBG-02] Module Released - Reference : \t%d\n", dev_open));

	module_put(THIS_MODULE);

	return 0;
}

static int device_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct core_info *cinfo		= this_cpu_ptr(core_info);
	void *mm_buffer			= this_cpu_ptr(mmap_buffer);
	unsigned long start 		= (unsigned long)vma->vm_start;
	unsigned long size  		= (unsigned long)(vma->vm_end - vma->vm_start);
	unsigned long page, pos;
	int i, j;

	DEV_DEBUG(trace_printk("[DBG-03] Processing mmap\n"));
	
	/* Shared memory is going to be accessed. Acquire the spinlock */
	spin_lock(&cinfo->core_lock);

	if (size > MMT_BUF_SIZE) {
		DEV_DEBUG(trace_printk("[ERR-04] Requested map size exceeds limit - Limit : \t%d\n", (int)MMT_BUF_SIZE));
		
		return -EINVAL;
	}

	pos = (unsigned long)mm_buffer;

	page = ((virt_to_phys((void *)pos)) >> PAGE_SHIFT);

	if (remap_pfn_range(vma, start, page, size, PAGE_SHARED)) {
		DEV_DEBUG(trace_printk("[ERR-05] Failed to remap the pages\n"));

		return -EAGAIN;
	}

	/*
	 * 1. Ascertain the value of index. If it is maxed, return with error
	 * 2. If an earlier transaction with user-space is pending, return with
	      error
	 * 3. Traverse the valid[i] to find the first available free index
	 * 4. Update that index, increment index inside mod_info
	 * 5. Record this index for return to user
	 * 6. Mark the trans_pending field in the bwlockmod_info structure
	 */
	 
	/* Step-1 & 2 */
	if (cinfo->index == TBL_SIZE || cinfo->trans_pending == 1) {
		DEV_DEBUG(trace_printk("[ERR-XX] Table is fully occupied[%d] or a transaction is pending[%d]\n", cinfo->index == TBL_SIZE, cinfo->trans_pending));
		
		return -EAGAIN;
	}

	/* Step-3 & 4 & 5*/
	for (i = 0; i < (TBL_SIZE/32); i++) {
		j = 0;
		if (cinfo->valid[i] != 0xFFFFFFFF) {
			while (field32_get_mask(cinfo->valid[i], j)) {
				j++;
			}
			cinfo->valid[i] |= field32_set_bit(j);
			cinfo->free_index = field32_get_index(j, i);
			cinfo->index++;
			break;
		}
	}

	/* Step-6 */
	cinfo->trans_pending = 1;

	/* Release the spinlock */
	spin_unlock(&cinfo->core_lock);

	/* Mark the successful completion of mmap request */
	DEV_DEBUG(trace_printk("[DBG-04] mmap successfully processed\n"));

	return 0;
}

long device_ioctl (struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct core_info *cinfo 	= this_cpu_ptr(core_info);
	long ioctl_ret = 0;

	/* Acquire the spinlock before manipulating shared memory */
	spin_lock(&cinfo->core_lock);

	switch (cmd)
	{
		case IOCTL_GET_INDEX:
			/* It is expected that the user-space processes will
			   behave nicely when invoking the ioctl */

			if (cinfo->trans_pending == 1) {
				cinfo->trans_pending = 0;
				ioctl_ret = cinfo->free_index;
			} else {
				/* This is an error since there are no pending 
				   transactions with user-space */
				ioctl_ret = NO_PENDING_TRANSACTIONS;
			}

			break;
		
		default:
			ioctl_ret = PARAM_NOT_RECOGNIZED;
			break;
	}

	/* Release the spinlock */
	spin_unlock(&cinfo->core_lock);

	return ioctl_ret;
}
	
static struct file_operations user_control_fops = {
	.mmap		= device_mmap,
	.open 		= device_open,
	.release	= device_release,
	.unlocked_ioctl = device_ioctl
};

int init_module( void )
{
	int i;

	struct bwlockmod_info *global = &bwlockmod_info;

	major = alloc_chrdev_region(&first, 0, 1, DEVICE_NAME);

	if (major < 0) {
		DEV_DEBUG(trace_printk("[ERR-06] Registering user device file failed\n"));

		return major;
	}

	c1 = class_create(THIS_MODULE, DEVICE_NAME);

	if (c1 == NULL) {
		DEV_DEBUG(trace_printk("[ERR-07] Registering user device class failed\n"));
		unregister_chrdev_region(first, 1);

		return -EAGAIN;
	}

	if (device_create(c1, NULL, first, NULL, DEVICE_NAME) == NULL) {
		DEV_DEBUG(trace_printk("[ERR-08] Creation of user device failed\n"));
		class_destroy(c1);
		unregister_chrdev_region(first, 1);

		return -EAGAIN;
	}

	cdev_init(&c_dev, &user_control_fops);

	if (cdev_add(&c_dev, first, 1) == -1) {
		DEV_DEBUG(trace_printk("[ERR-09] Failed to add user device\n"));
		device_destroy(c1, first);
		class_destroy(c1);
		unregister_chrdev_region(first, 1);
	
		return -EAGAIN;
	}

	DEV_DEBUG(trace_printk("[DBG-04] mmap stage in module init went fine!\n"));

	/* initialized bwlockmod_info structure */
	memset(global, 0, sizeof(struct bwlockmod_info));
	zalloc_cpumask_var(&global->throttle_mask, GFP_NOWAIT);
	zalloc_cpumask_var(&global->active_mask, GFP_NOWAIT);

	if (g_period_us < 0 || g_period_us > 1000000) {
		printk(KERN_INFO "Must be 0 < period < 1 sec\n");
		return -ENODEV;
	}

	if (g_test) {
		self_test();
		return -ENODEV;
	}

	spin_lock_init(&global->lock);
	global->start_tick = jiffies;
	global->period_in_ktime = ktime_set(0, g_period_us * 1000);

	/* initialize all online cpus to be active */
	cpumask_copy(global->active_mask, cpu_online_mask);

	pr_info("ARCH: %s\n", g_hw_type);
	pr_info("HZ=%d, g_period_us=%d\n", HZ, g_period_us);

	pr_info("Initilizing perf counter\n");
	core_info = alloc_percpu(struct core_info);
	smp_mb();

	get_online_cpus();
	for_each_online_cpu(i) {
		struct perf_event *event;
		struct core_info *cinfo = per_cpu_ptr(core_info, i);

		int budget;
		/* initialize counter h/w & event structure */
		budget = convert_mb_to_events(sysctl_maxperf_bw_mb);
		pr_info("budget[%d] = %d (%d MB/s)\n", i, budget, sysctl_maxperf_bw_mb);

		/* create performance counter */
		event = init_counter(i, budget);
		if (!event)
			break;

		/* initialize per-core data structure */
		smp_call_function_single(i, __init_per_core, (void *)event, 1);
		smp_mb();


		/* create and wake-up throttle threads */
		cinfo->throttle_thread =
			kthread_create_on_node(throttle_thread,
					       (void *)((unsigned long)i),
					       cpu_to_node(i),
					       "kthrottle/%d", i);

		BUG_ON(IS_ERR(cinfo->throttle_thread));
		kthread_bind(cinfo->throttle_thread, i);
		wake_up_process(cinfo->throttle_thread);
	}

	register_hotcpu_notifier(&bwlockmod_cpu_notifier);

	bwlockmod_init_debugfs();

	pr_info("Start event counters\n");
	start_counters();
	smp_mb();

	pr_info("Start period timer (period=%lld us)\n",
		div64_u64(global->period_in_ktime.tv64, 1000));

	get_cpu();
	global->master = smp_processor_id();

	hrtimer_init(&global->hr_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED );
	global->hr_timer.function = &period_timer_callback_master;
	hrtimer_start(&global->hr_timer, global->period_in_ktime, 
		      HRTIMER_MODE_REL_PINNED);
	put_cpu();

	idle_notifier_register(&bwlockmod_idle_nb);
	return 0;
}

void cleanup_module( void )
{
	int i;

	struct bwlockmod_info *global = &bwlockmod_info;

	smp_mb();

	get_online_cpus();
	
	cdev_del(&c_dev);
	device_destroy(c1, first);
	class_destroy(c1);
	unregister_chrdev_region(first, 1);

	on_each_cpu(__kill_throttlethread, NULL, 1);

	/* unregister sched-tick callback */
	pr_info("Cancel timer\n");
	hrtimer_cancel(&global->hr_timer);

	/* stop perf_event counters */
	disable_counters();

	/* destroy perf objects */
	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		void *mm_buffer = per_cpu_ptr(mmap_buffer, i);
		free_page((unsigned long)mm_buffer);
		pr_info("Stopping kthrottle/%d\n", i);
		cinfo->throttled_task = NULL;
		kthread_stop(cinfo->throttle_thread);
		perf_event_release_kernel(cinfo->event);
	}

	/* unregister callbacks */
	idle_notifier_unregister(&bwlockmod_idle_nb);
	unregister_hotcpu_notifier(&bwlockmod_cpu_notifier);

	/* remove debugfs entries */
	debugfs_remove_recursive(bwlockmod_dir);

	/* free allocated data structure */
	free_cpumask_var(global->throttle_mask);
	free_cpumask_var(global->active_mask);
	free_percpu(core_info);

	pr_info("module uninstalled successfully\n");
	return;
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Heechul Yun <heechul@illinois.edu>");


static void test_ipi_cb(void *info)
{
	trace_printk("IPI called on %d\n", smp_processor_id());
}

#if 0
static void test_ipi_master(void)
{
	ktime_t start, duration;
	int i;

	/* Measuring IPI overhead */
	trace_printk("self-test begin\n");
	start = ktime_get();
	for (i = 0; i < g_test; i++) {
		on_each_cpu(test_ipi_cb, 0, 0);
	}
	duration = ktime_sub(ktime_get(), start);
	trace_printk("self-test end\n");
	pr_info("#iterations: %d | duration: %lld us | average: %lld ns\n",
		g_test, duration.tv64/1000, duration.tv64/g_test);
}
#endif

enum hrtimer_restart test_timer_cb(struct hrtimer *timer)
{
	ktime_t now;
	now = timer->base->get_time();
	hrtimer_forward(timer, now, ktime_set(0, 1000 * 1000));
	trace_printk("master begin\n");
	on_each_cpu(test_ipi_cb, 0, 0);
	trace_printk("master end\n");
	g_test--;
	smp_mb();
	if (g_test == 0) 
		return HRTIMER_NORESTART;
	else
		return HRTIMER_RESTART;
}

static int self_test(void)
{
	struct hrtimer __test_hr_timer;

	hrtimer_init(&__test_hr_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED );
	__test_hr_timer.function = &test_timer_cb;
	hrtimer_start(&__test_hr_timer, ktime_set(0, 1000 * 1000), /* 1ms */
		      HRTIMER_MODE_REL_PINNED);

	while (g_test) {
		smp_mb();
		cpu_relax();
	}

	return 0;
}

