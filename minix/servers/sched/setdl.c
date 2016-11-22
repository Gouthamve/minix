#include "sched.h"
#include "schedproc.h"
#include <assert.h>
#include <minix/com.h>
#include <machine/archtypes.h>
#include "../../kernel/proc.h" /* for queue constants */

int do_setdl(message *m_ptr){
  struct schedproc *rmp;

  /* Standard Sanity check.*/

  /* check who can send you requests */
  if (!accept_message(m_ptr))
    return EPERM;

  /* Get value of proc_nr_n */
  int proc_nr_n;
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
  do_edf();
  return OK;
}
