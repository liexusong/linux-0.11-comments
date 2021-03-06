/*
 *  linux/fs/exec.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * #!-checking implemented by tytso.
 */

/*
 * Demand-loading implemented 01.12.91 - no need to read anything but
 * the header into memory. The inode of the executable is put into
 * "current->executable", and page faults do the actual loading. Clean.
 *
 * Once more I can proudly say that linux stood up to being changed: it
 * was less than 2 hours work to get demand-loading completely implemented.
 */

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <a.out.h>

#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <asm/segment.h>

extern int sys_exit(int exit_code);
extern int sys_close(int fd);

/*
 * MAX_ARG_PAGES defines the number of pages allocated for arguments
 * and envelope for the new program. 32 should suffice, this gives
 * a maximum env+arg of 128kB !
 */
#define MAX_ARG_PAGES 32

/*
 * create_tables() parses the env- and arg-strings in new user
 * memory and creates the pointer tables from them, and puts their
 * addresses on the "stack", returning the new stack pointer value.
 */
// 建立参数数组和环境变量数组

/*
	0x4000000
		+--------+
		|  ....  | (环境变量和启动参数的数据)
		+--------+ . <--- p
		|  NULL  |  \
		+--------+   +------> (envc+1)
		|  ....  |  /
		+--------+ . <--- envp
		|  NULL  |  \
		+--------+   +------> (argc+1)
		|  ....  |  /
		+--------+ . <--- argv
		|  envp  |
		+--------+
		|  argv  |
		+--------+
		|  argc  |
		+--------+ <--- sp
*/

static unsigned long * create_tables(char * p,int argc,int envc)
{
	unsigned long *argv,*envp;
	unsigned long * sp;

	sp = (unsigned long *) (0xfffffffc & (unsigned long) p);
	sp -= envc+1;
	envp = sp;
	sp -= argc+1;
	argv = sp;
	// put: envp -> sp
	put_fs_long((unsigned long)envp,--sp); // envp指针数组
	put_fs_long((unsigned long)argv,--sp); // argv指针数组
	put_fs_long((unsigned long)argc,--sp); // argc值
	while (argc-->0) {
		put_fs_long((unsigned long) p,argv++);
		while (get_fs_byte(p++)) /* nothing */ ;
	}
	put_fs_long(0,argv);
	while (envc-->0) {
		put_fs_long((unsigned long) p,envp++);
		while (get_fs_byte(p++)) /* nothing */ ;
	}
	put_fs_long(0,envp);
	return sp;
}

/*
 * count() counts the number of arguments/envelopes
 */
static int count(char ** argv)
{
	int i=0;
	char ** tmp;

	if ((tmp = argv))
		while (get_fs_long((unsigned long *) (tmp++)))
			i++;

	return i;
}

/*
 * 'copy_string()' copies argument/envelope strings from user
 * memory to free pages in kernel mem. These are in a format ready
 * to be put directly into the top of new user memory.
 *
 * Modified by TYT, 11/24/91 to add the from_kmem argument, which specifies
 * whether the string and the string array are from user or kernel segments:
 *
 * from_kmem     argv *        argv **
 *    0          user space    user space
 *    1          kernel space  user space
 *    2          kernel space  kernel space
 *
 * 上面的意思是:
 * 0 代表参数和参数指针都来源于用户态
 * 1 代表参数指针来源于内核态, 参数来源于用户态
 * 2 代表参数和参数指针都来源于内核态
 *
 * We do this by playing games with the fs segment register.  Since it
 * it is expensive to load a segment register, we try to avoid calling
 * set_fs() unless we absolutely have to.
 */
// argv是参数数组的指针(可能在用户态, 也可能在内核态)
// page是内核态的内存页
static unsigned long copy_strings(int argc,char ** argv,unsigned long *page,
		unsigned long p, int from_kmem)
{
	char *tmp, *pag=NULL;
	int len, offset = 0;
	unsigned long old_fs, new_fs;

	if (!p)
		return 0;	/* bullet-proofing */
	new_fs = get_ds();   // 获取DS寄存器的值 (内核态)
	old_fs = get_fs();   // 获取FS寄存器的值
	if (from_kmem==2)    // 如果数据来源于内核
		set_fs(new_fs);  // 则设置FS为内核数据段

	while (argc-- > 0) {
		if (from_kmem == 1) // 如果参数指针来源于内核态
			set_fs(new_fs);
		// 获取参数的指针
		if (!(tmp = (char *)get_fs_long(((unsigned long *)argv)+argc)))
			panic("argc is wrong");
		if (from_kmem == 1)
			set_fs(old_fs);
		len=0;		/* remember zero-padding */
		do {
			len++; // 字符串的长度
		} while (get_fs_byte(tmp++)); // 找到字符串的结尾字符NIL
		if (p-len < 0) {	/* this shouldn't happen - 128kB */
			set_fs(old_fs);
			return 0;
		}
		// 复制字符串
		while (len) {
			--p; --tmp; --len;
			if (--offset < 0) { // offset是内存页的偏移量
				offset = p % PAGE_SIZE;
				if (from_kmem==2)
					set_fs(old_fs);
				if (!(pag = (char *) page[p/PAGE_SIZE]) &&
				    !(pag = (char *) page[p/PAGE_SIZE] =
				      (unsigned long *) get_free_page()))
					return 0;
				if (from_kmem==2)
					set_fs(new_fs);

			}
			*(pag + offset) = get_fs_byte(tmp); // 从后往前复制
		}
	}
	if (from_kmem==2)
		set_fs(old_fs);
	return p;
}

// 这个函数主要功能是修改进程的代码段和数据段的限长
// 代码段长度修改为执行文件的代码段长度
// 数据段长度修改为60M
// 并且把page内存页与线性地址做映射关系
// text_size: 代码段长度
// page: 要映射的内存页
static unsigned long change_ldt(unsigned long text_size,unsigned long * page)
{
	unsigned long code_limit,data_limit,code_base,data_base;
	int i;

	code_limit = text_size+PAGE_SIZE -1;
	code_limit &= 0xFFFFF000; // 内存页对齐
	data_limit = 0x4000000;   // 数据段限制
	code_base = get_base(current->ldt[1]);
	data_base = code_base;

	set_base(current->ldt[1],code_base);
	set_limit(current->ldt[1],code_limit);

	set_base(current->ldt[2],data_base);
	set_limit(current->ldt[2],data_limit);

/* make sure fs points to the NEW data segment */
	__asm__("pushl $0x17\n\tpop %%fs"::);
	data_base += data_limit;
	for (i=MAX_ARG_PAGES-1 ; i>=0 ; i--) {
		data_base -= PAGE_SIZE;
		if (page[i])
			put_page(page[i],data_base); // 映射线性地址与物理地址(data_base是线性地址)
	}
	return data_limit;
}

/*
 * 'do_execve()' executes a new program.
 */
int do_execve(unsigned long * eip,long tmp,char * filename,
	char ** argv, char ** envp)
{
	struct m_inode * inode;
	struct buffer_head * bh;
	struct exec ex;
	unsigned long page[MAX_ARG_PAGES];
	int i,argc,envc;
	int e_uid, e_gid;
	int retval;
	int sh_bang = 0;
	unsigned long p=PAGE_SIZE*MAX_ARG_PAGES-4; // 4096 * 32 - 4

	if ((0xffff & eip[1]) != 0x000f) // eip[1]是cs寄存器的值(内核态不能调用execve系统调用)
		panic("execve called from supervisor mode");
	for (i=0 ; i<MAX_ARG_PAGES ; i++)	/* clear page-table */
		page[i]=0;
	if (!(inode=namei(filename)))		/* get executables inode */
		return -ENOENT;
	argc = count(argv);
	envc = count(envp);

restart_interp:
	if (!S_ISREG(inode->i_mode)) {	/* must be regular file */
		retval = -EACCES;
		goto exec_error2;
	}
	i = inode->i_mode;
	e_uid = (i & S_ISUID) ? inode->i_uid : current->euid; // 是否使用执行文件的uid
	e_gid = (i & S_ISGID) ? inode->i_gid : current->egid; // 是否使用执行文件的gid
	if (current->euid == inode->i_uid) // 如果执行文件属于当前进程(使用宿主权限)
		i >>= 6;
	else if (current->egid == inode->i_gid) // 执行文件与当前进程的组相同(使用组权限)
		i >>= 3;
	if (!(i & 1) &&
	    !((inode->i_mode & 0111) && suser())) { // 如果其他用户没有执行权限或者不是超级用户
		retval = -ENOEXEC;
		goto exec_error2;
	}
	// 读取文件头
	if (!(bh = bread(inode->i_dev,inode->i_zone[0]))) {
		retval = -EACCES;
		goto exec_error2;
	}
	ex = *((struct exec *) bh->b_data);	/* read exec-header */

	// 1) 如果是shell脚本, 执行下面的逻辑
	if ((bh->b_data[0] == '#') && (bh->b_data[1] == '!') && (!sh_bang)) {
		/*
		 * This section does the #! interpretation.
		 * Sorta complicated, but hopefully it will work.  -TYT
		 */

		char buf[1023], *cp, *interp, *i_name, *i_arg;
		unsigned long old_fs;

		strncpy(buf, bh->b_data+2, 1022);
		brelse(bh);
		iput(inode);
		buf[1022] = '\0';
		if ((cp = strchr(buf, '\n'))) {
			*cp = '\0';
			for (cp = buf; (*cp == ' ') || (*cp == '\t'); cp++);
		}
		if (!cp || *cp == '\0') {
			retval = -ENOEXEC; /* No interpreter name found */
			goto exec_error1;
		}
		interp = i_name = cp;
		i_arg = 0;
		for ( ; *cp && (*cp != ' ') && (*cp != '\t'); cp++) {
 			if (*cp == '/')
				i_name = cp+1;
		}
		if (*cp) {
			*cp++ = '\0';
			i_arg = cp;
		}
		/*
		 * OK, we've parsed out the interpreter name and
		 * (optional) argument.
		 */
		if (sh_bang++ == 0) {
			p = copy_strings(envc, envp, page, p, 0);
			p = copy_strings(--argc, argv+1, page, p, 0);
		}
		/*
		 * Splice in (1) the interpreter's name for argv[0]
		 *           (2) (optional) argument to interpreter
		 *           (3) filename of shell script
		 *
		 * This is done in reverse order, because of how the
		 * user environment and arguments are stored.
		 */
		p = copy_strings(1, &filename, page, p, 1);
		argc++;
		if (i_arg) {
			p = copy_strings(1, &i_arg, page, p, 2);
			argc++;
		}
		p = copy_strings(1, &i_name, page, p, 2);
		argc++;
		if (!p) {
			retval = -ENOMEM;
			goto exec_error1;
		}
		/*
		 * OK, now restart the process with the interpreter's inode.
		 */
		old_fs = get_fs();
		set_fs(get_ds());
		if (!(inode=namei(interp))) { /* get executables inode */
			set_fs(old_fs);
			retval = -ENOENT;
			goto exec_error1;
		}
		set_fs(old_fs);
		goto restart_interp;
	}

	brelse(bh);

	// 2) 执行bin程序

	if (N_MAGIC(ex) != ZMAGIC || ex.a_trsize || ex.a_drsize ||
		ex.a_text+ex.a_data+ex.a_bss>0x3000000 ||
		inode->i_size < ex.a_text+ex.a_data+ex.a_syms+N_TXTOFF(ex)) {
		retval = -ENOEXEC;
		goto exec_error2;
	}
	if (N_TXTOFF(ex) != BLOCK_SIZE) {
		printk("%s: N_TXTOFF != BLOCK_SIZE. See a.out.h.", filename);
		retval = -ENOEXEC;
		goto exec_error2;
	}

	if (!sh_bang) { // 复制参数和环境变量到内存页
		p = copy_strings(envc,envp,page,p,0);
		p = copy_strings(argc,argv,page,p,0);
		if (!p) {
			retval = -ENOMEM;
			goto exec_error2;
		}
	}

/* OK, This is the point of no return */
	if (current->executable)
		iput(current->executable);
	current->executable = inode;
	for (i=0 ; i<32 ; i++)
		current->sigaction[i].sa_handler = NULL;
	for (i=0 ; i<NR_OPEN ; i++) // 执行程序时需要关闭的文件
		if ((current->close_on_exec>>i)&1)
			sys_close(i);
	current->close_on_exec = 0;
	// 释放进程占用的内存页(因为执行新程序的时候, 这些内存页都是没有用的)
	free_page_tables(get_base(current->ldt[1]),get_limit(0x0f));
	free_page_tables(get_base(current->ldt[2]),get_limit(0x17));
	if (last_task_used_math == current)
		last_task_used_math = NULL;
	current->used_math = 0;
	p += change_ldt(ex.a_text,page)-MAX_ARG_PAGES*PAGE_SIZE;
	p = (unsigned long) create_tables((char *)p,argc,envc);
	// 程序暂时可以使用的最大内存
	current->brk = ex.a_bss +
		(current->end_data = ex.a_data +
		(current->end_code = ex.a_text));
	current->start_stack = p & 0xfffff000;
	current->euid = e_uid;
	current->egid = e_gid;
	i = ex.a_text+ex.a_data;
	while (i&0xfff) // 如果代码段+数据段不是页对齐, 那么补充不足的一页的数据
		put_fs_byte(0,(char *) (i++));

	// 这里是execve返回时继续执行的入口(也就是程序的入口mian函数)
	eip[0] = ex.a_entry;  /* eip, magic happens :-) */
	eip[3] = p;			  /* stack pointer */
	return 0;

exec_error2:
	iput(inode);
exec_error1:
	for (i=0 ; i<MAX_ARG_PAGES ; i++)
		free_page(page[i]);
	return(retval);
}
