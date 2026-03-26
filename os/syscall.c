#include "syscall.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"
#include "proc.h" //added

uint64 sys_write(int fd, uint64 va, uint len)
{
    debugf("sys_write fd = %d va = %x, len = %d", fd, va, len);
    if (fd != STDOUT)
        return -1;
	

    struct proc *p = curr_proc();
    char str[MAX_STR_LEN];
    int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
    debugf("size = %d", size);

    for (int i = 0; i < size; ++i) {
        console_putchar(str[i]);
    }
    return size;
}



__attribute__((noreturn)) void sys_exit(int code)
{
	exit(code);
	__builtin_unreachable();
}

uint64 sys_sched_yield()
{
	yield();
	return 0;
}
// old: uint64 sys_gettimeofday(TimeVal *val, int _tz) // TODO: implement sys_gettimeofday in pagetable. (Virtual Addr to Physical Addr)
uint64 sys_gettimeofday(uint64 uaddr, int _tz)
{
    struct proc *p = curr_proc();

    uint64 pa = useraddr(p->pagetable, uaddr);
    if (pa == 0)
        return -1;

    TimeVal *val = (TimeVal *)pa;
    uint64 cycle = get_cycle();
    val->sec = cycle / CPU_FREQ;
    val->usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;

    return 0;
}
	/* The code in `ch3` will leads to memory bugs*/

	// uint64 cycle = get_cycle();
	// val->sec = cycle / CPU_FREQ;
	// val->usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
//}

// TODO: add support for mmap and munmap syscall.
// hint: read through docstrings in vm.c. Watching CH4 video may also help.
// Note the return value and PTE flags (especially U,X,W,R)
uint64 sys_task_info(uint64 uaddr)
{
    struct proc *p = curr_proc();

    uint64 pa = useraddr(p->pagetable, uaddr);
    if (pa == 0)
        return -1;

    TaskInfo *ti = (TaskInfo *)pa;
    TaskInfo kinfo;

    if (p->state == RUNNING) {
        kinfo.status = Running;
    } else if (p->state == RUNNABLE || p->state == SLEEPING) {
        kinfo.status = Ready;
    } else {
        kinfo.status = Exited;
    }

    for (int i = 0; i < MAX_SYSCALL_NUM; i++) {
        kinfo.syscall_times[i] = p->syscall_times[i];
    }

    uint64 now = get_cycle();
    if (p->start_time == 0) {
        kinfo.time = 0;
    } else {
        uint64 diff = now - p->start_time;
        kinfo.time = (int)(diff * 1000 / CPU_FREQ);
    }

    *ti = kinfo;
    return 0;
}
//old:
/*
* LAB1: you may need to define sys_task_info here
*/
// uint64 sys_task_info(TaskInfo *ti) {
//     struct proc *p = curr_proc();
//     TaskInfo kinfo;
//     if (p->state == RUNNING) {
//         kinfo.status = Running;
//     } else if (p->state == RUNNABLE) {
//         kinfo.status = Ready;
//     } else if (p->state == SLEEPING) {
//         kinfo.status = Ready;
//     } else {
//         kinfo.status = Exited;
//     }
//     for (int i = 0; i < MAX_SYSCALL_NUM; i++) {
//         kinfo.syscall_times[i] = p->syscall_times[i];
//     }
//     uint64 now = get_cycle();
//     if (p->start_time == 0) {
//         kinfo.time = 0;
//     } else {
//         uint64 diff = now - p->start_time;
//         kinfo.time = (int)(diff * 1000 / CPU_FREQ);
//     }
//     memmove(ti, &kinfo, sizeof(TaskInfo));
//     return 0;
// }


uint64 sys_mmap(uint64 start, uint64 len, int port, int flag, int fd)
{
    struct proc *p = curr_proc();

    (void)flag;
    (void)fd;

    if (len == 0)
        return 0;

    if (!PGALIGNED(start))
        return -1;

    if (len > (1ULL << 30))
        return -1;

    if ((port & ~0x7) != 0)
        return -1;

    if ((port & 0x7) == 0)
        return -1;

    if (start + len < start)
        return -1;
    uint64 end = PGROUNDUP(start + len);

    // error if any page in [start, end) is already mapped
    for (uint64 va = start; va < end; va += PGSIZE) {
        if (walkaddr(p->pagetable, va) != 0)
            return -1;
    }

    int perm = PTE_U;
    if (port & 0x1)
        perm |= PTE_R;
    if (port & 0x2)
        perm |= PTE_W;
    if (port & 0x4)
        perm |= PTE_X;

    for (uint64 va = start; va < end; va += PGSIZE) {
        void *mem = kalloc();
        if (mem == 0)
            return -1;

        memset(mem, 0, PGSIZE);

        if (mappages(p->pagetable, va, PGSIZE, (uint64)mem, perm) != 0)
            return -1;
    }

    return 0;
}

uint64 sys_munmap(uint64 start, uint64 len)
{
    struct proc *p = curr_proc();

    if (len == 0)
        return 0;

    if (!PGALIGNED(start))
        return -1;

    if (len > (1ULL << 30))
        return -1;

    uint64 end = PGROUNDUP(start + len);

    // error if any page in [start, end) is not mapped
    for (uint64 va = start; va < end; va += PGSIZE) {
        if (walkaddr(p->pagetable, va) == 0)
            return -1;
    }

    uvmunmap(p->pagetable, start, (end - start) / PGSIZE, 1);
    return 0;
}

extern char trap_page[];

void syscall()
{
	struct trapframe *trapframe = curr_proc()->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]", id, args[0],
	       args[1], args[2], args[3], args[4], args[5]);
	/*
	* LAB1: you may need to update syscall counter for task info here
	*/
	struct proc *p = curr_proc();
    if (id >= 0 && id < MAX_SYSCALL_NUM) {
        p->syscall_times[id]++;
    }   
	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
		// __builtin_unreachable();
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
	/*
	* LAB1: you may need to add SYS_taskinfo case here
	*/
	//old:
	// case SYS_gettimeofday:
	// 	ret = sys_gettimeofday((TimeVal *)args[0], args[1]);
	// 	break;
	case SYS_gettimeofday:
    	ret = sys_gettimeofday(args[0], args[1]);
    	break;

	// case SYS_task_info:
    //     ret = sys_task_info((TaskInfo *)args[0]);
    //     break;

	case SYS_task_info:
        ret = sys_task_info(args[0]);
        break;
    case SYS_mmap:
        ret = sys_mmap(args[0], args[1], args[2], args[3], args[4]);
        break;
    case SYS_munmap:
        ret = sys_munmap(args[0], args[1]);
        break;
    break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}
