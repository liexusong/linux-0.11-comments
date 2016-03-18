/*
 *  linux/kernel/blk_dev/ll_rw.c
 *
 * (C) 1991 Linus Torvalds
 */

/*
 * This handles all read/write requests to block devices
 */
#include <errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>

#include "blk.h"

/*
 * The request-struct contains all necessary data
 * to load a nr of sectors into memory
 */
struct request request[NR_REQUEST];

/*
 * used to wait on when there are no free requests
 */
struct task_struct * wait_for_request = NULL;

/* blk_dev_struct is:
 *	do_request-address
 *	next-request
 */
struct blk_dev_struct blk_dev[NR_BLK_DEV] = {
	{ NULL, NULL },		/* no_dev */
	{ NULL, NULL },		/* dev mem */
	{ NULL, NULL },		/* dev fd */
	{ NULL, NULL },		/* dev hd */
	{ NULL, NULL },		/* dev ttyx */
	{ NULL, NULL },		/* dev tty */
	{ NULL, NULL }		/* dev lp */
};


// 锁住缓冲块: 只有在make_request()处被调用,
// 而make_request()被ll_rw_block()调用
// 锁住缓冲块的目的是防止中断导致的竞争情况
static inline void lock_buffer(struct buffer_head * bh)
{
	// 关闭所有IO中断: 防止因为硬盘中断而导致的竞争(数据不一致)
	// 因为硬盘中断也会修改缓冲块的b_lock字段,
	// 而硬盘中断是随机发生的(也就是会导致竞争情况出现)
	// ===============
	// 这种情况出现在:
	// 当b_lock为1的时候去执行sleep_on()操作,
	// 但这时候硬盘处理完毕了发生中断,
	// 而此时中断处理会把b_lock设置为0, 并且唤醒等待的进程
	// 但此时当前进程还没有被添加到睡眠队列中,
	// 所以会导致当前进程不会被唤醒的情况,
	// 关闭中断后可以防止此情况出现,
	// 也就是说在执行sleep_on()时不会被硬盘中断打断.
	cli();
	while (bh->b_lock)
		sleep_on(&bh->b_wait);
	bh->b_lock=1;
	sti();
}

static inline void unlock_buffer(struct buffer_head * bh)
{
	if (!bh->b_lock)
		printk("ll_rw_block.c: buffer not locked\n\r");
	bh->b_lock = 0;
	wake_up(&bh->b_wait);
}

/*
 * add-request adds a request to the linked list.
 * It disables interrupts so that it can muck with the
 * request-lists in peace.
 */
static void add_request(struct blk_dev_struct * dev, struct request * req)
{
	struct request * tmp;

	req->next = NULL;
	cli();  // 防止IO中断影响请求队列
	if (req->bh)
		req->bh->b_dirt = 0;
	if (!(tmp = dev->current_request)) {
		dev->current_request = req;
		sti();
		(dev->request_fn)();
		return;
	}
	for ( ; tmp->next ; tmp=tmp->next)
		if ((IN_ORDER(tmp,req) ||
		    !IN_ORDER(tmp,tmp->next)) &&
		    IN_ORDER(req,tmp->next))
			break;
	req->next=tmp->next;
	tmp->next=req;
	sti(); // 打开中断
}

static void make_request(int major,int rw, struct buffer_head * bh)
{
	struct request * req;
	int rw_ahead;

/* WRITEA/READA is special case - it is not really needed, so if the */
/* buffer is locked, we just forget about it, else it's a normal read */
	if ((rw_ahead = (rw == READA || rw == WRITEA))) {
		if (bh->b_lock)
			return;
		if (rw == READA)
			rw = READ;
		else
			rw = WRITE;
	}
	if (rw!=READ && rw!=WRITE)
		panic("Bad block dev command, must be R/W/RA/WA");
	lock_buffer(bh); // 锁着缓冲块, 防止其他进程对其进行修改
	if ((rw == WRITE && !bh->b_dirt) || (rw == READ && bh->b_uptodate)) {
		unlock_buffer(bh);
		return;
	}
repeat:
/* we don't allow the write-requests to fill up the queue completely:
 * we want some room for reads: they take precedence. The last third
 * of the requests are only for reads.
 */
// 这里有bug, 当找不到空闲的请求, 但还没有执行sleep_on()的时候发生了硬盘中断
// 这时候硬盘中断程序会把当前请求设置为空闲并且唤醒等待空闲请求的进程,
// 但此时当前进程并没有添加到等待队列中, 所以当回到本流程的时候会把当前进程添加到等待队列中,
// 然后睡眠了, 所以此时就会出现当前进程一直睡眠不会被唤醒的情况
	if (rw == READ)
		req = request+NR_REQUEST;
	else
		req = request+((NR_REQUEST*2)/3);
/* find an empty request */
	while (--req >= request)
		if (req->dev<0)
			break;
/* if none found, sleep on new requests: check for rw_ahead */
	if (req < request) { // 找不到空闲的request
		if (rw_ahead) {  // 如果是ahead操作, 直接返回
			unlock_buffer(bh);
			return;
		}
		sleep_on(&wait_for_request); // 否则等待有空闲的request
		goto repeat;
	}
/* fill up the request-info, and add it to the queue */
	req->dev = bh->b_dev;
	req->cmd = rw;
	req->errors=0;
	req->sector = bh->b_blocknr<<1;
	req->nr_sectors = 2;
	req->buffer = bh->b_data;
	req->waiting = NULL;
	req->bh = bh;
	req->next = NULL;
	add_request(major+blk_dev,req);
}

void ll_rw_block(int rw, struct buffer_head * bh)
{
	unsigned int major;

	if ((major=MAJOR(bh->b_dev)) >= NR_BLK_DEV ||
	!(blk_dev[major].request_fn)) {
		printk("Trying to read nonexistent block-device\n\r");
		return;
	}
	make_request(major,rw,bh);
}

void blk_dev_init(void)
{
	int i;

	for (i=0 ; i<NR_REQUEST ; i++) {
		request[i].dev = -1;
		request[i].next = NULL;
	}
}
