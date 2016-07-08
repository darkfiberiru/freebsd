#ifndef __LINUX_PREEMPT_H
#define __LINUX_PREEMPT_H

#include <linux/linkage.h>
#include <linux/list.h>

#define in_interrupt() 	(curthread->td_intr_nesting_level)

#endif
