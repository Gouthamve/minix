/* This file contains the scheduling policy for SCHED
 *
 * The entry points are:
 *   do_noquantum:        Called on behalf of process' that run out of quantum
 *   do_start_scheduling  Request to start scheduling a proc
 *   do_stop_scheduling   Request to stop scheduling a proc
 *   do_nice		  Request to change the nice level on a proc
 *   init_scheduling      Called from main.c to set up/prepare scheduling
 */
#include "sched.h"
#include "schedproc.h"
#include <assert.h>
#include <minix/com.h>
#include <machine/archtypes.h>
#include "../../kernel/proc.h" /* for queue constants */
#include <stdlib.h>
#include <sys/resource.h>

static minix_timer_t sched_timer;
static unsigned balance_timeout;

#define BALANCE_TIMEOUT	5 /* how often to balance queues in seconds */

#define DEFAULT_QUEUE 7

#ifndef PRIO_MIN
#define PRIO_MIN -20
#endif
#ifndef PRIO_MAX
#define PRIO_MAX 20
#endif
#ifndef SCHED_EDF
#define SCHED_EDF 1
#endif
 
static int schedule_process(struct schedproc * rmp, unsigned flags);
static void balance_queues(minix_timer_t *tp);

int SAME_sched_noquantum(struct schedproc *rmp, int pid);
int NEW_sched_noquantum(struct schedproc *rmp, int pid);
int sched_start_scheduling(struct schedproc *rmp, int pid);
int sched_stop_scheduling(struct schedproc *rmp, int pid);
int sched_start_scheduling2(struct schedproc *rmp, int pid);
int sched_start_scheduling3(struct schedproc *rmp, int pid);
int sched_do_nice1(struct schedproc *rmp, int pid);
int sched_balance_queues(struct schedproc *rmp, int pid);
int do_edf();
int do_setdl(message *m_ptr);
int choose_scheduler(message *m_ptr);
int do_getsystime(message *m_ptr);

static long cur_edf_clock;

#define SCHEDULE_CHANGE_PRIO	0x1
#define SCHEDULE_CHANGE_QUANTUM	0x2
#define SCHEDULE_CHANGE_CPU	0x4

#define SCHEDULE_CHANGE_ALL	(	\
		SCHEDULE_CHANGE_PRIO	|	\
		SCHEDULE_CHANGE_QUANTUM	|	\
		SCHEDULE_CHANGE_CPU		\
		)

#define schedule_process_local(p)	\
	schedule_process(p, SCHEDULE_CHANGE_PRIO | SCHEDULE_CHANGE_QUANTUM)
#define schedule_process_migrate(p)	\
	schedule_process(p, SCHEDULE_CHANGE_CPU)

#define CPU_DEAD	-1

#define cpu_is_available(c)	(cpu_proc[c] >= 0)

#define DEFAULT_USER_TIME_SLICE 200

// Global constant to choose Preemptive / Non-preemptive scheduling.
// 0 - Premptive , 1 - Non-preemptive.
int sched_type=0;

/* processes created by RS are sysytem processes */
#define is_system_proc(p)	((p)->parent == RS_PROC_NR)

static unsigned cpu_proc[CONFIG_MAX_CPUS];

static void pick_cpu(struct schedproc * proc)
{
#ifdef CONFIG_SMP
	unsigned cpu, c;
	unsigned cpu_load = (unsigned) -1;
	
	if (machine.processors_count == 1) {
		proc->cpu = machine.bsp_id;
		return;
	}

	/* schedule sysytem processes only on the boot cpu */
	if (is_system_proc(proc)) {
		proc->cpu = machine.bsp_id;
		return;
	}

	/* if no other cpu available, try BSP */
	cpu = machine.bsp_id;
	for (c = 0; c < machine.processors_count; c++) {
		/* skip dead cpus */
		if (!cpu_is_available(c))
			continue;
		if (c != machine.bsp_id && cpu_load > cpu_proc[c]) {
			cpu_load = cpu_proc[c];
			cpu = c;
		}
	}
	proc->cpu = cpu;
	cpu_proc[cpu]++;
#else
	proc->cpu = 0;
#endif
}

/*===========================================================================*
 *				do_noquantum				     *
 *===========================================================================*/

int do_noquantum(message *m_ptr)
{
	register struct schedproc *rmp;
	int rv, proc_nr_n;

	if (sched_isokendpt(m_ptr->m_source, &proc_nr_n) != OK) {
		printf("SCHED: WARNING: got an invalid endpoint in OOQ msg %u.\n",
		m_ptr->m_source);
		return EBADEPT;
	}

	rmp = &schedproc[proc_nr_n];

  // Check if it is an RT Process.
  // Basically, if the priority belongs to newly added queue number (16).
  if(rmp->priority<=MIN_USER_Q && rmp->priority>=MAX_USER_Q){
    SAME_sched_noquantum(rmp, proc_nr_n);
  }
  else if(rmp->priority < MAX_USER_Q -1){
    // Increasing priority does not bring it to the RT queue.
		rmp->priority += 1; /* lower priority by incrementing queue number */
  }

  // Schedule locally if not RT process.
	if (rmp->priority <= MAX_USER_Q -1) {
    if ((rv = schedule_process_local(rmp)) != OK) {
      return rv;
    }
  }else if(rv==(NEW_sched_noquantum(rmp,proc_nr_n))!=OK){
    return rv;
  }

	return OK;
}

/*===========================================================================*
 *				do_stop_scheduling			     *
 *===========================================================================*/
int do_stop_scheduling(message *m_ptr)
{
	register struct schedproc *rmp;
	int proc_nr_n;

	/* check who can send you requests */
	if (!accept_message(m_ptr))
		return EPERM;

	if (sched_isokendpt(m_ptr->m_lsys_sched_scheduling_stop.endpoint,
		    &proc_nr_n) != OK) {
		printf("SCHED: WARNING: got an invalid endpoint in OOQ msg "
		"%d\n", m_ptr->m_lsys_sched_scheduling_stop.endpoint);
		return EBADEPT;
	}

	rmp = &schedproc[proc_nr_n];
#ifdef CONFIG_SMP
	cpu_proc[rmp->cpu]--;
#endif
	rmp->flags = 0; /*&= ~IN_USE;*/
// TODO
  return sched_stop_scheduling(rmp, proc_nr_n);
}

/*===========================================================================*
 *				do_start_scheduling			     *
 *===========================================================================*/
int do_start_scheduling(message *m_ptr)
{
	register struct schedproc *rmp;
	int rv, proc_nr_n, parent_nr_n;
	
	/* we can handle two kinds of messages here */
	assert(m_ptr->m_type == SCHEDULING_START || 
		m_ptr->m_type == SCHEDULING_INHERIT);

	/* check who can send you requests */
	if (!accept_message(m_ptr))
		return EPERM;

	/* Resolve endpoint to proc slot. */
	if ((rv = sched_isemtyendpt(m_ptr->m_lsys_sched_scheduling_start.endpoint,
			&proc_nr_n)) != OK) {
		return rv;
	}
	rmp = &schedproc[proc_nr_n];

	/* Populate process slot */
	rmp->endpoint     = m_ptr->m_lsys_sched_scheduling_start.endpoint;
	rmp->parent       = m_ptr->m_lsys_sched_scheduling_start.parent;
	rmp->max_priority = m_ptr->m_lsys_sched_scheduling_start.maxprio;
  // TODO
  // Set a new processes deadline to inf.
  rmp->deadline = cur_edf_clock+100000000;
  // Schedule it immediately.
  rmp->waiting_time = 0;
  // Update timeslices.
  read_tsc_64(&(rmp->last_tsc));
  rmp->start_tsc = rmp->last_tsc;

	if (rmp->max_priority >= NR_SCHED_QUEUES) {
		return EINVAL;
	}

	/* Inherit current priority and time slice from parent. Since there
	 * is currently only one scheduler scheduling the whole system, this
	 * value is local and we assert that the parent endpoint is valid */
	if (rmp->endpoint == rmp->parent) {
		/* We have a special case here for init, which is the first
		   process scheduled, and the parent of itself. */
		//rmp->priority   = USER_Q;
		rmp->priority   = DEFAULT_QUEUE;
		rmp->time_slice = DEFAULT_USER_TIME_SLICE;
    // TODO
    //sched_start_scheduling(rmp, 0);

		/*
		 * Since kernel never changes the cpu of a process, all are
		 * started on the BSP and the userspace scheduling hasn't
		 * changed that yet either, we can be sure that BSP is the
		 * processor where the processes run now.
		 */
#ifdef CONFIG_SMP
		rmp->cpu = machine.bsp_id;
		/* FIXME set the cpu mask */
#endif
	}
	
	switch (m_ptr->m_type) {

	case SCHEDULING_START:
		/* We have a special case here for system processes, for which
		 * quanum and priority are set explicitly rather than inherited 
		 * from the parent */
    // TODO
    // We will not set priority to max.
    // Instead we will a default value decided - DEFAULT_QUEUE.
    sched_start_scheduling2(rmp, 0);
		//rmp->priority   = rmp->max_priority;
		rmp->time_slice = m_ptr->m_lsys_sched_scheduling_start.quantum;
		break;
		
	case SCHEDULING_INHERIT:
		/* Inherit current priority and time slice from parent. Since there
		 * is currently only one scheduler scheduling the whole system, this
		 * value is local and we assert that the parent endpoint is valid */
		if ((rv = sched_isokendpt(m_ptr->m_lsys_sched_scheduling_start.parent,
				&parent_nr_n)) != OK)
			return rv;

    // TODO
    sched_start_scheduling3(rmp, 0);
		// rmp->priority = schedproc[parent_nr_n].priority;
		// rmp->time_slice = schedproc[parent_nr_n].time_slice;
		break;
		
	default: 
		assert(0);
	}

	/* Take over scheduling the process. The kernel reply message populates
	 * the processes current priority and its time slice */
	if ((rv = sys_schedctl(0, rmp->endpoint, 0, 0, 0)) != OK) {
		printf("Sched: Error taking over scheduling for %d, kernel said %d\n",
			rmp->endpoint, rv);
		return rv;
	}
	rmp->flags = IN_USE;

	/* Schedule the process, giving it some quantum */
	pick_cpu(rmp);
	while ((rv = schedule_process(rmp, SCHEDULE_CHANGE_ALL)) == EBADCPU) {
		/* don't try this CPU ever again */
		cpu_proc[rmp->cpu] = CPU_DEAD;
		pick_cpu(rmp);
	}

	if (rv != OK) {
		printf("Sched: Error while scheduling process, kernel replied %d\n",
			rv);
		return rv;
	}

	/* Mark ourselves as the new scheduler.
	 * By default, processes are scheduled by the parents scheduler. In case
	 * this scheduler would want to delegate scheduling to another
	 * scheduler, it could do so and then write the endpoint of that
	 * scheduler into the "scheduler" field.
	 */

	m_ptr->m_sched_lsys_scheduling_start.scheduler = SCHED_PROC_NR;

	return OK;
}

/*===========================================================================*
 *				do_nice					     *
 *===========================================================================*/
int do_nice(message *m_ptr)
{
	struct schedproc *rmp;
	int rv;
	int proc_nr_n;
	unsigned new_q, old_q, old_max_q;

	/* check who can send you requests */
	if (!accept_message(m_ptr))
		return EPERM;

	if (sched_isokendpt(m_ptr->m_pm_sched_scheduling_set_nice.endpoint, &proc_nr_n) != OK) {
		printf("SCHED: WARNING: got an invalid endpoint in OoQ msg "
		"%d\n", m_ptr->m_pm_sched_scheduling_set_nice.endpoint);
		return EBADEPT;
	}

	rmp = &schedproc[proc_nr_n];
  // TODO
	new_q = m_ptr->m_pm_sched_scheduling_set_nice.maxprio;
  sched_do_nice1(rmp, m_ptr->m_pm_sched_scheduling_set_nice.maxprio);
	if (new_q >= NR_SCHED_QUEUES) {
		return EINVAL;
	}

	/* Store old values, in case we need to roll back the changes */
	old_q     = rmp->priority;
	old_max_q = rmp->max_priority;
  //sched_do_nice2(rmp, m_ptr->m_pm_sched_scheduling_set_nice.maxprio);

	/* Update the proc entry and reschedule the process */
	rmp->max_priority = rmp->priority = new_q;

	if ((rv = schedule_process_local(rmp)) != OK) {
		/* Something went wrong when rescheduling the process, roll
		 * back the changes to proc struct */
		rmp->priority     = old_q;
		rmp->max_priority = old_max_q;
	}
  //sched_do_nice3(rmp, m_ptr->m_pm_sched_scheduling_set_nice.maxprio);
	return rv;
}

/*===========================================================================*
 *				schedule_process			     *
 *===========================================================================*/
static int schedule_process(struct schedproc * rmp, unsigned flags)
{
	int err;
	int new_prio, new_quantum, new_cpu;

	pick_cpu(rmp);

	if (flags & SCHEDULE_CHANGE_PRIO)
		new_prio = rmp->priority;
	else
		new_prio = -1;

	if (flags & SCHEDULE_CHANGE_QUANTUM)
		new_quantum = rmp->time_slice;
	else
		new_quantum = -1;

	if (flags & SCHEDULE_CHANGE_CPU)
		new_cpu = rmp->cpu;
	else
		new_cpu = -1;

	if ((err = sys_schedule(rmp->endpoint, new_prio,
		new_quantum, new_cpu)) != OK) {
		printf("PM: An error occurred when trying to schedule %d: %d\n",
		rmp->endpoint, err);
	}

	return err;
}


/*===========================================================================*
 *				start_scheduling			     *
 *===========================================================================*/

void init_scheduling(void)
{
	balance_timeout = BALANCE_TIMEOUT * sys_hz();
	init_timer(&sched_timer);
	set_timer(&sched_timer, balance_timeout, balance_queues, 0);
  // TODO
  // Add params to make EDF workflow.
  cur_edf_clock = 0;
  // For measuring time slice.
  u64_t r;
  read_tsc_64(&r);
  srandom((unsigned)r);
}


/*===========================================================================*
 *				balance_queues				     *
 *===========================================================================*/

/* This function in called every 100 ticks to rebalance the queues. The current
 * scheduler bumps processes down one priority when ever they run out of
 * quantum. This function will find all proccesses that have been bumped down,
 * and pulls them back up. This default policy will soon be changed.
 */
static void balance_queues(minix_timer_t *tp)
{
	struct schedproc *rmp;
	int proc_nr;

	for (proc_nr=0, rmp=schedproc; proc_nr < NR_PROCS; proc_nr++, rmp++) {
		if (rmp->flags & IN_USE) {
      // TODO
      sched_balance_queues(rmp,0);
		}
	}

	set_timer(&sched_timer, balance_timeout, balance_queues, 0);
}

int do_setdl(message *m_ptr){
  struct schedproc *rmp;

  /* Standard Sanity check.*/

  /* check who can send you requests */
  if (!accept_message(m_ptr))
    return EPERM;

  /* Get value of proc_nr_n */
  int proc_nr_n;
  if (sched_isokendpt(m_ptr->m2_l1, &proc_nr_n) != OK) {
    printf("SCHED: WARNING: got an invalid endpoint in OOQ msg "
    "%ld\n", m_ptr->m2_l1);
    return EBADEPT;
  }

  rmp = &schedproc[proc_nr_n];
  int old_dl;
  old_dl = rmp->deadline;
  rmp->priority = USER_Q;
  /*Set flags carefully. If they're not defined, set them to SCHED_EDF. */
  rmp->flags = rmp->flags | SCHED_EDF;
  rmp->deadline = m_ptr->m2_l1;

  printf("SCHED: System call do_setdl called with deadline value: %ld\n", m_ptr->m2_l1);
  do_edf();
  return OK;
}

// From do_noquantum. 
// This function has two main operations.
// 1. Every time a process finishes its quantum, we need to decide what we want to do with it.
//    Schedule it again / put it in a different queue etc.
// 2. Everytime a process calls this function, it means that one quantum is complete
//    and hence the global edf time must be increased.

int SAME_sched_noquantum(struct schedproc *rmp, int pid){
  printf("SCHED : Quantum expired in process %d in queue %d\n", pid,rmp->priority);
  u64_t current_tsc;
  read_tsc_64(&current_tsc);
  rmp->waiting_time = rmp->waiting_time+((current_tsc - rmp->last_tsc)-
      (rmp->time_slice*sys_hz()/1000));
  rmp->last_tsc=current_tsc;
  cur_edf_clock += 1;
  rmp->priority=USER_Q; /* Run the same process. Put it back in USER_Q */
  return OK;
}

int NEW_sched_noquantum(struct schedproc *rmp, int pid){
  printf("SCHED : Quantum expired in process %d in queue %d\n", pid,rmp->priority);
  u64_t current_tsc;
  read_tsc_64(&current_tsc);
  rmp->waiting_time = rmp->waiting_time+((current_tsc - rmp->last_tsc)-
      (rmp->time_slice*sys_hz()/1000));
  rmp->last_tsc=current_tsc;
  cur_edf_clock += 1;
  if(sched_type==0)
    return do_edf(); /* Let another process with earliest deadline run.  */
  else{
    /*Non preemptive version, let the same process run.*/
    schedule_process_local(rmp);
  }
  return OK;
}

int sched_stop_scheduling(struct schedproc *rmp, int pid){
  if(rmp->priority>=MAX_USER_Q && rmp->priority<=MIN_USER_Q){
    printf("SCHED : Waiting time of process %d is %llu\n",pid,rmp->waiting_time);
    u64_t current_tsc;
    read_tsc_64(&current_tsc);
    u64_t turn_around_time = ((current_tsc - rmp->start_tsc)-
      (rmp->time_slice*sys_hz()/1000));
    printf("SCHED : Turnaround time of process %d is %llu\n",pid,turn_around_time);
  }
  /*Bring new process in.*/
  return do_edf(); 
}

int sched_start_scheduling(struct schedproc *rmp, int pid){
  /* Give it default prio. Default time slice.*/
  rmp->priority = DEFAULT_QUEUE;
  rmp->time_slice = DEFAULT_USER_TIME_SLICE;
  return OK;
}

int sched_start_scheduling2(struct schedproc *rmp, int pid){
  /* Give it default prio. Also, prio cannot drop below DEFAULT_QUEUE queue.*/
  rmp->priority = DEFAULT_QUEUE;
  rmp->max_priority=DEFAULT_QUEUE;
  return OK;
}

int sched_start_scheduling3(struct schedproc *rmp, int pid){
  /* RT process. Give it prio USER_Q-1. 
   * Process must stay within the user queues.*/
  rmp->priority = (USER_Q)-1;
  rmp->time_slice = 200;
  rmp->max_priority = MAX_USER_Q;
  return OK;
}

int sched_do_nice1(struct schedproc *rmp, int pid){
  if(rmp->priority >=MAX_USER_Q && rmp->priority<=MIN_USER_Q)
    /* Tell the process that it needs to be put into USER_Q because its an RT.*/
    return USER_Q; 
  else{ 
    if (pid < PRIO_MIN || pid > PRIO_MAX) 
      return DEFAULT_QUEUE;

    // Pretty straightforward stuff.
    int new_q_default = (pid-PRIO_MIN) * 2 * DEFAULT_QUEUE / (PRIO_MAX-PRIO_MIN+1);

    /* Non-executable conditions. */
    if ((signed) new_q_default < MAX_USER_Q) new_q_default = MAX_USER_Q;
    if (new_q_default > MIN_USER_Q) new_q_default = MIN_USER_Q;

    return new_q_default;
  }

}

int sched_balance_queues(struct schedproc *rmp, int pid){
  /* If its a non RT process, then you just increase priority (by decreasing queue number) */
  /*Cuz thats how a multi level queue works.*/
  if(rmp->priority > 1 && rmp->priority < MAX_USER_Q){
    rmp->priority -= 1;
    schedule_process_local(rmp);
  }
  return OK;
}

/*===========================================================================*
 *				do_edf					     *
 *===========================================================================*/

int do_edf(){
  struct schedproc *rmp = NULL;
  long max_edf = 10000000;
  int start=-1;
  
  //for(int i=0; i<proc_nr_n; i++)
  // proc_nr_n gives currently executing ones. We want maximum number of processes.
  for(int i=0; i<NR_PROCS; i++){
    rmp = &(schedproc[i]);
    // Flags are set. IN_USE is active. Only then it quualifies for the check.
    if((rmp->flags & IN_USE) && (SCHED_EDF) && (rmp->priority >= MAX_USER_Q)){
      if(rmp->deadline < cur_edf_clock){
        printf("SCHED : Deadline missed for process: %d, hence, not scheduled.", i);
        rmp->priority = MAX_USER_Q -1;
        rmp->max_priority = MAX_USER_Q -1;
      }
      else if(rmp->deadline < max_edf){
        max_edf = rmp->deadline;
        start = i;
      }
    }
  }
  if(start == -1){
    /*No process to be scheduled.*/
    printf("SCHED : No valid process to schedule.");
  }else{
    rmp = &(schedproc[start]);
    schedule_process_local(rmp);
  }
  return OK;
}

/*===========================================================================*
 *				choose_scheduler					     *
 *===========================================================================*/

int choose_scheduler(message *m_ptr){

  // Choose to pass the scheduler type as message argument.
  // 0 - Preemptive, 1 - Non-preemptive.
  sched_type = m_ptr->m1_i1;
  if(sched_type==1){
    printf("SCHED: Scheduler type assigned as Non-preemptive.");
  }else if(sched_type==0){
    printf("SCHED: Scheduler type assigned as Preemptive.");
  }
  return 0;
}


/*===========================================================================*
 *				Setting up Syscall getsystime.					     *
 *===========================================================================*/

int do_getsystime(message *m_ptr){
  m_ptr->m2_l1 = cur_edf_clock;
  return OK;
}
