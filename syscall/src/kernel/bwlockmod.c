/**
 * FILE		: bwlockmod.c
 * BRIEF	: Memory bandwidth controller for multi-core systems
 *
 * Copyright (C) 2014  Heechul Yun <heechul.yun@ku.edu>
 *
 * SECONDARY	: If the modified version of this module works, then it
 * AUTHOR	  was modified by Waqar Ali (wali@ku.edu). If it doesn't
 *		  work, then I don't know who modified it... :(
 *
 * This file is distributed under the University of Illinois Open Source
 * License. See LICENSE.TXT for details.
 *
 */

/**************************************************************************
 * Special Conditional Compilation Information
 *************************************************************************/
#define pr_fmt(fmt) 		KBUILD_MODNAME ": " fmt

/**************************************************************************
 * Included Files
 *************************************************************************/

/* Include the function prototypes for this module's local functions */
#include "bwlockmod.h"

/**************************************************************************
 * Global Variables
 **************************************************************************/
static struct bwlockmod_info 	bwlockmod_info;
static struct core_info __percpu *core_info;
static struct dentry 		*bwlockmod_dir;

/* Should be defined in the scheduler code */
static int 			sysctl_maxperf_bw_mb 	= 10000;
static int 			sysctl_throttle_bw_mb	= 100;

static char			*g_hw_type 		= "";
static int 			g_period_us 		= 1000;
static int 			g_use_reclaim 		= 0;	/* minimum remaining time to reclaim */
static int 			g_use_bwlock 		= 1;
static int 			g_use_exclusive 	= 0;
static int 			g_budget_min_value 	= 1000;
static int 			g_test 			= 0;

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

/**************************************************************************
 * Module parameters
 **************************************************************************/

module_param(g_test, 		int,	S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
module_param(g_hw_type,		charp,	S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
module_param(g_use_reclaim,	int,	S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
module_param(g_use_bwlock,	int, 	S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
module_param(g_period_us,	int, 	S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);

MODULE_PARM_DESC(g_test,	"number of test iterations");
MODULE_PARM_DESC(g_hw_type,	"hardware type");
MODULE_PARM_DESC(g_use_reclaim,	"enable/disable reclaim");
MODULE_PARM_DESC(g_use_bwlock,	"enable/disable bandwidth lock");
MODULE_PARM_DESC(g_period_us,	"throttling period in usec");

/**************************************************************************
 * Local Function Definitions
 **************************************************************************/

/* Populate the notifier callback structure with the requisite function */
static struct notifier_block bwlockmod_cpu_notifier =
{
	.notifier_call = bwlockmod_cpu_callback,
};

/* Populate the idle notifier callback in the relevant structure */
static struct notifier_block bwlockmod_idle_nb = {
	.notifier_call = bwlockmod_idle_notifier,
};

/* Update the file-information data structure */
static const struct file_operations bwlockmod_control_fops = {
	.open		= bwlockmod_control_open,
	.write          = bwlockmod_control_write,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/* Populate the file information data structure */
static const struct file_operations bwlockmod_limit_fops = {
	.open		= bwlockmod_limit_open,
	.write          = bwlockmod_limit_write,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static const struct file_operations bwlockmod_usage_fops = {
	.open		= bwlockmod_usage_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/* Populate the failcnt file-structure with the create interface function */
static const struct file_operations bwlockmod_failcnt_fops = {
	.open		= bwlockmod_failcnt_open,
	.write          = bwlockmod_failcnt_write,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/*
 * This function initializes the bandwidth lock kernel module
 */
int init_module ( void )
{
	struct bwlockmod_info *global = &bwlockmod_info;
	int i = 0;

	/* Reset all the bits in the bwlock-module info data-structure */
	memset(global, 0, sizeof (struct bwlockmod_info));
	zalloc_cpumask_var (&global->throttle_mask, GFP_NOWAIT);
	zalloc_cpumask_var (&global->active_mask, GFP_NOWAIT);

	/* Verify that the period parameter is correct */
	if (g_period_us < 0 || g_period_us > 1000000) {
		printk (KERN_INFO "Must be 0 < period < 1 sec\n");

		/* None can do! */
		return -ENODEV;
	}

	/* Test yourself if required */
	if (g_test) {
		/* This is gonna be tough... */
		self_test ();

		/* I let myself down... :( */
		return -ENODEV;
	}

	/* Initialize the global spin-lock. This lock will be used throughout this kernel
	   module to serialize access to critical code regions */
	spin_lock_init(&global->lock);

	/* Because jiffy-lube is the best */
	global->start_tick = jiffies;
	global->period_in_ktime = ktime_set (0, g_period_us * 1000);

	/* initialize all online cpus to be active */
	cpumask_copy (global->active_mask, cpu_online_mask);

	/* Print some basic hardware information to debug trace buffer */
	pr_info ("ARCH: %s\n", g_hw_type);
	pr_info ("HZ=%d, g_period_us=%d\n", HZ, g_period_us);

	pr_info ("Initilizing perf counter\n");
	core_info = alloc_percpu (struct core_info);

	/* Place a load / store memory barrier to synchronize across smp cores */
	smp_mb ();

	/* Referesh the number of online cpu information */
	get_online_cpus ();

	/* Iterate over each online cpu */
	for_each_online_cpu (i) {
		int budget;
		struct perf_event *event;
		struct core_info *cinfo = per_cpu_ptr (core_info, i);

		/* Initialize counter h/w & event structure */
		budget = convert_mb_to_events (sysctl_maxperf_bw_mb);
		pr_info ("budget[%d] = %d (%d MB/s)\n", i, budget, sysctl_maxperf_bw_mb);

		/* Create performance counter */
		event = init_counter (i, budget);

		/* Check if the event was success */
		if (!event)
			break;

		/* Initialize per-core data structure */
		smp_call_function_single (i, __init_per_core, (void *)event, 1);
		smp_mb ();

		/* Create and wake-up throttle threads */
		cinfo->throttle_thread = kthread_create_on_node (throttle_thread,
					       		 	 (void *)((unsigned long)i),
					       		 	 cpu_to_node (i),
					       		 	 "kthrottle/%d", i);

		BUG_ON (IS_ERR (cinfo->throttle_thread));
		kthread_bind (cinfo->throttle_thread, i);
		wake_up_process (cinfo->throttle_thread);
	}

	/* Keep an eye out for hot cpus */
	register_hotcpu_notifier (&bwlockmod_cpu_notifier);

	/* Initialize debug file-system */
	bwlockmod_init_debugfs ();

	/* Kick-off the PM-counters */
	pr_info ("Start event counters\n");
	start_counters ();
	smp_mb ();

	pr_info ("Start period timer (period=%lld us)\n",
		 div64_u64 (global->period_in_ktime.tv64, 1000));

	get_cpu ();
	global->master = smp_processor_id ();

	/* Initialize the high resolution timer */
	hrtimer_init (&global->hr_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED);
	global->hr_timer.function = &period_timer_callback_master;

	/* Start the high resolution timer */
	hrtimer_start (&global->hr_timer, global->period_in_ktime, HRTIMER_MODE_REL_PINNED);
	put_cpu();

	/* Register idle notifier */
	idle_notifier_register (&bwlockmod_idle_nb);

	/* All is end that ends well */
	return 0;
}

/*
 * This function cleans up after the bandwidth lock kernel module
 */
void cleanup_module ( void )
{
	struct bwlockmod_info *global = &bwlockmod_info;
	int i;

	/* Place a load / store memory barrier across smp cores */
	smp_mb ();

	/* Refresh the online cpu information */
	get_online_cpus ();

	/* Kill the throttle-thread on each online cpu */
	on_each_cpu (__kill_throttlethread, NULL, 1);

	/* Unregister sched-tick callback */
	pr_info ("Cancel timer\n");
	hrtimer_cancel (&global->hr_timer);

	/* Stop perf_event counters */
	disable_counters ();

	/* Destroy perf objects */
	for_each_online_cpu (i) {
		struct core_info *cinfo = per_cpu_ptr (core_info, i);
		pr_info ("Stopping kthrottle/%d\n", i);
		cinfo->throttled_task = NULL;
		kthread_stop (cinfo->throttle_thread);
		perf_event_release_kernel (cinfo->event);
	}

	/* Unregister callbacks */
	idle_notifier_unregister (&bwlockmod_idle_nb);
	unregister_hotcpu_notifier (&bwlockmod_cpu_notifier);

	/* Remove debugfs entries */
	debugfs_remove_recursive (bwlockmod_dir);

	/* Free allocated data structure */
	free_cpumask_var (global->throttle_mask);
	free_cpumask_var (global->active_mask);
	free_percpu (core_info);

	pr_info ("Bandwidth lock has kicked the bucket!\n");

	/* Witness Me! */
	return;
}

/*
 * This function handles the overflow event.
 *
 * TODO: Identify everything this function does
 */
static void bwlockmod_process_overflow ( struct irq_work *entry )
{
	struct core_info *cinfo = this_cpu_ptr (core_info);
	struct bwlockmod_info *global = &bwlockmod_info;
	ktime_t start = ktime_get ();
	s64 budget_used;
	int amount = 0;
	int i;

	BUG_ON (in_nmi () || !in_irq ());

	/* Obtain global spin-lock to serialize access to critical section */
	spin_lock (&global->lock);

	if (!cpumask_test_cpu (smp_processor_id (), global->active_mask)) {
		/* Release the spin-lock */
		spin_unlock (&global->lock);
		trace_printk ("ERR: not active\n");

		/* Return to the caller since the current core is not active */
		return;
	} else if (global->period_cnt != cinfo->period_cnt) {
		trace_printk ("ERR: global(%ld) != local(%ld) period mismatch\n",
			      global->period_cnt,
			      cinfo->period_cnt);

		/* Release the spin-lock */
		spin_unlock (&global->lock);

		/* Return since we are in an error condition */
		return;
	}

	/* Release the spin-lock */
	spin_unlock (&global->lock);

	/* Calculate the budget used so far */
	budget_used = bwlockmod_event_used (cinfo);

	/* Erroneous overflow, that could have happend before period timer
	   stopped the PMU */
	if (budget_used < cinfo->cur_budget) {
		trace_printk ("ERR: overflow in timer. used %lld < cur_budget %d. ignore\n",
			       budget_used, 
			       cinfo->cur_budget);

		/* Return since we in an error condition */
		return;
	}

	/* Try to reclaim budget from the global pool */
	amount = request_budget(cinfo);

	/* Verify that the amount obtained is correct */
	if (amount > 0) {
		cinfo->cur_budget += amount;
		local64_set (&cinfo->event->hw.period_left, amount);
		DEBUG_RECLAIM (trace_printk("successfully reclaimed %d\n", amount));

		/* All done here */
		return;
	}

	/* No more overflow interrupt */
	local64_set (&cinfo->event->hw.period_left, 0xfffffff);

	/* Check if we donated too much */
	if (budget_used < cinfo->budget) {
		trace_printk ("ERR: throttling error\n");
		cinfo->prev_throttle_error = 1;
	}

	/* We are going to be throttled */
	spin_lock (&global->lock);

	/* Add our cpu to the global throttle mask */
	cpumask_set_cpu (smp_processor_id (), global->throttle_mask);

	if (cpumask_equal (global->throttle_mask, global->active_mask)) {
		/* all other cores are alreay throttled */
		spin_unlock (&global->lock);

		if (g_use_exclusive == 1) {
			/* Algorithm 1: last one get all remaining time */
			cinfo->exclusive_mode = 1;
			cinfo->exclusive_time = ktime_get ();
			DEBUG_RECLAIM (trace_printk ("exclusive mode begin\n"));

			/* All done here */
			return;
		} else if (g_use_exclusive == 2) {
			/* Algorithm 2: wakeup all (i.e., non regulation) */
			smp_call_function (__unthrottle_core, NULL, 0);
			cinfo->exclusive_mode = 1;
			cinfo->exclusive_time = ktime_get ();
			DEBUG_RECLAIM (trace_printk ("exclusive mode begin\n"));

			/* Nothing more to do */
			return;
		} else if (g_use_exclusive == 5) {
			smp_call_function_single (global->master,
						  __newperiod, 
					  	 (void *)cinfo->period_cnt,
						 0);

			/* All done here */
			return;
		} else if (g_use_exclusive > 5) {
			trace_printk ("ERR: Unsupported exclusive mode %d\n", 
				       g_use_exclusive);

			/* Return to the caller */
			return;
		} else if (g_use_exclusive != 0 && cpumask_weight (global->active_mask) == 1) {
			trace_printk ("ERR: don't throttle one active core\n");

			/* Return wihtout doing anythin else */
			return;
		}
	} else {
		/* Release the spin-lock */
		spin_unlock (&global->lock);
	}

	if (cinfo->prev_throttle_error)
		return;

	/*
	 * Fail to reclaim. Now throttle this core
	 */
	DEBUG_RECLAIM (trace_printk ("Failed to reclaim after %lld nsec.\n",
				      ktime_get ().tv64 - start.tv64));

	/* Wake-up throttle task */
	cinfo->throttled_task = current;
	cinfo->throttled_time = start;

#ifdef USE_BWLOCK_DYNPRIO
	/* Dynamically lower the throttled task's priority
	   TBD: register the 'current' task */
	for (i = 0; i < cinfo->dprio_cnt; i++) {
		if (cinfo->dprio[i].task == current)
			break;
	}

	if (i == cinfo->dprio_cnt && i < MAX_DYNPRIO_TSKS) {
		cinfo->dprio[i].task = current;
		cinfo->dprio[i].start = start;
		cinfo->dprio[i].origprio = task_nice (current);
		cinfo->dprio_cnt++;
	}

	intr_set_user_nice (current, 19);
#endif

	WARN_ON_ONCE (!strncmp (current->comm, "swapper", 7));
	smp_mb ();
	wake_up_interruptible (&cinfo->throttle_evt);

	/* Finally done here */
	return;
}


/*
 * This function is triggered in reponse to the HR-timer expiration on the master
 * core. It calls the slave-callback functions on all other system cores
 */
enum hrtimer_restart period_timer_callback_master ( struct hrtimer *timer )
{
	struct bwlockmod_info *global = &bwlockmod_info;
	cpumask_var_t active_mask;
	long new_period;
	ktime_t now;
	int orun;

	/* Get the time at this instant */
	now = timer->base->get_time ();

	/* Print debug messages to trace buffer */
        DEBUG (trace_printk ("master begin\n"));
	BUG_ON (smp_processor_id () != global->master);

	orun = hrtimer_forward (timer, now, global->period_in_ktime);

	/* Check the stop condition */
	if (orun == 0)
		return HRTIMER_RESTART;

	/* Acquire the global spin-lock to serialize access */
	spin_lock(&global->lock);

	global->period_cnt += orun;
	global->budget = 0;
	new_period = global->period_cnt;
	cpumask_copy (active_mask, global->active_mask);

	/* Release the spin-lock */
	spin_unlock (&global->lock);

	DEBUG (trace_printk ("Spinlock end\n"));

	/* Check for error conditon */
	if (orun > 1)
		trace_printk ("ERR: timer overrun %d at period %ld\n", orun, new_period);

	/* Obtain the number of bandwidth locked cores */
	global->bwlocked_cores = mg_nr_bwlocked_cores ();

	/* Invoke the slave call-back on each core */
	bwlockmod_on_each_cpu_mask (active_mask, period_timer_callback_slave, (void *)new_period, 0);

	/* Print debug message to trace buffer */
	DEBUG(trace_printk("Master end\n"));

	/* Return to the caller */
	return HRTIMER_RESTART;
} 

/*
 * This is the per-core handler function for periodic timer interrupt. The
 * main purpose of this function is to unblock a core if it was previously
 * throttled and replenish the budget of a core if required
 *
 * TODO: Create simplified pseduo-code implementaion of this function
 */
static void period_timer_callback_slave ( void *info )
{
	struct core_info *cinfo = this_cpu_ptr (core_info);
	struct bwlockmod_info *global = &bwlockmod_info;
	int cpu = smp_processor_id ();
	long new_period = (long)info;
	struct task_struct *target;
	int i;

	/* Must be irq disabled. hard irq */
	BUG_ON(!irqs_disabled());
	WARN_ON_ONCE(!in_irq());

	if (new_period <= cinfo->period_cnt) {
		trace_printk ("ERR: new_period(%ld) <= cinfo->period_cnt(%ld)\n",
			       new_period,
			       cinfo->period_cnt);

		/* Return since we have encountered an error */
		return;
	}

	/* Assign local period */
	cinfo->period_cnt = new_period;

	/* Stop counter */
	cinfo->event->pmu->stop (cinfo->event, PERF_EF_UPDATE);

	/* I'm actively participating */
	spin_lock (&global->lock);

	cpumask_clear_cpu (cpu, global->throttle_mask);
	cpumask_set_cpu (cpu, global->active_mask);

	/* Release the global spin-lock */
	spin_unlock (&global->lock);

	/* Unthrottle tasks (if any) */
	if (cinfo->throttled_task)
		target = (struct task_struct *)cinfo->throttled_task;
	else
		target = current;

	cinfo->throttled_task = NULL;

	/* Check if bandwidth lock is allowed */
	if (g_use_bwlock) {
		if (global->bwlocked_cores > 0) {
			if (target->bwlock_val > 0)
				cinfo->limit = convert_mb_to_events (sysctl_maxperf_bw_mb);
			else
				cinfo->limit = convert_mb_to_events (sysctl_throttle_bw_mb);
		} else {
			cinfo->limit = convert_mb_to_events (sysctl_maxperf_bw_mb);

#if USE_BWLOCK_DYNPRIO
			/* TBD: If there were deprioritized tasks, restore their priorities */
			for (i = 0; i < cinfo->dprio_cnt; i++) {
				int oprio = cinfo->dprio[i].origprio;
				intr_set_user_nice (cinfo->dprio[i].task, oprio);
			}

			cinfo->dprio_cnt = 0;
#endif
		}
	}

	DEBUG_BWLOCK (trace_printk ("%s|bwlock_val %d|g->bwlocked_cores %d\n", 
				   (current)? current->comm : "null", 
				    current->bwlock_val,
				    global->bwlocked_cores));

	DEBUG (trace_printk ("%p|New period %ld. global->budget=%d\n",
			      cinfo->throttled_task,
			      cinfo->period_cnt, 
			      global->budget));
	
	/* Update statistics */
	update_statistics (cinfo);

	/* New budget assignment from user */
	spin_lock (&global->lock);

	if (cinfo->limit > 0) {
		/* Limit mode */
		cinfo->budget = cinfo->limit;
	} else {
		WARN_ON_ONCE (1);
		trace_printk ("ERR: both limit and weight = 0");
	}

	/* Unlock the global spin-lock */
	spin_unlock (&global->lock);

	/* Budget can't be zero? */
	cinfo->budget = max (cinfo->budget, 1);

	if (cinfo->event->hw.sample_period != cinfo->budget) {
		/* New budget is assigned */
		DEBUG (trace_printk ("MSG: new budget %d is assigned\n", cinfo->budget));
		cinfo->event->hw.sample_period = cinfo->budget;
	}

	/* Per-task donation policy */
	if (!g_use_reclaim || rt_task(target)) {
		cinfo->cur_budget = cinfo->budget;
		DEBUG (trace_printk ("HRT or !g_use_reclaim: don't donate\n"));
	} else if (target->policy == SCHED_BATCH || target->policy == SCHED_IDLE) {
		/* Non rt task: donate all */
		donate_budget (cinfo->period_cnt, cinfo->budget);
		cinfo->cur_budget = 0;
		DEBUG (trace_printk ("NonRT: donate all %d\n", cinfo->budget));
	} else if (target->policy == SCHED_NORMAL) {
		BUG_ON (rt_task (target));
		if (cinfo->used[PREDICTOR] < cinfo->budget) {
			/* donate 'expected surplus' ahead of time. */
			int surplus = max (cinfo->budget - cinfo->used[PREDICTOR], 0);
			donate_budget (cinfo->period_cnt, surplus);
			cinfo->cur_budget = cinfo->budget - surplus;
			DEBUG (trace_printk ("SRT: surplus: %d, budget: %d\n", surplus, 
					      cinfo->budget));
		} else {
			cinfo->cur_budget = cinfo->budget;
			DEBUG (trace_printk ("SRT: don't donate\n"));
		}
	}

	/* setup an interrupt */
	cinfo->cur_budget = max (1, cinfo->cur_budget);
	local64_set (&cinfo->event->hw.period_left, cinfo->cur_budget);

	/* enable performance counter */
	cinfo->event->pmu->start (cinfo->event, PERF_EF_RELOAD);

	/* All done here */
	return;
}

/*
 * This function invokes the function 'func', passing it the parameter 'info' as its input
 * on each core which is specified by the cpu-mask 'mask'.
 * Apparently, this function should be called with IRQs disabled
 *
 * TODO: What is the purpose of the 'wait' parameter?
 */
static void bwlockmod_on_each_cpu_mask ( const struct cpumask *mask,
					 smp_call_func_t func,
					 void *info,
					 bool wait )
{
	int cpu = smp_processor_id();

	/* Invoke the desired function on all system cores */
	smp_call_function_many(mask, func, info, wait);

	/* Invoke the desired function on the current core as well if required */
	if (cpumask_test_cpu(cpu, mask)) {
		func(info);
	}

	/* All done here */
	return;
}

/*
 * This is the IRQ handler associated with PMC overflow interrupt. This function invokes
 * the current event handling function on the core on which it executes
 */
static void event_overflow_callback ( struct perf_event *event,
				      struct perf_sample_data *data,
				      struct pt_regs *regs )
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	BUG_ON(!cinfo);

	/* Mark work to be done against this irq as pending */
	irq_work_queue(&cinfo->pending);

	/* All done here */
	return;
}
/*
 * This is the per-core initializer function which setups the data-structures
 * and system resources reqired by bandwidth lock kernel module
 */
static void __init_per_core ( void *info )
{
	struct core_info *cinfo = this_cpu_ptr (core_info);
	memset (cinfo, 0, sizeof (struct core_info));

	/* Prevent memory loads from being re-ordered across the smp cores */
	smp_rmb ();

	/* initialize per_event structure */
	cinfo->event = (struct perf_event *)info;

	/* initialize budget */
	cinfo->budget = cinfo->limit = cinfo->event->hw.sample_period;

	/* create idle threads */
	cinfo->throttled_task = NULL;

	init_waitqueue_head (&cinfo->throttle_evt);

	/* initialize statistics */
	__reset_stats (cinfo);

	print_core_info (smp_processor_id (), cinfo);

	/* Prevent memory stores from being re-ordered across the smp cores */
	smp_wmb ();

	/* initialize nmi irq_work_queue */
	init_irq_work (&cinfo->pending, bwlockmod_process_overflow);

	/* Oh long johnson... */
	return;
}

/*
 * This function initializes the PMC events to be used by bandwidth lock kernel
 * module
 */
struct perf_event *init_counter ( int cpu,
				  int budget )
{
	struct perf_event *event = NULL;

	/* Populate the PMC event to be counted */
	struct perf_event_attr sched_perf_hw_attr = {
		.type           = PERF_TYPE_RAW,
		.config         = 0x52,
		.size		= sizeof (struct perf_event_attr),
		.pinned		= 1,
		.disabled	= 1,
		.exclude_kernel = 1,
	};

	if (!strcmp (g_hw_type, "core2")) {
		sched_perf_hw_attr.type           = PERF_TYPE_RAW;
		sched_perf_hw_attr.config         = 0x7024; /* 7024 - incl. prefetch 
							       5024 - only prefetch
							       4024 - excl. prefetch */
	} else if (!strcmp (g_hw_type, "snb")) {
		sched_perf_hw_attr.type           = PERF_TYPE_RAW;
		sched_perf_hw_attr.config         = 0x08b0; /* 08b0 - incl. prefetch */
	} else if (!strcmp (g_hw_type, "soft")) {
		sched_perf_hw_attr.type           = PERF_TYPE_SOFTWARE;
		sched_perf_hw_attr.config         = PERF_COUNT_SW_CPU_CLOCK;
	}

	/* Select based on requested event type */
	sched_perf_hw_attr.sample_period = budget;

	/* Register using hardware perf events */
	event = perf_event_create_kernel_counter (&sched_perf_hw_attr,
						  cpu, 
						  NULL,
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 2, 0)
						  event_overflow_callback,
						  NULL
#else
						  event_overflow_callback
#endif
						  );

	/* Check for valid event registery */
	if (!event)
		return NULL;

	if (IS_ERR (event)) {
		/* Vary the KERN level based on the returned errno */
		if (PTR_ERR (event) == -EOPNOTSUPP)
			pr_info ("cpu%d. not supported\n", cpu);
		else if (PTR_ERR (event) == -ENOENT)
			pr_info ("cpu%d. not h/w event\n", cpu);
		else
			pr_err ("cpu%d. unable to create perf event: %ld\n", cpu, PTR_ERR (event));

		/* Return to the caller since we are in error state */
		return NULL;
	}

	/* Event registration successful */
	pr_info ("cpu%d enabled counter\n", cpu);

	/* Place a memory barrier to synchronize across smp cores */
	smp_wmb ();

	return event;
}

/*
 * This function disables the PM-event counter on a particular core
 */
static void __disable_counter ( void *info )
{
	struct core_info *cinfo = this_cpu_ptr (core_info);
	BUG_ON (!cinfo->event);

	/* Stop the counter */
	cinfo->event->pmu->stop (cinfo->event, PERF_EF_UPDATE);
	cinfo->event->pmu->del (cinfo->event, 0);

	/* Print the debug message to trace buffer */
	pr_info ("LLC bandwidth throttling disabled\n");

	/* Return to caller */
	return;
}

/*
 * This function disables PM-event counters on all cores
 */
static void disable_counters ( void )
{
	/* Invoke the disable counter function on each core */
	on_each_cpu (__disable_counter, NULL, 0);

	/* All done here */
	return;
}

/*
 * This function starts the PM-event counter on a particular core
 */
static void __start_counter ( void *info )
{
	struct core_info *cinfo = this_cpu_ptr (core_info);

	/* Kick off the PMC event counting */
	cinfo->event->pmu->add(cinfo->event, PERF_EF_START);

	/* Return to the caller */
	return;
}

/*
 * This function starts PM-event counters on all cores
 */
static void start_counters ( void )
{
	/* Invoke the start counter function on each core */
	on_each_cpu(__start_counter, NULL, 0);

	/* All done here */
	return;
}

/*
 * This function increments the global budget
 *
 * TODO: The working of this function is not quite clear.
 */
static int donate_budget ( long cur_period,
			   int budget )
{
	struct bwlockmod_info *global = &bwlockmod_info;

	/* Acuire the spin-lock for critical section */
	spin_lock(&global->lock);

	if (global->period_cnt == cur_period) {
		global->budget += budget;
	}

	/* Release the spin-lock after the critical section */
	spin_unlock(&global->lock);

	/* Return the new-budget value to the caller */
	return global->budget;
}

/*
 * This function reduces the global budget
 *
 * TODO: The working of this function is not quite clear.
 */
static int reclaim_budget ( long cur_period,
			    int budget )
{	
	struct bwlockmod_info *global = &bwlockmod_info;
	int reclaimed = 0;

	/* Acquire spin-lock for critical section */
	spin_lock(&global->lock);

	if (global->period_cnt == cur_period) {
		reclaimed = min(budget, global->budget);
		global->budget -= reclaimed;
	}

	/* Release spin-lock after the critical section */
	spin_unlock(&global->lock);

	/* Return the reclaimed budget to caller */
	return reclaimed;
}

/*
 * This function reclaims budget from the global budget
 *
 * TODO: The working of this function is not quite clear.
 */
static int request_budget ( struct core_info *cinfo )
{
	int budget_used = bwlockmod_event_used (cinfo);
	int amount = 0;

	BUG_ON (!cinfo);

	if (budget_used < cinfo->budget && current->policy == SCHED_NORMAL) {
		/* Didn't used up my original budget */
		amount = cinfo->budget - budget_used;
	} else {
		/* I'm requesting more than what I was originally assigned */
		amount = g_budget_min_value;
	}

	if (amount > 0) {
		/* Successfully reclaimed my budget */
		amount = reclaim_budget (cinfo->period_cnt, amount);
	}

	/* Return the amount of budget actually obtained */
	return amount;
}

/*
 * This function converts memory bandwidth into LLC-events
 */
static inline u64 convert_mb_to_events ( int mb )
{
	u64 bytes	= mb * 1024 * 1024;
	u64 time_metric	= CACHE_LINE_SIZE * (1000000 / g_period_us);
	u64 events	= div64_u64 (bytes, time_metric);

	/* Return the answer to the caller */
	return events;
}

/*
 * This function converts the LLC-miss events into memory bandwidth
 */
static inline int convert_events_to_mb ( u64 events )
{
	int time_metric	= g_period_us * 1024 * 1024;
	u64 size_metric = events * CACHE_LINE_SIZE * 1000000 + (time_metric - 1);
	int mb 		= div64_u64 (size_metric, time_metric);

	/* Return the calculated mega-bytes value to caller */
	return mb;
}

/*
 * This function calculates the number of performance monitoring events which have
 * been registered so far in the current period
 */
static inline u64 perf_event_count ( struct perf_event *event )
{
	u64 event_count = local64_read (&event->count);
	u64 child_count = atomic64_read (&event->child_count);
	u64 total_count = event_count + child_count;

	/* Return the total PMC event count */
	return total_count; 
}

/*
 * This function calculates the number of PMC events the core has used so far since
 * the last period
 */
static inline u64 bwlockmod_event_used ( struct core_info *cinfo )
{
	u64 curr_event_value	= perf_event_count (cinfo->event);
	u64 new_event_value	= curr_event_value - cinfo->old_val;

	/* Return the new event count to the caller */
	return new_event_value;
}

/*
 * This function calculates the number of cores which are currently holding bandwdith lock
 * and returns that number to the caller as an integer
 *
 * TODO: Why not use the nr_bwlocked_cores () function directly?
 */
static int mg_nr_bwlocked_cores ( void )
{
	struct bwlockmod_info *global = &bwlockmod_info;
	int nr = nr_bwlocked_cores();
	int i = 0;

	/* Iterate over all the available cores */
	for_each_cpu (i, global->throttle_mask) {
		struct task_struct *t = per_cpu_ptr (core_info, i)->throttled_task;

		if (t && t->bwlock_val > 0) {
			nr += t->bwlock_val;
		}
	}

	/* Return the number of bandwidth locked cores to the caller */
	return nr;
}


static void print_core_info ( int cpu, 
			      struct core_info *cinfo )
{
	/* Print the core-related information to the trace buffer */
	pr_info ("CPU%d: budget: %d, cur_budget: %d, period: %ld\n", 
	         cpu,
		 cinfo->budget,
		 cinfo->cur_budget,
		 cinfo->period_cnt);

	/* Return to the caller */
	return;
}

/*
 * This function prints the value of the current context (e.g. IRQ, NMI etc.) to the
 * trace buffer
 */
static inline void print_current_context ( void )
{
	trace_printk ("in_interrupt(%ld)(hard(%ld),softirq(%d)"
		      ",in_nmi(%d)),irqs_disabled(%d)\n",
		      in_interrupt (),
		      in_irq (),
		      (int)in_softirq (),
		      (int)in_nmi (),
		      (int)irqs_disabled ());

	/* All done here */
	return;
}

/* 
 * This function prints the state of the CPU (online / offline) to the trace buffer. The
 * parameter 'nfb' is not used in the function body
 */
static int bwlockmod_cpu_callback ( struct notifier_block *nfb,
				    unsigned long action,
				    void *hcpu)
{
	unsigned int cpu = (unsigned long)hcpu;
	
	/* Switch based on the desired action */
	switch (action) {
		case CPU_ONLINE:
		case CPU_ONLINE_FROZEN:
			trace_printk ("CPU%d is online\n", cpu);
			break;

		case CPU_DEAD:
		case CPU_DEAD_FROZEN:
			trace_printk ("CPU%d is offline\n", cpu);
			break;
	}

	/* Return the status to caller */
	return NOTIFY_OK;
}

/*
 * TODO: Identify the purpose of this function
 */
static int bwlockmod_idle_notifier ( struct notifier_block *nb,
				     unsigned long val,
				     void *data)
{
	struct bwlockmod_info *global = &bwlockmod_info;
	unsigned long flags;

	DEBUG (trace_printk ("Idle State Update: %ld\n", val));

	spin_lock_irqsave (&global->lock, flags);

	if (val == IDLE_START) {
		cpumask_clear_cpu (smp_processor_id (), global->active_mask);
		DEBUG (if (cpumask_equal (global->throttle_mask, global->active_mask))
			      trace_printk ("DBG: Last idle\n"););
	} else {
		cpumask_set_cpu (smp_processor_id (), global->active_mask);
	}

	spin_unlock_irqrestore (&global->lock, flags);

	/* Return to the caller */
	return 0;
}

/*
 * Update per-core usage statistics
 */
void update_statistics ( struct core_info *cinfo )
{
	/* Counter must have stopped by now */
	u64 exclusive_vtime	= 0;
	int used;
	s64 new;

	/* Update the budget used so far */
	new = perf_event_count (cinfo->event);;
	used = (int)(new - cinfo->old_val); 
	cinfo->old_val = new;
	cinfo->overall.used_budget += used;
	cinfo->overall.assigned_budget += cinfo->budget;

	/* EWMA filtered per-core usage statistics */
	cinfo->used[0] = used;
	cinfo->used[1] = (cinfo->used[1] * (2 - 1) + used) >> 1; 
	cinfo->used[2] = (cinfo->used[2] * (4 - 1) + used) >> 2; 

	/* Core is currently throttled. */
	if (cinfo->throttled_task) {
		cinfo->overall.throttled_time_ns += (ktime_get ().tv64 - cinfo->throttled_time.tv64);
		cinfo->overall.throttled++;
	}

	/* Throttling Error Condition:
	   I was too aggressive in giving up "unsed" budget */
	if (cinfo->prev_throttle_error && used < cinfo->budget) {
		int diff = cinfo->budget - used;
		int idx;

		cinfo->overall.throttled_error ++;
		BUG_ON (cinfo->budget == 0);
		idx = (int)(diff * 10 / cinfo->budget);
		cinfo->overall.throttled_error_dist[idx]++;
		trace_printk ("ERR: throttled_error: %d < %d\n", used, cinfo->budget);

		/* Compensation for error to catch-up*/
		cinfo->used[PREDICTOR] = cinfo->budget + diff;
	}

	cinfo->prev_throttle_error = 0;

	/* I was the lucky guy who used the DRAM exclusively */
	if (cinfo->exclusive_mode) {
		u64 exclusive_ns, exclusive_bw;

		/* Used Time */
		exclusive_ns = (ktime_get ().tv64 - cinfo->exclusive_time.tv64);
		
		/* Used BW */
		exclusive_bw = (cinfo->used[0] - cinfo->budget);

		cinfo->overall.exclusive_ns += exclusive_ns;
		cinfo->overall.exclusive_bw += exclusive_bw;
		cinfo->exclusive_vtime += exclusive_vtime;
		cinfo->overall.exclusive++;
		cinfo->exclusive_mode = 0;
	}

	DEBUG_PROFILE (trace_printk ("%lld %d %p CPU%d org: %d cur: %d period: %ld\n",
			   	      new,
				      used,
				      cinfo->throttled_task,
			   	      smp_processor_id (), 
			   	      cinfo->budget,
			   	      cinfo->cur_budget,
			   	      cinfo->period_cnt));

	/* All done here */
	return;
}

/*
 * This is the routine which describes the behavior of kthrottle thread which
 * is used to block a core which has consumed its bandwidth budget
 */
static int throttle_thread ( void *arg )
{
	int cpunr = (unsigned long)arg;
	struct core_info *cinfo = per_cpu_ptr (core_info, cpunr);

	static const struct sched_param param = {
		.sched_priority = MAX_USER_RT_PRIO / 2,
	};

	sched_setscheduler (current, SCHED_FIFO, &param);

	while (!kthread_should_stop () && cpu_online (cpunr)) {
		DEBUG (trace_printk ("Wait for an event\n"));
		wait_event_interruptible (cinfo->throttle_evt,
					  cinfo->throttled_task ||
					  kthread_should_stop ());

		DEBUG (trace_printk ("Event Received!\n"));

		/* Break the loop if the thread needs to be stopped */
		if (kthread_should_stop ())
			break;

		/* Insert a memory barrier to synchronize across cpus */
		smp_mb ();

		while (cinfo->throttled_task && !kthread_should_stop ()) {
			cpu_relax ();

			/* Insert a memory barrier to synchronize across cpus */
			smp_mb ();
		}
	}

	DEBUG (trace_printk ("exit\n"));

	/* Return to caller */
	return 0;
}

/*
 * This function kills the kthrottle thread which is used to block a core
 */
static void __kill_throttlethread ( void *info )
{
	struct core_info *cinfo = this_cpu_ptr (core_info);

	/* Print debug message to trace buffer */
	pr_info ("Stopping kthrottle/%d\n", smp_processor_id ());
	cinfo->throttled_task = NULL;

	/* Place a memory barrier to synchronize across smp cores */
	smp_mb ();

	/* Return to the caller */
	return;
}

/*
 * This function is used to unthrottle a currently blocked core
 */
static void __unthrottle_core ( void *info )
{
	struct core_info *cinfo = this_cpu_ptr (core_info);

	/* Check if the core is actually throttled */
	if (cinfo->throttled_task) {
		cinfo->exclusive_mode = 1;
		cinfo->exclusive_time = ktime_get ();

		cinfo->throttled_task = NULL;
		smp_wmb ();
		DEBUG_RECLAIM (trace_printk ("exclusive mode begin\n"));
	}

	/* Successfully unthrottled the core */
	return;
}


/*
 * TODO: Identify what this function does.
 */
static void __newperiod ( void *info )
{
	struct bwlockmod_info *global = &bwlockmod_info;
	ktime_t start = ktime_get ();
	long period = (long)info;
	
	/* Acquire the global spin-lock to serialize access to critical section */
	spin_lock (&global->lock);

	if (period == global->period_cnt) {
		ktime_t new_expire = ktime_add (start, global->period_in_ktime);
		long new_period = ++global->period_cnt;
		global->budget = 0;

		/* Release the spin-lock */
		spin_unlock (&global->lock);

		/* Arrived before timer interrupt is called */
		hrtimer_start_range_ns (&global->hr_timer, new_expire, 0, HRTIMER_MODE_ABS_PINNED);
		DEBUG (trace_printk ("Begin new period\n"));

		/* Invoke the slave callback on each system core */
		on_each_cpu (period_timer_callback_slave, (void *)new_period, 0);
	} else
		/* Release the spin-lock */
		spin_unlock(&global->lock);

	/* All done here */
	return;
}

/*
 * TODO: Identify the purpose of dynamic priority
 */
#if USE_BWLOCK_DYNPRIO
static void set_load_weight ( struct task_struct *p )
{
	int prio = p->static_prio - MAX_RT_PRIO;
	struct load_weight *load = &p->se.load;

	load->weight = prio_to_weight[prio];
	load->inv_weight = prio_to_wmult[prio];

	/* Return to the caller */
	return;
}

void intr_set_user_nice ( struct task_struct *p,
			  long nice )
{
	/* Lets see if this task is nice */
	if (task_nice (p) == nice || nice < -20 || nice > 19)
		/* Nopes. This is a rude one! */
		return;

	if (p->policy != SCHED_NORMAL)
		/* We don't deal with no normal tasks :( */
		return;

	p->static_prio = NICE_TO_PRIO (nice);
	set_load_weight (p);
	p->prio = p->static_prio;

	/* All done here */
	return;
}
#endif

static void __reset_stats ( void *info )
{
	struct core_info *cinfo = this_cpu_ptr (core_info);
	DEBUG_USER (trace_printk ("CPU%d\n", smp_processor_id ()));

	/* Update Local Period Information */
	cinfo->period_cnt = 0;

	/* Initial Condition */
	cinfo->used[0] = cinfo->used[1] = cinfo->used[2] = cinfo->budget; 

	cinfo->overall.throttled_time_ns = 0;
	cinfo->overall.assigned_budget = 0;
	cinfo->overall.throttled_error = 0;
	cinfo->cur_budget = cinfo->budget;
	cinfo->overall.used_budget = 0;
	cinfo->overall.throttled = 0;

	memset (cinfo->overall.throttled_error_dist, 0, sizeof(int)*10);
	cinfo->throttled_time = ktime_set (0,0);


	/* Place a memory barrier to synchronize across smp cores */
	smp_mb ();

	DEBUG_USER (trace_printk ("MSG: Clear statistics of Core%d\n",
				   smp_processor_id ()));

	/* Return to the caller */
	return;
}

static ssize_t bwlockmod_control_write ( struct file *filp,
					 const char __user *ubuf,
					 size_t cnt,
					 loff_t *ppos )
{
	char buf[BUF_SIZE];
	char *p = buf;

	if (copy_from_user (&buf, ubuf, (cnt > BUF_SIZE)? BUF_SIZE : cnt) != 0)
		return 0;

	if (!strncmp (p, "reclaim ", 8))
		sscanf (p+8, "%d", &g_use_reclaim);
	else if (!strncmp (p, "exclusive ", 10))
		sscanf (p+10, "%d", &g_use_exclusive);
	else
		pr_info ("ERROR: %s\n", p);
	smp_mb ();

	/* Return the count to the caller */
	return cnt;
}

static int bwlockmod_control_show ( struct seq_file *m,
				    void *v )
{
	struct bwlockmod_info *global = &bwlockmod_info;
	char buf[64];

	seq_printf (m, "reclaim: %d\n", g_use_reclaim);
	seq_printf (m, "exclusive: %d\n", g_use_exclusive);
	cpulist_scnprintf (buf, 64, global->active_mask);

	seq_printf (m, "active: %s\n", buf);
	cpulist_scnprintf (buf, 64, global->throttle_mask);
	seq_printf (m, "throttle: %s\n", buf);

	/* Return to caller */
	return 0;
}

static int bwlockmod_control_open ( struct inode *inode,
				    struct file *filp )
{
	return single_open (filp, bwlockmod_control_show, NULL);
}

static void __update_budget ( void *info )
{
	struct core_info *cinfo = this_cpu_ptr (core_info);

	if ((unsigned long)info == 0) {
		pr_info ("ERR: Requested budget is zero\n");
		return;
	}
	cinfo->limit = (unsigned long)info;
	smp_mb ();
	DEBUG_USER (trace_printk ("MSG: New budget of Core%d is %d\n",
				  smp_processor_id (), 
				  cinfo->budget));


	/* All done here */
	return;
}

static ssize_t bwlockmod_limit_write ( struct file *filp,
				       const char __user *ubuf,
				       size_t cnt,
				       loff_t *ppos )
{
	char buf[BUF_SIZE];
	int use_mb = 0;
	char *p = buf;
	int i;

	if (copy_from_user (&buf, ubuf, (cnt > BUF_SIZE)? BUF_SIZE : cnt) != 0) 
		return 0;

	if (!strncmp (p, "mb ", 3)) {
		use_mb = 1;
		p += 3;
	}

	/* Iterate over each online cpu */
	for_each_online_cpu (i) {
		unsigned long events;
		int input;

		sscanf (p, "%d", &input);

		if (input == 0) {
			pr_err ("ERR: CPU%d: input is zero: %s.\n",i, p);
			continue;
		}

		if (!use_mb)
			input = (sysctl_maxperf_bw_mb * 100) / input;

		/* Get the amount of bandwidth expanded so far */
		events = (unsigned long)convert_mb_to_events (input);

		/* Print the output to trace buffer */
		pr_info ("CPU%d: New budget = %ld (%d %s)\n",
			  i, 
			  events,
			  input,
			  (use_mb)? "MB/s" : "pct");

		/* Invoke the update-budget function on smp cores */
		smp_call_function_single (i, __update_budget, (void *)events, 0);
		
		/* Get the first occurence of space in the string p */
		p = strchr (p, ' ');

		/* Break if the character is not found */
		if (!p) 
			break;

		/* Move on to the next character in the string */
		p++;
	}

	/* Insert a memory barrier to synchronize across smp cores */
	smp_mb ();

	/* Return to the count to caller */
	return cnt;
}


static int bwlockmod_limit_show ( struct seq_file *m,
				  void *v )
{
	struct bwlockmod_info *global = &bwlockmod_info;
	int i, j, cpu;
	cpu = get_cpu ();

	/* Insert a memory barrier to synchronize across smp cores */
	smp_mb ();

	/* Print status message to the relevant file */
	seq_printf (m, "cpu  |budget (MB/s)\n");
	seq_printf (m, "-------------------------------\n");

	/* Iterate over all online cpus */
	for_each_online_cpu (i) {
		struct core_info *cinfo = per_cpu_ptr (core_info, i);
		int budget = 0;

		if (cinfo->limit > 0) {
			budget = cinfo->limit;
		}
	
		seq_printf (m, "CPU%d: %d (%dMB/s)\n",
			    i,
			    budget,
			    convert_events_to_mb (budget));

#if USE_BWLOCK_DYNPRIO
		for (j = 0; j < cinfo->dprio_cnt; j++) {
			seq_printf (m, "\t%12s  %3d\n",
				    (cinfo->dprio[j].task)->comm, 
				    cinfo->dprio[j].origprio);
		}
#endif
	}

	seq_printf (m, "bwlocked_core: %d\n", global->bwlocked_cores);

	put_cpu ();

	/* Return to the caller */
	return 0;

}

static int bwlockmod_limit_open ( struct inode *inode,
				  struct file *filp )
{
	return single_open (filp, bwlockmod_limit_show, NULL);
}

static int bwlockmod_usage_show ( struct seq_file *m,
				  void *v )
{
	int i, j;

	/* Place a memory barrier to synchronize across smp cores */
	smp_mb ();

	/* Current Utilization */
	for (j = 0; j < 3; j++) {
		for_each_online_cpu (i) {
			struct core_info *cinfo = per_cpu_ptr (core_info, i);
			u64 budget, used, util;

			budget = cinfo->budget;
			used = cinfo->used[j];
			util = div64_u64 (used * 100, (budget)? budget : 1);
			seq_printf (m, "%llu ", util);
		}

		seq_printf (m, "\n");
	}

	seq_printf (m, "<overall>----\n");

	/* Overall Utilization
	   WARN: Assume budget did not change */
	for_each_online_cpu (i) {
		struct core_info *cinfo = per_cpu_ptr (core_info, i);
		u64 total_budget, total_used, result;

		total_budget = cinfo->overall.assigned_budget;
		total_used   = cinfo->overall.used_budget;
		result       = div64_u64 (total_used * 100, (total_budget)? total_budget : 1 );

		seq_printf (m, "%lld ", result);
	}

	/* Return to the caller */
	return 0;
}


static int bwlockmod_usage_open ( struct inode *inode,
				  struct file *filp )
{
	return single_open (filp, bwlockmod_usage_show, NULL);
}

static ssize_t bwlockmod_failcnt_write ( struct file *filp,
					 const char __user *ubuf,
					 size_t cnt,
					 loff_t *ppos )
{
	/* Reset Local Statistics */
	struct bwlockmod_info *global = &bwlockmod_info;

	/* Acquire the spin-lock for critical section */
	spin_lock (&global->lock);
	global->budget = global->period_cnt = 0;
	global->start_tick = jiffies;

	/* Release the spin-lock after critical section */
	spin_unlock (&global->lock);

	smp_mb ();
	on_each_cpu (__reset_stats, NULL, 0);

	/* Return the count to caller */
	return cnt;
}

static int bwlockmod_failcnt_show ( struct seq_file *m,
				     void *v )
{
	int i;

	/* Place a memroy barrier to synchronize across smp cores */
	smp_mb();

	/* Total #of throttled periods */
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

	/* Return the count to caller */
	return 0;
}

static int bwlockmod_failcnt_open ( struct inode *inode,
				    struct file *filp )
{
	return single_open (filp, bwlockmod_failcnt_show, NULL);
}

static int bwlockmod_init_debugfs ( void )
{
	bwlockmod_dir = debugfs_create_dir ("bwlockmod", NULL);

	BUG_ON (!bwlockmod_dir);

	debugfs_create_file ("control", 0444, bwlockmod_dir, NULL, &bwlockmod_control_fops);
	debugfs_create_file ("limit", 	0444, bwlockmod_dir, NULL, &bwlockmod_limit_fops);
	debugfs_create_file ("usage", 	0666, bwlockmod_dir, NULL, &bwlockmod_usage_fops);
	debugfs_create_file ("failcnt", 0644, bwlockmod_dir, NULL, &bwlockmod_failcnt_fops);

	/* Return to the caller */
	return 0;

}

/*
 * TODO: Identify the purpose of this function
 */
static void test_ipi_cb ( void *info )
{
	/* Indicate in the trace buffer that IPI has been called on a particular processor */
	trace_printk ("IPI called on %d\n", smp_processor_id ());

	/* Okie dokie */
	return;
}

/*
 * TODO: Identify the purpose of this function
 */
enum hrtimer_restart test_timer_cb ( struct hrtimer *timer )
{
	enum hrtimer_restart status = HRTIMER_RESTART;
	ktime_t now;

	/* As if 'now' can be anything else. duh! */
	now = timer->base->get_time ();

	/* Initiate time travel */
	hrtimer_forward (timer, now, ktime_set (0, 1000 * 1000));

	/* Indicate in the trace buffer that master has started processing */
	trace_printk ("Master begin\n");

	/* Invoke IPI on each core */
	on_each_cpu (test_ipi_cb, 0, 0);

	/* Indicate in the trace buffer that master core is done processing */
	trace_printk("master end\n");

	/* Track test time */
	g_test--;

	/* Place memory barrier to synchronize across smp cores */
	smp_mb();

	/* Verify if the test value is correct */
	if (g_test == 0) 
		status = HRTIMER_NORESTART;

	/* Return the status value to caller */
	return status;
}

/*
 * TODO: Identify the purpose of this function
 */
static int self_test ( void )
{
	struct hrtimer __test_hr_timer;

	/* Initialize the high-resolution timer */
	hrtimer_init (&__test_hr_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED);

	/* Register the timer call-back function */
	__test_hr_timer.function = &test_timer_cb;

	/* Initiate the timer */
	hrtimer_start(&__test_hr_timer,
		      ktime_set(0, 1000 * 1000), /* 1ms */
		      HRTIMER_MODE_REL_PINNED);

	/* Loop while test is running */
	while (g_test) {
		/* Place memory barrier to synchronize across smp cores */
		smp_mb();

		/* Cut the cpu some slack */
		cpu_relax();
	}

	/* Skadoooooosh... */
	return 0;
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Heechul Yun <heechul@illinois.edu>");
