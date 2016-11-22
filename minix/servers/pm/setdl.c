#include <stdio.h>
#include "pm.h"         // provides global variables such as m_in

int do_setdl(){
  int dl = m_in.m1_i1; 
  struct schedproc *rmp;
  int proc_nr_n;

  /* check who can send you requests */
  if (!accept_message(m_ptr))
    return EPERM;

  if (sched_isokendpt(m_ptr->SCHEDULING_ENDPOINT, &proc_nr_n) != OK) {
    printf("SCHED: WARNING: got an invalid endpoint in OOQ msg "
    "%ld\n", m_ptr->SCHEDULING_ENDPOINT);
    return EBADEPT;
  }

  rmp = &schedproc[proc_nr_n];
  int old_dl;
  old_ld = rmp->deadline;
  rmp->priority = USER_Q;
  /*Set flags carefully. If they're not defined, set them to SCHED_EDF. */
  rmp->flags = rmp->flags | SCHED_EDF;
  rmp->deadline = m_ptr->m2_l1;

  printf("SCHED: System call do_setdl called with deadline value: %d\n", dl);
  return 0;
}
