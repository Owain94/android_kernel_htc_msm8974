/*
 *  linux/mm/oom_kill.c
 * 
 *  Copyright (C)  1998,2000  Rik van Riel
 *	Thanks go out to Claus Fischer for some serious inspiration and
 *	for goading me into coding this file...
 *  Copyright (C)  2010  Google, Inc.
 *	Rewritten by David Rientjes
 *
 *  The routines in this file are used to kill a process when
 *  we're seriously out of memory. This gets called from __alloc_pages()
 *  in mm/page_alloc.c when we really run out of memory.
 *
 *  Since we won't call these routines often (on a well-configured
 *  machine) this file will double as a 'coding guide' and a signpost
 *  for newbie kernel hackers. It features several pointers to major
 *  kernel subsystems and hints as to where to find out what things do.
 */

#include <linux/oom.h>
#include <linux/mm.h>
#include <linux/err.h>
#include <linux/gfp.h>
#include <linux/sched.h>
#include <linux/swap.h>
#include <linux/timex.h>
#include <linux/jiffies.h>
#include <linux/cpuset.h>
#include <linux/export.h>
#include <linux/notifier.h>
#include <linux/memcontrol.h>
#include <linux/mempolicy.h>
#include <linux/security.h>
#include <linux/ptrace.h>
#include <linux/freezer.h>
#include <linux/ftrace.h>
#include <linux/ratelimit.h>

#define CREATE_TRACE_POINTS
#include <trace/events/oom.h>

int sysctl_panic_on_oom;
int sysctl_oom_kill_allocating_task;
int sysctl_oom_dump_tasks = 1;
static DEFINE_SPINLOCK(zone_scan_lock);

extern void show_meminfo(void);

void compare_swap_oom_score_adj(int old_val, int new_val)
{
	struct sighand_struct *sighand = current->sighand;

	spin_lock_irq(&sighand->siglock);
	if (current->signal->oom_score_adj == old_val)
		current->signal->oom_score_adj = new_val;
	trace_oom_score_adj_update(current);
	spin_unlock_irq(&sighand->siglock);
}

int test_set_oom_score_adj(int new_val)
{
	struct sighand_struct *sighand = current->sighand;
	int old_val;

	spin_lock_irq(&sighand->siglock);
	old_val = current->signal->oom_score_adj;
	current->signal->oom_score_adj = new_val;
	trace_oom_score_adj_update(current);
	spin_unlock_irq(&sighand->siglock);

	return old_val;
}

#ifdef CONFIG_NUMA
static bool has_intersects_mems_allowed(struct task_struct *tsk,
					const nodemask_t *mask)
{
	struct task_struct *start = tsk;

	do {
		if (mask) {
			if (mempolicy_nodemask_intersects(tsk, mask))
				return true;
		} else {
			if (cpuset_mems_allowed_intersects(current, tsk))
				return true;
		}
	} while_each_thread(start, tsk);

	return false;
}
#else
static bool has_intersects_mems_allowed(struct task_struct *tsk,
					const nodemask_t *mask)
{
	return true;
}
#endif 

struct task_struct *find_lock_task_mm(struct task_struct *p)
{
	struct task_struct *t = p;

	do {
		task_lock(t);
		if (likely(t->mm))
			return t;
		task_unlock(t);
	} while_each_thread(p, t);

	return NULL;
}

static bool oom_unkillable_task(struct task_struct *p,
		const struct mem_cgroup *memcg, const nodemask_t *nodemask)
{
	if (is_global_init(p))
		return true;
	if (p->flags & PF_KTHREAD)
		return true;

	
	if (memcg && !task_in_mem_cgroup(p, memcg))
		return true;

	
	if (!has_intersects_mems_allowed(p, nodemask))
		return true;

	return false;
}

/**
 * oom_badness - heuristic function to determine which candidate task to kill
 * @p: task struct of which task we should calculate
 * @totalpages: total present RAM allowed for page allocation
 *
 * The heuristic for determining which task to kill is made to be as simple and
 * predictable as possible.  The goal is to return the highest value for the
 * task consuming the most memory to avoid subsequent oom failures.
 */
unsigned long oom_badness(struct task_struct *p, struct mem_cgroup *memcg,
			  const nodemask_t *nodemask, unsigned long totalpages)
{
	long points;
	long adj;

	if (oom_unkillable_task(p, memcg, nodemask))
		return 0;

	p = find_lock_task_mm(p);
	if (!p)
		return 0;

	adj = p->signal->oom_score_adj;
	if (adj == OOM_SCORE_ADJ_MIN) {
		task_unlock(p);
		return 0;
	}

	/*
	 * The baseline for the badness score is the proportion of RAM that each
	 * task's rss, pagetable and swap space use.
	 */
	points = get_mm_rss(p->mm) + p->mm->nr_ptes +
		 get_mm_counter(p->mm, MM_SWAPENTS);
	task_unlock(p);

	if (has_capability_noaudit(p, CAP_SYS_ADMIN))
		adj -= 30;

	/* Normalize to oom_score_adj units */
	adj *= totalpages / 1000;
	points += adj;

	/*
	 * Never return 0 for an eligible task regardless of the root bonus and
	 * oom_score_adj (oom_score_adj can't be OOM_SCORE_ADJ_MIN here).
	 */
	return points > 0 ? points : 1;
}

#ifdef CONFIG_NUMA
static enum oom_constraint constrained_alloc(struct zonelist *zonelist,
				gfp_t gfp_mask, nodemask_t *nodemask,
				unsigned long *totalpages)
{
	struct zone *zone;
	struct zoneref *z;
	enum zone_type high_zoneidx = gfp_zone(gfp_mask);
	bool cpuset_limited = false;
	int nid;

	
	*totalpages = totalram_pages + total_swap_pages;

	if (!zonelist)
		return CONSTRAINT_NONE;
	if (gfp_mask & __GFP_THISNODE)
		return CONSTRAINT_NONE;

	if (nodemask && !nodes_subset(node_states[N_HIGH_MEMORY], *nodemask)) {
		*totalpages = total_swap_pages;
		for_each_node_mask(nid, *nodemask)
			*totalpages += node_spanned_pages(nid);
		return CONSTRAINT_MEMORY_POLICY;
	}

	
	for_each_zone_zonelist_nodemask(zone, z, zonelist,
			high_zoneidx, nodemask)
		if (!cpuset_zone_allowed_softwall(zone, gfp_mask))
			cpuset_limited = true;

	if (cpuset_limited) {
		*totalpages = total_swap_pages;
		for_each_node_mask(nid, cpuset_current_mems_allowed)
			*totalpages += node_spanned_pages(nid);
		return CONSTRAINT_CPUSET;
	}
	return CONSTRAINT_NONE;
}
#else
static enum oom_constraint constrained_alloc(struct zonelist *zonelist,
				gfp_t gfp_mask, nodemask_t *nodemask,
				unsigned long *totalpages)
{
	*totalpages = totalram_pages + total_swap_pages;
	return CONSTRAINT_NONE;
}
#endif

enum oom_scan_t {
	OOM_SCAN_OK,		/* scan thread and find its badness */
	OOM_SCAN_CONTINUE,	/* do not consider thread for oom kill */
	OOM_SCAN_ABORT,		/* abort the iteration and return */
	OOM_SCAN_SELECT,	/* always select this thread first */
};

static enum oom_scan_t oom_scan_process_thread(struct task_struct *task,
		struct mem_cgroup *memcg, unsigned long totalpages,
		const nodemask_t *nodemask, bool force_kill)
{
	if (task->exit_state)
		return OOM_SCAN_CONTINUE;
	if (oom_unkillable_task(task, memcg, nodemask))
		return OOM_SCAN_CONTINUE;

	/*
	 * This task already has access to memory reserves and is being killed.
	 * Don't allow any other task to have access to the reserves.
	 */
	if (test_tsk_thread_flag(task, TIF_MEMDIE)) {
		if (unlikely(frozen(task)))
			__thaw_task(task);
		if (!force_kill)
			return OOM_SCAN_ABORT;
	}
	if (!task->mm)
		return OOM_SCAN_CONTINUE;

	if (task->flags & PF_EXITING) {
		/*
		 * If task is current and is in the process of releasing memory,
		 * allow the "kill" to set TIF_MEMDIE, which will allow it to
		 * access memory reserves.  Otherwise, it may stall forever.
		 *
		 * The iteration isn't broken here, however, in case other
		 * threads are found to have already been oom killed.
		 */
		if (task == current)
			return OOM_SCAN_SELECT;
		else if (!force_kill) {
			/*
			 * If this task is not being ptraced on exit, then wait
			 * for it to finish before killing some other task
			 * unnecessarily.
			 */
			if (!(task->group_leader->ptrace & PT_TRACE_EXIT))
				return OOM_SCAN_ABORT;
		}
	}
	return OOM_SCAN_OK;
}

/*
 * Simple selection loop. We chose the process with the highest
 * number of 'points'. We expect the caller will lock the tasklist.
 *
 * (not docbooked, we don't want this one cluttering up the manual)
 */
static struct task_struct *select_bad_process(unsigned int *ppoints,
		unsigned long totalpages, struct mem_cgroup *memcg,
		const nodemask_t *nodemask, bool force_kill)
{
	struct task_struct *g, *p;
	struct task_struct *chosen = NULL;
	unsigned long chosen_points = 0;

	do_each_thread(g, p) {
		unsigned int points;

		switch (oom_scan_process_thread(p, memcg, totalpages, nodemask,
						force_kill)) {
		case OOM_SCAN_SELECT:
			chosen = p;
			chosen_points = ULONG_MAX;
			/* fall through */
		case OOM_SCAN_CONTINUE:
			continue;
		case OOM_SCAN_ABORT:
			return ERR_PTR(-1UL);
		case OOM_SCAN_OK:
			break;
		};
		points = oom_badness(p, memcg, nodemask, totalpages);
		if (points > chosen_points) {
			chosen = p;
			chosen_points = points;
		}
	} while_each_thread(g, p);

	*ppoints = chosen_points * 1000 / totalpages;
	return chosen;
}

static void dump_tasks(const struct mem_cgroup *memcg, const nodemask_t *nodemask)
{
	struct task_struct *p;
	struct task_struct *task;

	pr_info("[ pid ]   uid  tgid total_vm      rss cpu oom_adj oom_score_adj name\n");
	for_each_process(p) {
		if (oom_unkillable_task(p, memcg, nodemask))
			continue;

		task = find_lock_task_mm(p);
		if (!task) {
			continue;
		}

		pr_info("[%5d] %5d %5d %8lu %8lu %3u     %3d         %5d %s\n",
			task->pid, task_uid(task), task->tgid,
			task->mm->total_vm, get_mm_rss(task->mm),
			task_cpu(task), task->signal->oom_adj,
			task->signal->oom_score_adj, task->comm);
		task_unlock(task);
	}
}

static void dump_header(struct task_struct *p, gfp_t gfp_mask, int order,
			struct mem_cgroup *memcg, const nodemask_t *nodemask)
{
	task_lock(current);
	pr_warning("%s invoked oom-killer: gfp_mask=0x%x, order=%d, "
		"oom_adj=%d, oom_score_adj=%d\n",
		current->comm, gfp_mask, order, current->signal->oom_adj,
		current->signal->oom_score_adj);
	cpuset_print_task_mems_allowed(current);
	task_unlock(current);
	dump_stack();
	show_meminfo();
	mem_cgroup_print_oom_info(memcg, p);
	show_mem(SHOW_MEM_FILTER_NODES);
	if (sysctl_oom_dump_tasks)
		dump_tasks(memcg, nodemask);
}

#define K(x) ((x) << (PAGE_SHIFT-10))
static void oom_kill_process(struct task_struct *p, gfp_t gfp_mask, int order,
			     unsigned int points, unsigned long totalpages,
			     struct mem_cgroup *memcg, nodemask_t *nodemask,
			     const char *message)
{
	struct task_struct *victim = p;
	struct task_struct *child;
	struct task_struct *t = p;
	struct mm_struct *mm;
	unsigned int victim_points = 0;
	static DEFINE_RATELIMIT_STATE(oom_rs, DEFAULT_RATELIMIT_INTERVAL,
					      DEFAULT_RATELIMIT_BURST);

	if (p->flags & PF_EXITING) {
		set_tsk_thread_flag(p, TIF_MEMDIE);
		return;
	}

	if (__ratelimit(&oom_rs))
		dump_header(p, gfp_mask, order, memcg, nodemask);

	task_lock(p);
	pr_err("%s: Kill process %d (%s) score %d or sacrifice child\n",
		message, task_pid_nr(p), p->comm, points);
	task_unlock(p);

	do {
		list_for_each_entry(child, &t->children, sibling) {
			unsigned int child_points;

			if (child->mm == p->mm)
				continue;
			child_points = oom_badness(child, memcg, nodemask,
								totalpages);
			if (child_points > victim_points) {
				victim = child;
				victim_points = child_points;
			}
		}
	} while_each_thread(p, t);

	victim = find_lock_task_mm(victim);
	if (!victim)
		return;

	
	mm = victim->mm;
	pr_err("Killed process %d (%s) total-vm:%lukB, anon-rss:%lukB, file-rss:%lukB\n",
		task_pid_nr(victim), victim->comm, K(victim->mm->total_vm),
		K(get_mm_counter(victim->mm, MM_ANONPAGES)),
		K(get_mm_counter(victim->mm, MM_FILEPAGES)));
	task_unlock(victim);

	for_each_process(p)
		if (p->mm == mm && !same_thread_group(p, victim) &&
		    !(p->flags & PF_KTHREAD)) {
			if (p->signal->oom_score_adj == OOM_SCORE_ADJ_MIN)
				continue;

			task_lock(p);	
			pr_err("Kill process %d (%s) sharing same memory\n",
				task_pid_nr(p), p->comm);
			task_unlock(p);
			do_send_sig_info(SIGKILL, SEND_SIG_FORCED, p, true);
		}

	set_tsk_thread_flag(victim, TIF_MEMDIE);
	do_send_sig_info(SIGKILL, SEND_SIG_FORCED, victim, true);
}
#undef K

static void check_panic_on_oom(enum oom_constraint constraint, gfp_t gfp_mask,
				int order, const nodemask_t *nodemask)
{
	if (likely(!sysctl_panic_on_oom))
		return;
	if (sysctl_panic_on_oom != 2) {
		if (constraint != CONSTRAINT_NONE)
			return;
	}
	read_lock(&tasklist_lock);
	dump_header(NULL, gfp_mask, order, NULL, nodemask);
	read_unlock(&tasklist_lock);
	panic("Out of memory: %s panic_on_oom is enabled\n",
		sysctl_panic_on_oom == 2 ? "compulsory" : "system-wide");
}

#ifdef CONFIG_CGROUP_MEM_RES_CTLR
void mem_cgroup_out_of_memory(struct mem_cgroup *memcg, gfp_t gfp_mask,
			      int order)
{
	unsigned long limit;
	unsigned int points = 0;
	struct task_struct *p;

	if (fatal_signal_pending(current)) {
		set_thread_flag(TIF_MEMDIE);
		return;
	}

	check_panic_on_oom(CONSTRAINT_MEMCG, gfp_mask, order, NULL);
	limit = mem_cgroup_get_limit(memcg) >> PAGE_SHIFT ? : 1;
	read_lock(&tasklist_lock);
	p = select_bad_process(&points, limit, memcg, NULL, false);
	if (p && PTR_ERR(p) != -1UL)
		oom_kill_process(p, gfp_mask, order, points, limit, memcg, NULL,
				 "Memory cgroup out of memory");
	read_unlock(&tasklist_lock);
}
#endif

static BLOCKING_NOTIFIER_HEAD(oom_notify_list);

int register_oom_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&oom_notify_list, nb);
}
EXPORT_SYMBOL_GPL(register_oom_notifier);

int unregister_oom_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&oom_notify_list, nb);
}
EXPORT_SYMBOL_GPL(unregister_oom_notifier);

int try_set_zonelist_oom(struct zonelist *zonelist, gfp_t gfp_mask)
{
	struct zoneref *z;
	struct zone *zone;
	int ret = 1;

	spin_lock(&zone_scan_lock);
	for_each_zone_zonelist(zone, z, zonelist, gfp_zone(gfp_mask)) {
		if (zone_is_oom_locked(zone)) {
			ret = 0;
			goto out;
		}
	}

	for_each_zone_zonelist(zone, z, zonelist, gfp_zone(gfp_mask)) {
		zone_set_flag(zone, ZONE_OOM_LOCKED);
	}

out:
	spin_unlock(&zone_scan_lock);
	return ret;
}

void clear_zonelist_oom(struct zonelist *zonelist, gfp_t gfp_mask)
{
	struct zoneref *z;
	struct zone *zone;

	spin_lock(&zone_scan_lock);
	for_each_zone_zonelist(zone, z, zonelist, gfp_zone(gfp_mask)) {
		zone_clear_flag(zone, ZONE_OOM_LOCKED);
	}
	spin_unlock(&zone_scan_lock);
}

static int try_set_system_oom(void)
{
	struct zone *zone;
	int ret = 1;

	spin_lock(&zone_scan_lock);
	for_each_populated_zone(zone)
		if (zone_is_oom_locked(zone)) {
			ret = 0;
			goto out;
		}
	for_each_populated_zone(zone)
		zone_set_flag(zone, ZONE_OOM_LOCKED);
out:
	spin_unlock(&zone_scan_lock);
	return ret;
}

static void clear_system_oom(void)
{
	struct zone *zone;

	spin_lock(&zone_scan_lock);
	for_each_populated_zone(zone)
		zone_clear_flag(zone, ZONE_OOM_LOCKED);
	spin_unlock(&zone_scan_lock);
}

void out_of_memory(struct zonelist *zonelist, gfp_t gfp_mask,
		int order, nodemask_t *nodemask, bool force_kill)
{
	const nodemask_t *mpol_mask;
	struct task_struct *p;
	unsigned long totalpages;
	unsigned long freed = 0;
	unsigned int points;
	enum oom_constraint constraint = CONSTRAINT_NONE;
	int killed = 0;

	blocking_notifier_call_chain(&oom_notify_list, 0, &freed);
	if (freed > 0)
		
		return;

	if (fatal_signal_pending(current)) {
		set_thread_flag(TIF_MEMDIE);
		return;
	}

	constraint = constrained_alloc(zonelist, gfp_mask, nodemask,
						&totalpages);
	mpol_mask = (constraint == CONSTRAINT_MEMORY_POLICY) ? nodemask : NULL;
	check_panic_on_oom(constraint, gfp_mask, order, mpol_mask);

	read_lock(&tasklist_lock);
	if (sysctl_oom_kill_allocating_task && current->mm &&
	    !oom_unkillable_task(current, NULL, nodemask) &&
	    current->signal->oom_score_adj != OOM_SCORE_ADJ_MIN) {
		oom_kill_process(current, gfp_mask, order, 0, totalpages, NULL,
				 nodemask,
				 "Out of memory (oom_kill_allocating_task)");
		goto out;
	}

	p = select_bad_process(&points, totalpages, NULL, mpol_mask,
			       force_kill);
	
	if (!p) {
		dump_header(NULL, gfp_mask, order, NULL, mpol_mask);
		read_unlock(&tasklist_lock);
		panic("Out of memory and no killable processes...\n");
	}
	if (PTR_ERR(p) != -1UL) {
		oom_kill_process(p, gfp_mask, order, points, totalpages, NULL,
				 nodemask, "Out of memory");
		killed = 1;
	}
out:
	read_unlock(&tasklist_lock);

	/*
	 * Give the killed threads a good chance of exiting before trying to
	 * allocate memory again.
	 */
	if (killed)
		schedule_timeout_killable(1);
}

void pagefault_out_of_memory(void)
{
	if (try_set_system_oom()) {
		out_of_memory(NULL, 0, 0, NULL, false);
		clear_system_oom();
	}
	schedule_timeout_killable(1);
}
