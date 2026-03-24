#include "syscall.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"

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

uint64 sys_gettimeofday(TimeVal *val, int _tz) // TODO: implement sys_gettimeofday in pagetable. (VA to PA)
{
	// My changes
	// Get current process's page table
    struct proc *p = curr_proc();

	// Translating virtual address -> physical address using the process's page table.
    // val is a VIRTUAL address from user space.
    // useraddr() walks the page table: VA → physical page → physical address
    TimeVal *physical_val = (TimeVal *)useraddr(p->pagetable, (uint64)val);

    if (physical_val == 0)
        return -1;

    uint64 cycle = get_cycle();
    physical_val->sec  = cycle / CPU_FREQ;
    physical_val->usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;

    return 0;

	/* The code in `ch3` will leads to memory bugs*/

	// uint64 cycle = get_cycle();
	// val->sec = cycle / CPU_FREQ;
	// val->usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	// return 0;
}

// TODO: add support for mmap and munmap syscall.
// hint: read through docstrings in vm.c. Watching CH4 video may also help.
// Note the return value and PTE flags (especially U,X,W,R)
/*
* LAB1: you may need to define sys_task_info here
*/
int sys_task_info(TaskInfo *ti)
{
	struct proc *p = curr_proc();
	// ti->status = TaskRunning;
	// int now_ms = get_cycle() * 1000 / CPU_FREQ;
	// ti->time = now_ms - p->start_time;
	// for (int i = 0; i < MAX_SYSCALL_NUM; i++) {
	// 	ti->syscall_times[i] = p->syscall_times[i];
	// }
	// return 0;

	// My changes
	TaskInfo *physical_ti = (TaskInfo *)useraddr(p->pagetable, (uint64)ti);

    if (physical_ti == 0)
        return -1;

    physical_ti->status = TaskRunning;
    int now_ms = get_cycle() * 1000 / CPU_FREQ;
    physical_ti->time = now_ms - p->start_time;

    for (int i = 0; i < MAX_SYSCALL_NUM; i++) {
        physical_ti->syscall_times[i] = p->syscall_times[i];
    }

    return 0;
}

extern char trap_page[];

// My changes
uint64 sys_mmap(uint64 start, uint64 len, int port, int flag, int fd)
{
    // #1 Validation

    // len == 0 is valid, we return success immediately
    if (len == 0)
        return 0;

    // if len exceeds 1 GiB limit
    if (len > (1 << 30))
        return -1;

    // must be page-aligned (PGSIZE = 4096)
    // If start % 4096 != 0, it's not aligned, error
    if (start % PGSIZE != 0)
        return -1;

    // port's upper bits (above bit 2) must all be 0
    // port & ~0x7 checks if any bit above bit2 is set
    if (port & ~0x7)
        return -1;

    // port's lower 3 bits can't ALL be 0
    // why, coz it would mean no read/write/execute meaningless mapping
    if ((port & 0x7) == 0)
        return -1;

    struct proc *p = curr_proc();

    // Rounding len UP to nearest page boundary
    // e.g., len=5000 → PGROUNDUP(5000) = 8192 (two 4096-byte pages)
    uint64 len_aligned = PGROUNDUP(len);

    // #2 Check no page in range is already mapped
    // walkaddr returns 0 if the VA has no mapping, non-zero if it does
    for (uint64 va = start; va < start + len_aligned; va += PGSIZE) {
        if (walkaddr(p->pagetable, va) != 0) {
            return -1;  // found an already-mapped pag, so error
        }
    }

    // #3 Converting port bits to RISC-V PTE permission flags 
    //
    // port layout:   bit2=X  bit1=W  bit0=R
    // PTE flag bits: PTE_R=(1<<1), PTE_W=(1<<2), PTE_X=(1<<3), PTE_U=(1<<4)
    //
    // Settin PTE_U, coz without it, the CPU refuses user-mode access
    // (gets a page fault immediately when user code touches the memory)
    int perm = PTE_U;
    if (port & 0x1) perm |= PTE_R;   // bit0 set → readable
    if (port & 0x2) perm |= PTE_W;   // bit1 set → writable
    if (port & 0x4) perm |= PTE_X;   // bit2 set → executable

    // #4 Allocating +  maping one page at a time
    for (uint64 va = start; va < start + len_aligned; va += PGSIZE) {
        void *pa = kalloc();
        if (pa == 0) {
            // Out of physical memory
            return -1;
        }

        // Zero the new page (prevents leaking old kernel data to user)
        memset(pa, 0, PGSIZE);

        // mappages(pagetable, va, size, pa, permissions)
        // Creates PTE: virtual address va → physical address pa
        if (mappages(p->pagetable, va, PGSIZE, (uint64)pa, perm) != 0) {
            kfree(pa);
            return -1;
        }
    }

    return 0;
}

// My changes
uint64 sys_munmap(uint64 start, uint64 len)
{
    // #1 Validation

    if (len == 0)
        return 0;

    // start must be page-aligned
    if (start % PGSIZE != 0)
        return -1;

    struct proc *p = curr_proc();

    uint64 len_aligned = PGROUNDUP(len);

    //#2 Verifying ALL pages in range are actually mapped
    for (uint64 va = start; va < start + len_aligned; va += PGSIZE) {
        if (walkaddr(p->pagetable, va) == 0) {
            return -1;  // found an unmapped page → error
        }
    }

    //#3 Unmapping all pages
    uvmunmap(p->pagetable, start, len_aligned / PGSIZE, 1);

    return 0;
}

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
	if (id >= 0 && id < 500) {
		curr_proc()->syscall_times[id]++;
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
	case SYS_gettimeofday:
		ret = sys_gettimeofday((TimeVal *)args[0], args[1]);
		break;
	/*
	* LAB1: you may need to add SYS_taskinfo case here
	*/
	case SYS_task_info:
		ret = sys_task_info((TaskInfo *)args[0]);
		break;
	// My changes
	// Project 2
	case SYS_mmap:
    	ret = sys_mmap(args[0], args[1], (int)args[2],
                   (int)args[3], (int)args[4]);
    	break;
	case SYS_munmap:
    	ret = sys_munmap(args[0], args[1]);
    	break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}
