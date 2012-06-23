/*
 * elevator rd
 */
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
//#include <linux/jiffies.h>

static char vippidbuf[16];
static struct proc_dir_entry *proc_entry;
static pid_t vippid=0;//虽然貌似有特殊进程pid为0,但是本程序vippid==0时认为其无效
static unsigned long dontuntil=0;//只有当jiffies>dontuntil后才能dispatch普通进程
static unsigned vipcount=0,normalcount=0;//测试统计用

struct rd_data {
	struct request_queue *q;
	struct list_head queue,vipqueue;
	struct timer_list resume_timer;//用来重新唤醒io
}*ndglobal;

//FIXME
static void rd_merged_requests(struct request_queue *q, struct request *rq,
				 struct request *next)
{
	list_del_init(&next->queuelist);
}

static int rd_dispatch(struct request_queue *q, int force)
{
	struct rd_data *nd = q->elevator->elevator_data;

	if (!list_empty(&nd->vipqueue)) {
		struct request *rq;
		rq = list_entry(nd->vipqueue.next, struct request, queuelist);
		list_del_init(&rq->queuelist);
		elv_dispatch_sort(q, rq);
		++vipcount;
		dontuntil=jiffies+msecs_to_jiffies(20);//20ms后才能dispatch普通进程的请求
		mod_timer(&nd->resume_timer,jiffies+msecs_to_jiffies(30));//定时器在30ms后尝试重启io
		return 1;
	}
	if ((force||time_after(jiffies,dontuntil))&&!list_empty(&nd->queue)) {
		struct request *rq;
		rq = list_entry(nd->queue.next, struct request, queuelist);
		list_del_init(&rq->queuelist);
		elv_dispatch_sort(q, rq);
		++normalcount;
		return 1;
	}
	return 0;
}

static void rd_add_request(struct request_queue *q, struct request *rq)
{
	struct rd_data *nd = q->elevator->elevator_data;

	if(vippid&&current->pid==vippid)
		list_add_tail(&rq->queuelist, &nd->vipqueue);
	else
		list_add_tail(&rq->queuelist, &nd->queue);
}

static int rd_queue_empty(struct request_queue *q)
{
	struct rd_data *nd = q->elevator->elevator_data;
	return list_empty(&nd->vipqueue)&&(!time_after(jiffies,dontuntil)||list_empty(&nd->queue));
}

static struct request *
rd_former_request(struct request_queue *q, struct request *rq)
{
	struct rd_data *nd = q->elevator->elevator_data;

	if (rq->queuelist.prev == &nd->queue||rq->queuelist.prev == &nd->vipqueue)
		return NULL;
	return list_entry(rq->queuelist.prev, struct request, queuelist);
}

static struct request *
rd_latter_request(struct request_queue *q, struct request *rq)
{
	struct rd_data *nd = q->elevator->elevator_data;

	if (rq->queuelist.next == &nd->queue||rq->queuelist.next == &nd->vipqueue)
		return NULL;
	return list_entry(rq->queuelist.next, struct request, queuelist);
}
static void resume_timeout(unsigned long data)
{
	printk(KERN_INFO "0timeout,j=%lu,vipcount=%u,normalcount=%u\n",jiffies,vipcount,normalcount);
	blk_run_queue((struct request_queue *)data);
	printk(KERN_INFO "1timeout,j=%lu,vipcount=%u,normalcount=%u\n",jiffies,vipcount,normalcount);
}
static void *rd_init_queue(struct request_queue *q)
{
	struct rd_data *nd;

	nd = kmalloc_node(sizeof(*nd), GFP_KERNEL, q->node);
	if (!nd)
		return NULL;
	nd->q=q;
	INIT_LIST_HEAD(&nd->queue);
	INIT_LIST_HEAD(&nd->vipqueue);
	ndglobal=nd;
	nd->resume_timer.function=resume_timeout;
	nd->resume_timer.data=(unsigned long)q;
	init_timer(&nd->resume_timer);
	return nd;
}

static void rd_exit_queue(struct elevator_queue *e)
{
	struct rd_data *nd = e->elevator_data;

	del_timer_sync(&nd->resume_timer);
	BUG_ON(!(list_empty(&nd->queue)&&list_empty(&nd->vipqueue)));
	kfree(nd);
}

static struct elevator_type elevator_rd = {
	.ops = {
		.elevator_merge_req_fn		= rd_merged_requests,//待改
		.elevator_dispatch_fn		= rd_dispatch,//
		.elevator_add_req_fn		= rd_add_request,//
		.elevator_queue_empty_fn	= rd_queue_empty,//
		.elevator_former_req_fn		= rd_former_request,//
		.elevator_latter_req_fn		= rd_latter_request,//
		.elevator_init_fn		= rd_init_queue,//
		.elevator_exit_fn		= rd_exit_queue,//
	},
	.elevator_name = "rd",
	.elevator_owner = THIS_MODULE,
};
static int rdatoi(const char *s)
{
	int result=0;
	while((*s)>='0'&&(*s)<='9')
	{
		result=result*10+((*s)&0x0f);
		++s;
	}
	if(*s!=0)//非法字符
		return 0;
	return result;
}
static int rdnice_write( struct file *filp, const char __user *buff,
	       	unsigned long len, void *data )
{
	if(len>sizeof(vippidbuf)-1)
		return -EFBIG;
	if (copy_from_user( vippidbuf, buff, len ))
		return -EFAULT;
	vippidbuf[len-1]=0;//无论用户传入串是否以0结尾,保证vippidbuf以0结尾
	if(vippidbuf[0]=='w')
	{
		printk(KERN_INFO "io waking up:j=%lu\n",jiffies);
		blk_run_queue(ndglobal->q);
		return len;
	}
	vippid=rdatoi(vippidbuf);
	printk(KERN_INFO "new vippid=%i,jiffies=%lu\n",vippid,jiffies);
	return len;
}
static int rdnice_read( char *page, char **start, off_t off,int count, int *eof, void *data )
{
	//测试用
	int len;
	//下面这个if不很清楚是干什么用的,貌似off>0表示目标缓冲区占据多页.那么就不处理直接退出
	if (off > 0) {
		*eof = 1;
		return 0;
	}
	len = sprintf(page, "%i\n", vippid);
	return len;
}
static int __init rd_init(void)
{
	int ret=0;
	elv_register(&elevator_rd);
	//初始化proc文件
	proc_entry=create_proc_entry("rdnice",0644,NULL);
	if (proc_entry == NULL) {
		ret = -ENOMEM;
		printk(KERN_INFO "rd-iosched: Couldn't create proc entry\n");
	} else {
		proc_entry->read_proc = rdnice_read;
		proc_entry->write_proc = rdnice_write;
		//proc_entry->owner = THIS_MODULE;
		printk(KERN_INFO "rd-iosched: Module loaded.\n");
	}
	return ret;
}

static void __exit rd_exit(void)
{
	remove_proc_entry("rdnice",NULL);
	elv_unregister(&elevator_rd);
	printk(KERN_INFO "rd-iosched: Module removed.\n");
}

module_init(rd_init);
module_exit(rd_exit);


MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("rd IO scheduler");
