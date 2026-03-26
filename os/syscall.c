#include "syscall.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"
#include "proc.h"

//writes user input to stdout
uint64 sys_write(int fd, uint64 va, uint len)
{
    //print the debug info (file desc, virtual addr, and req len)
    debugf("sys_write fd = %d va = %x, len = %d", fd, va, len);
    //give error for non stdout
    if (fd != STDOUT){
        return -1;
    }
        
    //get running process that's currently running to open its pg table
    struct proc *p = curr_proc();
    
    //make a kernel buffer (holds copied input string) 
    char str[MAX_STR_LEN];

    //copies null-term str from user VM to kernel buffer
    //  p->pagetable tells copyinstr which user pg table to use
    //  va is user VA that was passed into syscall
    //  MIN(len, MAX_STR_LEN) caps how much can be copied so it's not more than the buffer can hold
    int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN)); //ch 4.5 (use copyin/out interfaces)
    if (size < 0){
        return -1; //for if copyinstr has an error
    }

    //shows how many chars were copied
    debugf("size = %d", size);

    //write each copied char to console individually
    for (int i = 0; i < size; ++i) {
        console_putchar(str[i]);
    }

    //# of bytes written
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
//u_va = user virtual addr
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

// TODO: add support for mmap and munmap syscall.
// hint: read through docstrings in vm.c. Watching CH4 video may also help.
// Note the return value and PTE flags (especially U,X,W,R)
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

//start: start addr of VM to be mapped
//len: len of mapped byte (if 0, ret directly). upper limit 1GiB
//port: bit 0 = if it's readable, 1 = if it's writeable, 2 = if it's executable
//     other bits invalid
//flag: always 0
//fd: always 0
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
    break;
    //unknown syscall
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
    //store ret val back into a0
	trapframe->a0 = ret;
    //debug print ret val
	tracef("syscall ret %d", ret);
}
