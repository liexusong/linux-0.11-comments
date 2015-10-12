/*
 *  linux/kernel/fork.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  'fork.c' contains the help-routines for the 'fork' system call
 * (see also system_call.s), and some misc functions ('verify_area').
 * Fork is rather simple, once you get the hang of it, but the memory
 * management can be a bitch. See 'mm/mm.c': 'copy_page_tables()'
 */
#include <errno.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>
#include <asm/system.h>

extern void write_verify(unsigned long address);

long last_pid=0;

/**
 * 验证地址是否合法
 * @addr: 开始地址
 * @size: 内存大小
 */
void verify_area(void * addr,int size)
{
	unsigned long start;

	start = (unsigned long) addr;
	size += start & 0xfff;  // 真实要验证的内存大小
	start &= 0xfffff000;    // 真实的开始内存地址
	start += get_base(current->ldt[2]); /* 加上局部数据段的基地址(线性地址) */
	while (size>0) {
		size -= 4096;
		write_verify(start);
		start += 4096;
	}
}

int copy_mem(int nr,struct task_struct * p)
{
	unsigned long old_data_base,new_data_base,data_limit;
	unsigned long old_code_base,new_code_base,code_limit;

	code_limit=get_limit(0x0f); // 代码段长度限制(LDT中)
	data_limit=get_limit(0x17); // 数据段长度限制(LDT中)
	old_code_base = get_base(current->ldt[1]); // 被复制的进程代码段起始位置
	old_data_base = get_base(current->ldt[2]); // 被复制的进程数据段起始位置
	if (old_data_base != old_code_base)
		panic("We don't support separate I&D");
	if (data_limit < code_limit)
		panic("Bad data_limit");
	// 新进程代码段和数据段的内存起始位置
	// nr是进程号, 进程的数据段和代码段开始地址为(nr * 0x4000000)
	new_data_base = new_code_base = nr * 0x4000000;
	p->start_code = new_code_base;
	// 设置ldt
	set_base(p->ldt[1],new_code_base);
	set_base(p->ldt[2],new_data_base);
	// 新进程映射到到旧进程的内存地址
	if (copy_page_tables(old_data_base,new_data_base,data_limit)) {
		printk("free_page_tables: from copy_mem\n");
		free_page_tables(new_data_base,data_limit);
		return -ENOMEM;
	}
	return 0;
}

/*
 *  Ok, this is the main fork-routine. It copies the system process
 * information (task[nr]) and sets up the necessary registers. It
 * also copies the data segment in it's entirety.
 */
int copy_process(
		/* 下面参数由sys_fork()提供 */
		int nr, long ebp, long edi, long esi, long gs,
		/* 下面参数由system_call()中断提供 */
		long none, long ebx, long ecx, long edx,
		long fs, long es, long ds,
		/* 由中断发生时自动压栈 */
		long eip, long cs, long eflags, long esp, long ss)
{
	struct task_struct *p;
	int i;
	struct file *f;

	// 申请一个空白页(返回物理地址), 用于保存进程描述符
	p = (struct task_struct *) get_free_page();
	if (!p)
		return -EAGAIN;
	task[nr] = p;
	// 完全复制父进程的所有字段(ldt也在这里被复制, 在copy_mem的时候设置ldt的基地址)
	*p = *current;	/* NOTE! this doesn't copy the supervisor stack */
	p->state = TASK_UNINTERRUPTIBLE;
	p->pid = last_pid;  // 设置进程pid
	p->father = current->pid;  // 父进程pid
	p->counter = p->priority;  // CPU可用时间片
	p->signal = 0;  // 信号位图
	p->alarm = 0;   // 时钟定时器
	p->leader = 0;		/* process leadership doesn't inherit */
	p->utime = p->stime = 0;   // 内核态和用户态运行的时间
	p->cutime = p->cstime = 0;
	p->start_time = jiffies;  // 进程创建的时间
	// 设置TSS结构体
	p->tss.back_link = 0;
	p->tss.esp0 = PAGE_SIZE + (long) p; // 内核态堆栈(物理地址)
	p->tss.ss0 = 0x10;                  // 内核数据段(0 ~ 16MB)
	p->tss.eip = eip;
	p->tss.eflags = eflags;
	p->tss.eax = 0;  // 子进程fork()的返回值
	p->tss.ecx = ecx;
	p->tss.edx = edx;
	p->tss.ebx = ebx;
	p->tss.esp = esp;
	p->tss.ebp = ebp;
	p->tss.esi = esi;
	p->tss.edi = edi;
	p->tss.es = es & 0xffff;
	p->tss.cs = cs & 0xffff;
	p->tss.ss = ss & 0xffff;
	p->tss.ds = ds & 0xffff;
	p->tss.fs = fs & 0xffff;
	p->tss.gs = gs & 0xffff;
	p->tss.ldt = _LDT(nr); // 指向GDT的偏移量
	p->tss.trace_bitmap = 0x80000000;

	if (last_task_used_math == current)
		__asm__("clts ; fnsave %0"::"m" (p->tss.i387));
	if (copy_mem(nr,p)) {
		task[nr] = NULL;
		free_page((long) p);
		return -EAGAIN;
	}
	for (i=0; i<NR_OPEN;i++)
		if ((f=p->filp[i]))
			f->f_count++;
	if (current->pwd)
		current->pwd->i_count++;
	if (current->root)
		current->root->i_count++;
	if (current->executable)
		current->executable->i_count++;
	// 设置TSS和LDT对应GDT项
	set_tss_desc(gdt+(nr<<1)+FIRST_TSS_ENTRY,&(p->tss));
	set_ldt_desc(gdt+(nr<<1)+FIRST_LDT_ENTRY,&(p->ldt));
	p->state = TASK_RUNNING;	/* do this last, just in case */
	return last_pid;
}

/*
 * 找到一个空的task struct结构(返回其对应task数组的下标), 并且设置新的last_pid
 */
int find_empty_process(void)
{
	int i;

	repeat:
		if ((++last_pid)<0) last_pid=1;
		for(i=0 ; i<NR_TASKS ; i++)
			if (task[i] && task[i]->pid == last_pid) goto repeat;
	for(i=1 ; i<NR_TASKS ; i++)
		if (!task[i])
			return i;
	return -EAGAIN;
}
