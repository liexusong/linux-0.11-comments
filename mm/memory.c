/*
 *  linux/mm/memory.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * demand-loading started 01.12.91 - seems it is high on the list of
 * things wanted, and it should be easy to implement. - Linus
 */

/*
 * Ok, demand-loading was easy, shared pages a little bit tricker. Shared
 * pages started 02.12.91, seems to work. - Linus.
 *
 * Tested sharing by executing about 30 /bin/sh: under the old kernel it
 * would have taken more than the 6M I have free, but it worked well as
 * far as I could see.
 *
 * Also corrected some "invalidate()"s - I wasn't doing enough of them.
 */

#include <signal.h>

#include <asm/system.h>

#include <linux/sched.h>
#include <linux/head.h>
#include <linux/kernel.h>

volatile void do_exit(long code);

static inline volatile void oom(void)
{
	printk("out of memory\n\r");
	do_exit(SIGSEGV);
}

#define invalidate() \
__asm__("movl %%eax,%%cr3"::"a" (0))

/* these are not to be changed without changing head.s etc */
#define LOW_MEM 0x100000
#define PAGING_MEMORY (15*1024*1024)   /* 15MB */
#define PAGING_PAGES (PAGING_MEMORY>>12)
#define MAP_NR(addr) (((addr)-LOW_MEM)>>12)
#define USED 100

#define CODE_SPACE(addr) ((((addr)+4095)&~4095) < \
current->start_code + current->end_code)

static long HIGH_MEMORY = 0;

#define copy_page(from,to) \
__asm__("cld ; rep ; movsl"::"S" (from),"D" (to),"c" (1024))

static unsigned char mem_map [ PAGING_PAGES ] = {0,};

/*
 * Get physical address of first (actually last :-) free page, and mark it
 * used. If no free pages left, return 0.
 */
unsigned long get_free_page(void)
{
register unsigned long __res asm("ax");

// ecx: 计数用, 开始设置为PAGING_PAGES
// di: 用于mem_map内存开始地址
__asm__("std ; repne ; scasb\n\t" // 置方向位, al(0)与对应每个页面的(di)内容比较
	"jne 1f\n\t"                  // 如果没有等于0的字节, 则跳转到结束(返回0)
	"movb $1,1(%%edi)\n\t"        // 将对应的页面位置设置为1
	"sall $12,%%ecx\n\t"          // 页面数 * 4k == 相对页面开始地址
	"addl %2,%%ecx\n\t"           // 加上低端内存地址(0x100000)
	"movl %%ecx,%%edx\n\t"
	"movl $1024,%%ecx\n\t"
	"leal 4092(%%edx),%%edi\n\t"
	"rep ; stosl\n\t"             // 清空内存页数据
	"movl %%edx,%%eax\n"
	"1:"
	:"=a" (__res)
	:"0" (0),"i" (LOW_MEM),"c" (PAGING_PAGES),
	"D" (mem_map+PAGING_PAGES-1)
	);
return __res;  // 返回空闲页的物理地址
}

/*
 * Free a page of memory at physical address 'addr'. Used by
 * 'free_page_tables()'
 */
void free_page(unsigned long addr)
{
	if (addr < LOW_MEM) return;     // 地址小于系统的最小内存地址
	if (addr >= HIGH_MEMORY)        // 地址超出系统支持的最大内存地址
		panic("trying to free nonexistent page");
	addr -= LOW_MEM;
	addr >>= 12;
	if (mem_map[addr]--) return;
	mem_map[addr]=0;
	panic("trying to free free page");
}

/*
 * This function frees a continuos block of page tables, as needed
 * by 'exit()'. As does copy_page_tables(), this handles only 4Mb blocks.
 */
int free_page_tables(unsigned long from,unsigned long size)
{
	unsigned long *pg_table;
	unsigned long * dir, nr;

	if (from & 0x3fffff)
		panic("free_page_tables called with wrong alignment");
	if (!from)
		panic("Trying to free up swapper memory space");
	size = (size + 0x3fffff) >> 22;  // 除以4m, 计算出页目录项数
	dir = (unsigned long *) ((from>>20) & 0xffc); /* _pg_dir = 0 */
	for ( ; size-- > 0 ; dir++) {
		if (!(1 & *dir)) // 如果内存页无效, 跳过此页
			continue;
		pg_table = (unsigned long *) (0xfffff000 & *dir);
		for (nr=0 ; nr<1024 ; nr++) {
			if (1 & *pg_table)
				free_page(0xfffff000 & *pg_table);
			*pg_table = 0;
			pg_table++;
		}
		free_page(0xfffff000 & *dir);
		*dir = 0;
	}
	invalidate(); // 刷新CPU的高速缓存
	return 0;
}

/*
 *  Well, here is one of the most complicated functions in mm. It
 * copies a range of linerar addresses by copying only the pages.
 * Let's hope this is bug-free, 'cause this one I don't want to debug :-)
 *
 * Note! We don't copy just any chunks of memory - addresses have to
 * be divisible by 4Mb (one page-directory entry), as this makes the
 * function easier. It's used only by fork anyway.
 *
 * NOTE 2!! When from==0 we are copying kernel space for the first
 * fork(). Then we DONT want to copy a full page-directory entry, as
 * that would lead to some serious memory waste - we just copy the
 * first 160 pages - 640kB. Even that is more than we need, but it
 * doesn't take any more memory - we don't copy-on-write in the low
 * 1 Mb-range, so the pages can be shared with the kernel. Thus the
 * special case for nr=xxxx.
 */
int copy_page_tables(unsigned long from,unsigned long to,long size)
{
	unsigned long * from_page_table;
	unsigned long * to_page_table;
	unsigned long this_page;
	unsigned long * from_dir, * to_dir;
	unsigned long nr;

	if ((from&0x3fffff) || (to&0x3fffff))
		panic("copy_page_tables called with wrong alignment");
	// 源地址所在页目录项
	from_dir = (unsigned long *) ((from>>20) & 0xffc); /* _pg_dir = 0 */
	// 目标地址所在页目录项
	to_dir = (unsigned long *) ((to>>20) & 0xffc);
	// 要复制多少个项
	size = ((unsigned) (size+0x3fffff)) >> 22;
	for( ; size-- > 0 ; from_dir++,to_dir++) {
		if (1 & *to_dir)
			panic("copy_page_tables: already exist");
		if (!(1 & *from_dir))
			continue;
		from_page_table = (unsigned long *) (0xfffff000 & *from_dir);
		if (!(to_page_table = (unsigned long *) get_free_page()))
			return -1;	/* Out of memory, see freeing */
		// 设置页目录表项的信息:
		// 把最后3位设置为1, 表示对应页表映射的内存页面是: 用户级的,可读写的,存在的
		*to_dir = ((unsigned long) to_page_table) | 7;
		nr = (from==0)?0xA0:1024;
		for ( ; nr-- > 0 ; from_page_table++,to_page_table++) {
			this_page = *from_page_table;
			if (!(1 & this_page))
				continue;
			// 把当前页表项设置为只读
			this_page &= ~2;
			*to_page_table = this_page;
			if (this_page > LOW_MEM) {
				*from_page_table = this_page;
				this_page -= LOW_MEM;
				this_page >>= 12;
				mem_map[this_page]++;
			}
		}
	}
	invalidate();
	return 0;
}

/*
 * This function puts a page in memory at the wanted address.
 * It returns the physical address of the page gotten, 0 if
 * out of memory (either when trying to access page-table or
 * page.)
 */
// address映射到page
unsigned long put_page(unsigned long page,unsigned long address)
{
	unsigned long tmp, *page_table;

/* NOTE !!! This uses the fact that _pg_dir=0 */

	if (page < LOW_MEM || page >= HIGH_MEMORY)
		printk("Trying to put page %p at %p\n",page,address);
	if (mem_map[(page-LOW_MEM)>>12] != 1) // 如果此内存页还没有申请, 提示错误
		printk("mem_map disagrees with %p at %p\n",page,address);
	// 找到address对应的页目录项
	page_table = (unsigned long *) ((address>>20) & 0xffc);
	if ((*page_table)&1) // 如果页表存在
		page_table = (unsigned long *) (0xfffff000 & *page_table);
	else { // 如果页表不存在, 则申请一块内存页保存
		if (!(tmp=get_free_page()))
			return 0;
		*page_table = tmp|7;
		page_table = (unsigned long *) tmp;
	}
	// 映射到物理内存页page
	page_table[(address>>12) & 0x3ff] = page | 7;
/* no need for invalidate */
	return page;
}

void un_wp_page(unsigned long * table_entry)
{
	unsigned long old_page,new_page;

	old_page = 0xfffff000 & *table_entry;
	if (old_page >= LOW_MEM && mem_map[MAP_NR(old_page)]==1) {
		*table_entry |= 2;
		invalidate();
		return;
	}
	if (!(new_page=get_free_page()))
		oom();
	if (old_page >= LOW_MEM)
		mem_map[MAP_NR(old_page)]--;
	*table_entry = new_page | 7;
	invalidate();
	copy_page(old_page,new_page);
}

/*
 * This routine handles present pages, when users try to write
 * to a shared page. It is done by copying the page to a new address
 * and decrementing the shared-page counter for the old page.
 *
 * If it's in code space we exit with a segment error.
 */
void do_wp_page(unsigned long error_code,unsigned long address)
{
#if 0
/* we cannot do this yet: the estdio library writes to code space */
/* stupid, stupid. I really want the libc.a from GNU */
	if (CODE_SPACE(address))
		do_exit(SIGSEGV);
#endif
	un_wp_page((unsigned long *)
		(((address>>10) & 0xffc) + (0xfffff000 &
		*((unsigned long *) ((address>>20) &0xffc)))));

}

void write_verify(unsigned long address)
{
	unsigned long page;

	/* 页表项是否可写? 不可写就直接返回 */
	if (!( (page = *((unsigned long *) ((address>>20) & 0xffc)) )&1))
		return;
	page &= 0xfffff000;
	page += ((address>>10) & 0xffc); // address对应的页表项
	if ((3 & *(unsigned long *) page) == 1)  /* non-writeable, present (不可写, 存在) */
		un_wp_page((unsigned long *) page); // 写时复制
	return;
}

// 把线性地址address映射到物理地址
// 物理地址通过get_free_page()获得
void get_empty_page(unsigned long address)
{
	unsigned long tmp;

	if (!(tmp=get_free_page()) || !put_page(tmp,address)) {
		free_page(tmp);		/* 0 is ok - ignored */
		oom();
	}
}

/*
 * try_to_share() checks the page at address "address" in the task "p",
 * to see if it exists, and if it is clean. If so, share it with the current
 * task.
 *
 * NOTE! This assumes we have checked that p != current, and that they
 * share the same executable.
 */
static int try_to_share(unsigned long address, struct task_struct * p)
{
	unsigned long from;
	unsigned long to;
	unsigned long from_page;
	unsigned long to_page;
	unsigned long phys_addr;

	from_page = to_page = ((address>>20) & 0xffc);

	from_page += ((p->start_code>>20) & 0xffc);
	to_page += ((current->start_code>>20) & 0xffc);
/* is there a page-directory at from? */
	from = *(unsigned long *) from_page;
	if (!(from & 1)) // 如果源页表不存在, 返回
		return 0;
	from &= 0xfffff000;
	from_page = from + ((address>>10) & 0xffc); // 页表项地址
	phys_addr = *(unsigned long *) from_page;   // 页表项内容
/* is the page clean and present? */
	if ((phys_addr & 0x41) != 0x01)
		return 0;
	phys_addr &= 0xfffff000;
	// 判断物理地址是否正确
	if (phys_addr >= HIGH_MEMORY || phys_addr < LOW_MEM)
		return 0;
	to = *(unsigned long *) to_page;
	if (!(to & 1)) {
		if ((to = get_free_page()))
			*(unsigned long *) to_page = to | 7;
		else
			oom();
	}
	to &= 0xfffff000;
	to_page = to + ((address>>10) & 0xffc);
	if (1 & *(unsigned long *) to_page)
		panic("try_to_share: to_page already exists");
/* share them: write-protect */
	// 去除可写标志位
	*(unsigned long *) from_page &= ~2;
	// 复制源内存页地址
	*(unsigned long *) to_page = *(unsigned long *) from_page;
	invalidate(); // 清除高速缓存
	phys_addr -= LOW_MEM;
	phys_addr >>= 12;
	mem_map[phys_addr]++; // 计数器加一
	return 1;
}

/*
 * share_page() tries to find a process that could share a page with
 * the current one. Address is the address of the wanted page relative
 * to the current data space.
 *
 * We first check if it is at all feasible by checking executable->i_count.
 * It should be >1 if there are other tasks sharing this inode.
 */
static int share_page(unsigned long address)
{
	struct task_struct ** p;

	if (!current->executable) // 1) 进程是否跟程序文件相关
		return 0;
	if (current->executable->i_count < 2) // 2) 程序文件是否被多个进程引用
		return 0;
	for (p = &LAST_TASK ; p > &FIRST_TASK ; --p) {
		if (!*p)
			continue;
		if (current == *p)
			continue;
		if ((*p)->executable != current->executable)
			continue;
		// 找到一个与当前进程公用程序文件的进程
		// 那么就尝试共享内存页, 节省内存的使用
		if (try_to_share(address,*p))
			return 1;
	}
	return 0;
}

// 缺页处理:
// address是缺页的虚拟地址
void do_no_page(unsigned long error_code,unsigned long address)
{
	int nr[4];
	unsigned long tmp;
	unsigned long page;
	int block,i;

	address &= 0xfffff000; // 过滤偏移地址
	tmp = address - current->start_code; // 缺页页面对应的逻辑地址
	if (!current->executable || tmp >= current->end_data) {
		get_empty_page(address);
		return;
	}
	// 尝试共享内存页
	if (share_page(tmp))
		return;
	// 如果共享失败, 则申请块新的内存页
	if (!(page = get_free_page()))
		oom();
/* remember that 1 block is used for header */
	block = 1 + tmp/BLOCK_SIZE; // 跳过文件头
	for (i=0 ; i<4 ; block++,i++)
		nr[i] = bmap(current->executable,block); // 获得逻辑块号
	// 从设备读取数据到内存页(4k)
	bread_page(page,current->executable->i_dev,nr);
	i = tmp + 4096 - current->end_data;
	tmp = page + 4096; // 指向内存页尾端
	while (i-- > 0) {
		tmp--;
		*(char *)tmp = 0;
	}
	if (put_page(page,address)) // 映射线性地址到物理地址
		return;
	free_page(page);
	oom();
}

void mem_init(long start_mem, long end_mem)
{
	int i;

	HIGH_MEMORY = end_mem;
	for (i=0 ; i<PAGING_PAGES ; i++) // PAGING_PAGES等于15MB的page数
		mem_map[i] = USED;
	i = MAP_NR(start_mem);
	end_mem -= start_mem;
	end_mem >>= 12;
	while (end_mem-->0)
		mem_map[i++]=0;
}

void calc_mem(void)
{
	int i,j,k,free=0;
	long * pg_tbl;

	for(i=0 ; i<PAGING_PAGES ; i++)
		if (!mem_map[i]) free++;
	printk("%d pages free (of %d)\n\r",free,PAGING_PAGES);
	for(i=2 ; i<1024 ; i++) {
		if (1&pg_dir[i]) {
			pg_tbl=(long *) (0xfffff000 & pg_dir[i]);
			for(j=k=0 ; j<1024 ; j++)
				if (pg_tbl[j]&1)
					k++;
			printk("Pg-dir[%d] uses %d pages\n",i,k);
		}
	}
}
