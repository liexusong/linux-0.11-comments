/*
 *  linux/fs/inode.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <string.h> 
#include <sys/stat.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <asm/system.h>

struct m_inode inode_table[NR_INODE]={{0,},};

static void read_inode(struct m_inode * inode);
static void write_inode(struct m_inode * inode);

static inline void wait_on_inode(struct m_inode * inode)
{
	cli();
	while (inode->i_lock)
		sleep_on(&inode->i_wait);
	sti();
}

static inline void lock_inode(struct m_inode * inode)
{
	cli();
	while (inode->i_lock)
		sleep_on(&inode->i_wait);
	inode->i_lock=1;
	sti();
}

static inline void unlock_inode(struct m_inode * inode)
{
	inode->i_lock=0;
	wake_up(&inode->i_wait);
}

// 把知道指定设备的inode设置为空闲
void invalidate_inodes(int dev)
{
	int i;
	struct m_inode * inode;

	inode = 0+inode_table;
	for(i=0 ; i<NR_INODE ; i++,inode++) {
		wait_on_inode(inode); // 等待inode解锁
		if (inode->i_dev == dev) {
			if (inode->i_count)
				printk("inode in use on removed disk\n\r");
			inode->i_dev = inode->i_dirt = 0;
		}
	}
}

// 同步所有inode
void sync_inodes(void)
{
	int i;
	struct m_inode * inode;

	inode = 0+inode_table;
	for(i=0 ; i<NR_INODE ; i++,inode++) {
		wait_on_inode(inode);
		if (inode->i_dirt && !inode->i_pipe) // 管道不需要同步
			write_inode(inode);
	}
}

// 这个函数用于创建磁盘块并且保存到inode的block位置中
// 根据block位置可以分为: 1) 直接块, 2) 一级间接块, 3) 二级间接块
static int _bmap(struct m_inode * inode,int block,int create)
{
	struct buffer_head * bh;
	int i;

	if (block<0)
		panic("_bmap: block<0");
	if (block >= 7+512+512*512)
		panic("_bmap: block>big");

	if (block<7) { // 直接块
		if (create && !inode->i_zone[block]) // 创建新的磁盘块与inode对应
			if ((inode->i_zone[block]=new_block(inode->i_dev))) {
				inode->i_ctime=CURRENT_TIME;
				inode->i_dirt=1;
			}
		return inode->i_zone[block];
	}

	block -= 7; // 得到的是一级间接块的索引

	if (block<512) { // 一级间接块
		if (create && !inode->i_zone[7]) // 索引块还不存在, 先创建
			if ((inode->i_zone[7]=new_block(inode->i_dev))) {
				inode->i_dirt=1;
				inode->i_ctime=CURRENT_TIME;
			}
		if (!inode->i_zone[7])
			return 0;
		if (!(bh = bread(inode->i_dev,inode->i_zone[7]))) // 读取一级间接索引块
			return 0;
		i = ((unsigned short *) (bh->b_data))[block];
		if (create && !i) // 如果磁盘块还不存在, 那么就创建一块新的
			if ((i=new_block(inode->i_dev))) {
				((unsigned short *) (bh->b_data))[block]=i;
				bh->b_dirt=1; // 设置被修改标志
			}
		brelse(bh);
		return i;
	}

	// 下面是处理二级间接块的逻辑

	block -= 512; // 得到的是二级间接块索引

	if (create && !inode->i_zone[8]) // 先创建二级间接索引块
		if ((inode->i_zone[8]=new_block(inode->i_dev))) {
			inode->i_dirt=1;
			inode->i_ctime=CURRENT_TIME;
		}
	if (!inode->i_zone[8])
		return 0;
	if (!(bh=bread(inode->i_dev,inode->i_zone[8]))) // 读取索引块
		return 0;
	i = ((unsigned short *)bh->b_data)[block>>9]; // block>>9 = (block/512)
	if (create && !i)
		if ((i=new_block(inode->i_dev))) {
			((unsigned short *) (bh->b_data))[block>>9]=i;
			bh->b_dirt=1;
		}
	brelse(bh);
	if (!i)
		return 0;
	if (!(bh=bread(inode->i_dev,i))) // 这里是真正的数据块索引
		return 0;
	i = ((unsigned short *)bh->b_data)[block&511]; // 数据块索引 (0 ~ 511)
	if (create && !i)
		if ((i=new_block(inode->i_dev))) {
			((unsigned short *) (bh->b_data))[block&511]=i;
			bh->b_dirt=1;
		}
	brelse(bh);
	return i;
}

// 获取block对应的磁盘块号
int bmap(struct m_inode * inode,int block)
{
	return _bmap(inode,block,0);
}

// 申请一个磁盘块与block映射
int create_block(struct m_inode * inode, int block)
{
	return _bmap(inode,block,1);
}
		
void iput(struct m_inode * inode)
{
	if (!inode)
		return;
	wait_on_inode(inode); // wait inode unlock
	if (!inode->i_count)
		panic("iput: trying to free free inode");

	if (inode->i_pipe) { // 如果是管道
		wake_up(&inode->i_wait);
		if (--inode->i_count)
			return;
		free_page(inode->i_size); // 如果是管道, 那么i_size保存的就是内存地址, 此时需要释放管道使用的内存
		inode->i_count=0;
		inode->i_dirt=0;
		inode->i_pipe=0;
		return;
	}

	if (!inode->i_dev) {
		inode->i_count--;
		return;
	}

	if (S_ISBLK(inode->i_mode)) { // 如果是块设备文件(例如软盘或者硬盘), i_zone[0]存放的是设备号
		sync_dev(inode->i_zone[0]); // 刷新设备对应的缓冲块
		wait_on_inode(inode);
	}

repeat:
	if (inode->i_count>1) {
		inode->i_count--;
		return;
	}

	// 做释放资源的工作

	if (!inode->i_nlinks) { // 如果没有进程引用此inode, 那么释放他
		truncate(inode);    // 删除文件占用的磁盘块
		free_inode(inode);  // 释放inode
		return;
	}

	if (inode->i_dirt) {
		write_inode(inode);	/* we can sleep - so do again */
		wait_on_inode(inode);
		goto repeat;
	}

	inode->i_count--; // 减少计数器

	return;
}

struct m_inode * get_empty_inode(void)
{
	struct m_inode * inode;
	static struct m_inode * last_inode = inode_table;
	int i;

	do {
		inode = NULL;
		for (i = NR_INODE; i ; i--) {
			if (++last_inode >= inode_table + NR_INODE)
				last_inode = inode_table;
			if (!last_inode->i_count) {
				inode = last_inode;
				if (!inode->i_dirt && !inode->i_lock)
					break;
			}
		}
		if (!inode) { // 没有可用的内存inode
			for (i=0 ; i<NR_INODE ; i++)
				printk("%04x: %6d\t",inode_table[i].i_dev,
					inode_table[i].i_num);
			panic("No free inodes in mem");
		}
		wait_on_inode(inode);
		while (inode->i_dirt) {
			write_inode(inode);
			wait_on_inode(inode);
		}
	} while (inode->i_count);

	memset(inode,0,sizeof(*inode));
	inode->i_count = 1;
	return inode;
}

// 获得一个管道的inode
struct m_inode * get_pipe_inode(void)
{
	struct m_inode * inode;

	if (!(inode = get_empty_inode()))
		return NULL;
	if (!(inode->i_size=get_free_page())) {
		inode->i_count = 0;
		return NULL;
	}
	inode->i_count = 2;	/* sum of readers/writers */
	PIPE_HEAD(*inode) = PIPE_TAIL(*inode) = 0;
	inode->i_pipe = 1;
	return inode;
}

// 根据设备号与i节点号获取inode
struct m_inode * iget(int dev,int nr)
{
	struct m_inode * inode, * empty;

	if (!dev)
		panic("iget with dev==0");

	empty = get_empty_inode();
	inode = inode_table;

	while (inode < NR_INODE+inode_table) {
		if (inode->i_dev != dev || inode->i_num != nr) {
			inode++;
			continue;
		}

		// 找到dev和nr对应的inode

		wait_on_inode(inode); // 等待inode锁被释放
		if (inode->i_dev != dev || inode->i_num != nr) {
			inode = inode_table;
			continue;
		}

		inode->i_count++;

		// 如果此inode挂载了一个文件系统(只能是文件夹)
		// 那么把inode切换到挂载的文件系统根i节点
		if (inode->i_mount) {
			int i;

			for (i = 0 ; i<NR_SUPER ; i++)
				if (super_block[i].s_imount==inode)
					break;
			if (i >= NR_SUPER) {
				printk("Mounted inode hasn't got sb\n");
				if (empty)
					iput(empty);
				return inode;
			}
			iput(inode);
			// 挂载的设备号和根目录的块的inode号
			dev = super_block[i].s_dev;
			nr = ROOT_INO;
			inode = inode_table;
			continue;
		}

		if (empty)
			iput(empty);
		return inode;
	}

	if (!empty)
		return (NULL);
	inode=empty;
	inode->i_dev = dev;
	inode->i_num = nr;
	read_inode(inode); // 从磁盘中读取inode的数据到内存inode中
	return inode;
}

static void read_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;
	int block;

	lock_inode(inode); // 先锁着inode
	if (!(sb=get_super(inode->i_dev)))
		panic("trying to read inode without dev");
	// 计算inode所在的磁盘块
	block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks +
		(inode->i_num-1)/INODES_PER_BLOCK;
	if (!(bh=bread(inode->i_dev,block))) // 因为这里有可能会被睡眠
		panic("unable to read i-node block");
	*(struct d_inode *)inode =
		((struct d_inode *)bh->b_data)
			[(inode->i_num-1)%INODES_PER_BLOCK];
	brelse(bh);
	unlock_inode(inode);
}

static void write_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;
	int block;

	lock_inode(inode);
	if (!inode->i_dirt || !inode->i_dev) {
		unlock_inode(inode);
		return;
	}
	if (!(sb=get_super(inode->i_dev)))
		panic("trying to write inode without device");
	block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks +
		(inode->i_num-1)/INODES_PER_BLOCK;
	if (!(bh=bread(inode->i_dev,block)))
		panic("unable to read i-node block");
	((struct d_inode *)bh->b_data)
		[(inode->i_num-1)%INODES_PER_BLOCK] =
			*(struct d_inode *)inode;
	bh->b_dirt=1;
	inode->i_dirt=0;
	brelse(bh);
	unlock_inode(inode);
}
