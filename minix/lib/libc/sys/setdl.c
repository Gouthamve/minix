#include <sys/cdefs.h>
#include "namespace.h"
#include <errno.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <lib.h>
#include <unistd.h>
#include <string.h>
#include <stddef.h>


int set_deadline(long deadline)
{
	int v;
	message m;

	memset(&m, 0, sizeof(m));
	m.m_lc_pm_priority.deadline = deadline;

	return _syscall(PM_PROC_NR, PM_SETDL, &m);
}


