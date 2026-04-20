#include "syscall.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"
#include "proc.h"

#include "console.h"

uint64 sys_write(int fd, uint64 va, uint len)
{
	debugf("sys_write fd = %d str = %x, len = %d", fd, va, len);
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

//!!
//so users can type commands in usershell (supports STDIN)
// file descriptor, virtual addr, num of bytes
//CH5.4 - given
uint64 sys_read(int fd, uint64 va, uint64 len)
{	
    //only allow STDIN
    if (fd != STDIN){
        return -1;
    }
    //get curr process
	struct proc *p = curr_proc();
    //create kernel buffer
	char str[MAX_STR_LEN];
    //make sure len doesn't go beyond buffer size
    len = MIN(len, MAX_STR_LEN);
    //read each char of user input
	for (int i = 0; i < len; ++i) {
		int c = consgetc(); //read 1 char from console
		str[i] = c; //store char in kernel buff
	} 
    //copy bytes from kernel buff to user process mem
	copyout(p->pagetable, va, str, len);
	return len;
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

uint64 sys_gettimeofday(uint64 u_va, int _tz)
{
    //get process currently running
    struct proc *p = curr_proc();

    //task1
    //translate user VA into physical addr using process's pg table
    uint64 pa = useraddr(p->pagetable, u_va); //required after enabling VM
    
    //if translation fails (addr isn't mapped or accessible)
    if (pa == 0){
        return -1; //ret error
    }
        

    //put translated phys addr to TimeVal ptr so it's safe to write into user mem
    TimeVal *val = (TimeVal *)pa;

    //get curr CPU cycle count (time source)
    uint64 cycle = get_cycle();

    //conv cycles to sec
    val->sec = cycle / CPU_FREQ;
    //conv leftover cycles to microsecs
    val->usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;

    return 0;
}

uint64 sys_task_info(uint64 u_va) //fills TaskInfo in user mem w/ process status, syscall counts, running time
{
    //get process currently running to read state and pg table
    struct proc *p = curr_proc();

    //transl user VA to phys addr using process's pg table
    uint64 pa = useraddr(p->pagetable, u_va);
    
    //if user addr invalid/unmapped
    if (pa == 0){
        return -1;
    }

    //put to ptr so TaskInfo can be written into user mem
    TaskInfo *ti = (TaskInfo *)pa;

    //kernel local TaskInfo
    //  copy it into user mem after complete
    TaskInfo kinfo;

    //conv process state into TaskStatus enum
    if (p->state == RUNNING) {
        kinfo.status = Running;
    } else if (p->state == RUNNABLE || p->state == SLEEPING) {
        kinfo.status = Ready;
    } else {
        kinfo.status = Exited;
    }

    //copy each syscall count from process taable to TaskInfo struct
    for (int i = 0; i < MAX_SYSCALL_NUM; i++) {
        kinfo.syscall_times[i] = p->syscall_times[i];
    }

    // get current cycle count
    uint64 now = get_cycle();
    if (p->start_time == 0) {
        kinfo.time = 0;
    } else {
        //compute num of cycles since first scheduled
        uint64 diff = now - p->start_time;

        //cycles to ms
        kinfo.time = (int)(diff * 1000 / CPU_FREQ);
    }

    //write TaskInfo from local kernel to translated user mem
    *ti = kinfo;
    return 0;
}

uint64 sys_mmap(uint64 start, uint64 len, int port, int flag, int fd)
{
    struct proc *p = curr_proc();

    //ignore flag and fd for now
    (void)flag;
    (void)fd;

    //if 0, ret success immediately
    if (len == 0){
        return 0;
    }
    //mmap req starting VA to be page-aligned
    if (!PGALIGNED(start)){
        return -1;
    }
    //reject reqs longer than 1 GiB
    if (len > (1ULL << 30)){
        return -1;
    } 
    //unreadable, non-writable non-executable mem is meaningless
    if ((port & ~0x7) != 0){
        return -1;
    }  
    //other bits of port must be 0
    if ((port & 0x7) == 0){
        return -1;
    }
    //avoid overflow in start+len that would make range invalid
    if (start + len < start){
        return -1;
    }

    //round req end addr up to a pg boundary so we map full pgs in the byte range
    uint64 end = PGROUNDUP(start + len);

    // error if any page in [start, end) is already mapped
    for (uint64 va = start; va < end; va += PGSIZE) {
        //walkaddr ret nonzero if user virtual pg is alr mapped
        if (walkaddr(p->pagetable, va) != 0){
            return -1;
        }
    }

    //page table perm bits
    //PTE_U required to user mode can access pages
    //  Ch4.5 def: PTE_R (1L<<1), PTE_W (1L<<2), PTE_X (1L<<3), PTE_U (1L<<4)
    int perm = PTE_U;

    //if port bit 0 is set, add read perm
    if (port & 0x1){
        perm |= PTE_R;
    }
    //if port bit 1 is set, add write perm
    if (port & 0x2){
        perm |= PTE_W;
    }
    //if port bit 2 is set, add exec perm
    if (port & 0x4){
        perm |= PTE_X;
    }
        
    //allocate and map one physical pg at a time
    for (uint64 va = start; va < end; va += PGSIZE) {
        //get 1 free phys pg from kernel alloc
        void *mem = kalloc();
        if (mem == 0){
            return -1;
        }

        //clear pg to new mapping has zeroed memory
        memset(mem, 0, PGSIZE);

        //mapping from user virtual pg to alloc phys page w/ perm bits
        if (mappages(p->pagetable, va, PGSIZE, (uint64)mem, perm) != 0){ //ch4.5: "mappages creates a mapping from [va, va+size) to [pa, pa+size)" 
            return -1;
        }
    }

    return 0; //all pages mapped successfully
}

//removes a user VM mapping and frees phys pages
//start: starting user VA of region to unmap
//len: num of bytes to unmap
uint64 sys_munmap(uint64 start, uint64 len)
{
    //get process currently running to get its pg table
    struct proc *p = curr_proc();

    //if len is 0, ret success immediately
    if (len == 0){
        return 0;
    }
    //munmap req starting addr to be pg aligned
    if (!PGALIGNED(start)){
        return -1;
    }
    //reject req over 1 GiB
    if (len > (1ULL << 30)){
        return -1;
    }
    
    //round end of range up to a pg boundary to unmap each one
    uint64 end = PGROUNDUP(start + len);

    // error if any page in [start, end) is not mapped
    for (uint64 va = start; va < end; va += PGSIZE) { //Ch4.2: ""kalloc does not support contiguous physical memory allocation"
        //ret 0 if virtual pg is not mapped
        if (walkaddr(p->pagetable, va) == 0){
            return -1;
        }
    }

    //remove mappings for all pgs
    //(1 at end is to free phys mem behind the mappings)
    uvmunmap(p->pagetable, start, (end - start) / PGSIZE, 1); //get # of pages to unmap
    //  Ch4.5: "uvmunmap unmaps a section of the mapping, do_free controls whether to kfree the corresponding physical memory"
    return 0;
}

//every process has a unique pid
uint64 sys_getpid()
{
	return curr_proc()->pid;
}

//tracks who created 
uint64 sys_getppid()
{
	struct proc *p = curr_proc();
    //check if no par
	if (p->parent == NULL) {
        return IDLE_PID;
    } else {
        return p->parent->pid; //par process ID
    }
}

//map syscall num to kernel's fork()
uint64 sys_clone()
{
	return fork();
}

uint64 sys_exec(uint64 va)
{
	struct proc *p = curr_proc();
	char name[256];
    //copy filename str from user mem to ker buff
	if (copyinstr(p->pagetable, name, va, sizeof(name)) < 0){
        return -1;
    }
		
	return exec(name);
}

//waiting on child
//  pid: child to wait for, va: user addr where exit code should be stored
uint64 sys_wait(int pid, uint64 va)
{
	struct proc *p = curr_proc();

	// int *code = (int *)useraddr(p->pagetable, va);
	// return wait(pid, code);

    int *code = 0; //init status ptr

    //CH5: if the status addr is 0, there's no need to save the child exit code
    if (va != 0) {
        //transl user VA to kernel phys addr
        code = (int *)useraddr(p->pagetable, va);
        
        //if user addr is invalid
        if (code == 0){
            return -1;
        }
    }

    return wait(pid, code);
}

//syscall helper for spawn
//  va: virtual addr of filename
uint64 sys_spawn(uint64 va)
{
	// TODO: your job is to complete the sys call
	//curr proc for addr transl
    struct proc *p = curr_proc();
    char name[256];

    //copy filename str from user mem
    if (copyinstr(p->pagetable, name, va, sizeof(name)) < 0) {
        return -1;
    }

    return spawn(name);
}

uint64 sys_set_priority(long long prio){
    // TODO: your job is to complete the sys call
    //forwards request to kernel helper 
    return set_priority(prio);
}


extern char trap_page[];

void syscall()
{
    //get current process's trapframe (holds syscall args and ret val)
	struct trapframe *trapframe = curr_proc()->trapframe;
    //syscall ID is stored in register a7
	int id = trapframe->a7, ret;

    //extract up to 6 syscall args from a0-a5
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
    //debug (show syscall nums and args)
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
    //write syscall
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
    //read syscall
    case SYS_read:
        ret = sys_read(args[0], args[1], args[2]);
        break;
    //exit syscall (doesn't ret)
	case SYS_exit:
		sys_exit(args[0]);
		// __builtin_unreachable();
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
	/*
	* LAB1: you may need to add SYS_taskinfo case here
	*/
	case SYS_gettimeofday:
        //old:ret = sys_gettimeofday((TimeVal *)args[0], args[1]);
        ret = sys_gettimeofday(args[0], args[1]);
        //args[0] is now treated as a user VA not a direct ptr
    	break;
	case SYS_task_info:
        //old: ret = sys_task_info((TaskInfo *)args[0]);
        ret = sys_task_info(args[0]);
        //pass user VA (transl happens inside sys_task_info)
        break;
    case SYS_mmap:
        //fwd all params directly to sys_mmap
        ret = sys_mmap(args[0], args[1], args[2], args[3], args[4]);
        break;
    case SYS_munmap:
        //fwd params to sys_mmap
        ret = sys_munmap(args[0], args[1]);
        break;
    //lets user code ask for pid
    case SYS_getpid:
		ret = sys_getpid();
		break;
    //lets user code ask for par pid
	case SYS_getppid:
		ret = sys_getppid();
		break;
    //makes user-lvl fork reach ker
	case SYS_clone:
		ret = sys_clone();
		break;
    //makes user-lvl exec reach ker
	case SYS_execve:
		ret = sys_exec(args[0]);
		break;
	case SYS_wait4:
		ret = sys_wait(args[0], args[1]);
		break;
	case SYS_spawn:
		ret = sys_spawn(args[0]);
		break;
    //makes user-lvl priority change reach ker
    case SYS_setpriority:
        ret = sys_set_priority((long long)args[0]);
        break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
    //store ret val back into a0
	trapframe->a0 = ret;
    //debug print ret val
	tracef("syscall ret %d", ret);
}