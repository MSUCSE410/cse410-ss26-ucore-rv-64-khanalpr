#include "syscall.h"
#include "console.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"

uint64 console_write(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	tracef("write size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return len;
}

uint64 console_read(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	tracef("read size = %d", len);
	for (int i = 0; i < len; ++i) {
		int c = consgetc();
		str[i] = c;
	}
	copyout(p->pagetable, va, str, len);
	return len;
}

uint64 sys_write(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_write(va, len);
	case FD_INODE:
		return inodewrite(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
}

uint64 sys_read(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_read(va, len);
	case FD_INODE:
		return inoderead(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
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


uint64 sys_gettimeofday(uint64 val, int _tz)
{
	// struct proc *p = curr_proc();
	// uint64 cycle = get_cycle();
	// TimeVal t;
	// t.sec = cycle / CPU_FREQ;
	// t.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	// copyout(p->pagetable, val, (char *)&t, sizeof(TimeVal));
	// return 0;

	// My Changes (from project 2)
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
}

// My Changes (from project 2)
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

// My Changes (from project 2)
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

// My changes (from project 2)
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


uint64 sys_getpid()
{
	return curr_proc()->pid;
}

uint64 sys_getppid()
{
	struct proc *p = curr_proc();
	return p->parent == NULL ? IDLE_PID : p->parent->pid;
}

uint64 sys_clone()
{
	debugf("fork!");
	return fork();
}

static inline uint64 fetchaddr(pagetable_t pagetable, uint64 va)
{
	uint64 *addr = (uint64 *)useraddr(pagetable, va);
	return *addr;
}

uint64 sys_exec(uint64 path, uint64 uargv)
{
	struct proc *p = curr_proc();
	char name[MAX_STR_LEN];
	copyinstr(p->pagetable, name, path, MAX_STR_LEN);
	uint64 arg;
	static char strpool[MAX_ARG_NUM][MAX_STR_LEN];
	char *argv[MAX_ARG_NUM];
	int i;
	for (i = 0; uargv && (arg = fetchaddr(p->pagetable, uargv));
	     uargv += sizeof(char *), i++) {
		copyinstr(p->pagetable, (char *)strpool[i], arg, MAX_STR_LEN);
		argv[i] = (char *)strpool[i];
	}
	argv[i] = NULL;
	return exec(name, (char **)argv);
}

uint64 sys_wait(int pid, uint64 va)
{
	struct proc *p = curr_proc();
	int *code = (int *)useraddr(p->pagetable, va);
	return wait(pid, code);
}

uint64 sys_spawn(uint64 va)
{
	// TODO: your job is to complete the sys call
	// return -1;
	// Project 3 My Changes
	struct proc *p = curr_proc();
	char name[200];
	// copying the filename string from user space
	copyinstr(p->pagetable, name, va, 200);
	debugf("sys_spawn %s\n", name);
	return spawn(name);
}

uint64 sys_set_priority(long long prio)
{
	// TODO: your job is to complete the sys call
	// return -1;
	// just delegating to the proc-layer function
	return set_priority(prio);
}

uint64 sys_openat(uint64 va, uint64 omode, uint64 _flags)
{
	struct proc *p = curr_proc();
	char path[200];
	copyinstr(p->pagetable, path, va, 200);
	return fileopen(path, omode);
}

uint64 sys_close(int fd)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d", fd);
		return -1;
	}
	fileclose(f);
	p->files[fd] = 0;
	return 0;
}


// Project 4 - my changes
// sys_fstat - fill in a Stat struct for an open file descriptor
// syscall id 80
int sys_fstat(int fd,uint64 stat){
	//TODO: your job is to complete the syscall
	// Project 4 My Changes
	struct proc *p = curr_proc();

	// validating fd range first
	if (fd < 0 || fd >= FD_BUFFER_SIZE) {
		errorf("sys_fstat: bad fd %d", fd);
		return -1;
	}

	struct file *f = p->files[fd];
	if (f == NULL || f->type != FD_INODE) {
		errorf("sys_fstat: fd %d not open or not an inode", fd);
		return -1;
	}

	// translating the user virtual address to a kernel-accessible one
	Stat *kstat = (Stat *)useraddr(p->pagetable, stat);
	if (kstat == 0) {
		errorf("sys_fstat: bad stat address");
		return -1;
	}

	// making sure we have fresh data from disk
	ivalid(f->ip);

	kstat->dev   = 0; // we only have one device, hardcoding 0
	kstat->ino   = f->ip->inum;
	kstat->nlink = f->ip->nlink;

	// mapping our internal T_FILE/T_DIR to the values the test expects
	if (f->ip->type == T_FILE)
		kstat->mode = STAT_FILE;
	else if (f->ip->type == T_DIR)
		kstat->mode = STAT_DIR;
	else
		kstat->mode = 0;

	// zero the padding, tests might check it
	for (int idx = 0; idx < 7; idx++)
		kstat->pad[idx] = 0;

	return 0;
}

// Project 4 - my changes
// sys_linkat - creating a hard link (newpath) pointing to the same inode as oldpath
// syscall id 37
// olddirfd/newdirfd/flags are always AT_FDCWD/0 in this lab, so we ignore them
int sys_linkat(int olddirfd, uint64 oldpath, int newdirfd, uint64 newpath, uint64 flags){
	//TODO: your job is to complete the syscall
	// Project 4 My Changes
	struct proc *p = curr_proc();

	// copying both path strings out of user memory
	char old_name[DIRSIZ + 1];
	char new_name[DIRSIZ + 1];

	if (copyinstr(p->pagetable, old_name, oldpath, DIRSIZ + 1) < 0) {
		errorf("sys_linkat: bad oldpath");
		return -1;
	}
	if (copyinstr(p->pagetable, new_name, newpath, DIRSIZ + 1) < 0) {
		errorf("sys_linkat: bad newpath");
		return -1;
	}

	// linking a file to itself is an error per spec
	if (strncmp(old_name, new_name, DIRSIZ) == 0) {
		errorf("sys_linkat: old and new name are the same");
		return -1;
	}

	// look up the source inode
	struct inode *src = namei(old_name);
	if (src == 0) {
		errorf("sys_linkat: source file '%s' not found", old_name);
		return -1;
	}
	ivalid(src);

	// not able to hard-link a directory (would mess up the tree structure)
	if (src->type == T_DIR) {
		iput(src);
		errorf("sys_linkat: cannot hard link a directory");
		return -1;
	}

	// grabing root dir to write the new dirent into
	struct inode *dp = root_dir();
	ivalid(dp);

	// dirlink checks for duplicate name internally and returns -1 if found
	if (dirlink(dp, new_name, src->inum) < 0) {
		iput(dp);
		iput(src);
		errorf("sys_linkat: dirlink failed (name collision?)");
		return -1;
	}

	// bumping the link count and flush to disk
	src->nlink++;
	iupdate(src);

	iput(dp);
	iput(src);
	return 0;
}

// Project 4 - my changes
// sys_unlinkat - remove a directory entry; delete the file if nlink drops to 0
// syscall id 35
// dirfd and flags are always AT_FDCWD/0 in this lab, we ignore them
int sys_unlinkat(int dirfd, uint64 name, uint64 flags){
	//TODO: your job is to complete the syscall
	struct proc *p = curr_proc();

	char fname[DIRSIZ + 1];
	if (copyinstr(p->pagetable, fname, name, DIRSIZ + 1) < 0) {
		errorf("sys_unlinkat: bad name pointer");
		return -1;
	}

	// find the inode this name points to
	struct inode *ip = namei(fname);
	if (ip == 0) {
		errorf("sys_unlinkat: file '%s' not found", fname);
		return -1;
	}
	ivalid(ip);

	struct inode *dp = root_dir();
	ivalid(dp);

	// remove the dirent from the directory
	if (dirunlink(dp, fname) < 0) {
		// shouldnt really happen since namei found it, but be safe
		iput(dp);
		iput(ip);
		errorf("sys_unlinkat: dirunlink failed");
		return -1;
	}

	iput(dp);

	// decrement link count and persist it
	ip->nlink--;
	iupdate(ip);

	// iput will call itrunc + free the inode if nlink==0 and ref drops to 0
	iput(ip);
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
	// My Changes (from project 1)	   
	if (id >= 0 && id < 500) {
		curr_proc()->syscall_times[id]++;
	}	
	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_read:
		ret = sys_read(args[0], args[1], args[2]);
		break;
	case SYS_openat:
		ret = sys_openat(args[0], args[1], args[2]);
		break;
	case SYS_close:
		ret = sys_close(args[0]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
		// __builtin_unreachable();
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
	case SYS_gettimeofday:
		ret = sys_gettimeofday(args[0], args[1]);
		break;
	case SYS_getpid:
		ret = sys_getpid();
		break;
	case SYS_getppid:
		ret = sys_getppid();
		break;
	case SYS_clone: // SYS_fork
		ret = sys_clone();
		break;
	case SYS_execve:
		ret = sys_exec(args[0], args[1]);
		break;
	case SYS_wait4:
		ret = sys_wait(args[0], args[1]);
		break;
	case SYS_fstat:
	    ret = sys_fstat(args[0],args[1]);
		break;
	case SYS_linkat:
	    ret = sys_linkat(args[0],args[1],args[2],args[3],args[4]);
		break;
	case SYS_unlinkat:
	    ret = sys_unlinkat(args[0],args[1],args[2]);
		// Project 4 my changes
		break; 
	case SYS_spawn:
		ret = sys_spawn(args[0]);
		break;
	// My Changes (from project 1)	
	case SYS_task_info:
		ret = sys_task_info((TaskInfo *)args[0]);
		break;	
	// My Changes from Project 2
	case SYS_mmap:
    	ret = sys_mmap(args[0], args[1], (int)args[2],
                   (int)args[3], (int)args[4]);
    	break;
	case SYS_munmap:
    	ret = sys_munmap(args[0], args[1]);
    	break;
		// Project 3 My Changes	
	case SYS_setpriority:
		ret = sys_set_priority((long long)args[0]);
		break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}
