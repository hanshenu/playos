/**
 * 进程调度实现
 */
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>
#include <sys/types.h>
#include <asm/io.h>
#include <signal.h>
#include <linux/sys.h>

#define _S(nr) (1<<((nr)-1))
#define _BLOCKABLE (~(_S(SIGKILL) | _S(SIGSTOP)))


void show_task(int nr,struct task_struct * p)
{
	int i,j = 4096-sizeof(struct task_struct);

	//printk("%d: pid=%d, state=%d, ",nr,p->pid,p->state);
	i=0;
	while (i<j && !((char *)(p+1))[i])
		i++;
	printk("%d (of %d) chars free in kernel stack\n\r",i,j);
}

void show_stat(void)
{
	int i;

	for (i=0;i<NR_TASKS;i++)
		if (task[i])
			show_task(i,task[i]);
}

#define LATCH (1193180/HZ)

union task_union {
	struct task_struct task;
	char stack[PAGE_SIZE];
};
//每个任务都定义了4K的栈空间

long volatile jiffies = 0;
long startup_time=0;

static union task_union init_task = { INIT_TASK, };
static union task_union test_task_1 = { TEST_TASK_1, };
struct task_struct * task[NR_TASKS] = { &(init_task.task), &(test_task_1.task) };
long user_stack[PAGE_SIZE >> 2];

struct {
	long * a;
	short b;
} stack_start = { &user_stack[PAGE_SIZE >> 2], 0x10 };

struct task_struct *current = &(init_task.task);
struct task_struct *last_task_used_math = NULL;
//时钟中断和系统调用，定义在system_call.s中
extern int timer_interrupt(void);
extern int system_call(void);
extern void user_write_char(char);
//调度初始化
void sched_init(void) {
	int i;
	struct desc_struct * p;
	//设置init进程的tss和ldt
	set_tss_desc(gdt+FIRST_TSS_ENTRY, &(init_task.task.tss));
	set_ldt_desc(gdt+FIRST_LDT_ENTRY, &(init_task.task.ldt));
	p = gdt + 2 + FIRST_TSS_ENTRY; //p指向tss1
	//初始化任务的tss和idt都为0
	//for(i=1;i<NR_TASKS;i++) {
	for (i = 2; i < NR_TASKS; i++) {
		task[i] = NULL;
		p->a = p->b = 0;
		p++;
		p->a = p->b = 0;
		p++;
	}
	void init_task1();
	init_task1();

	/*
	 *置NT标志为0
	 *嵌套任务标志NT(Nested Task)
	 *嵌套任务标志NT用来控制中断返回指令IRET的执行。具体规定如下：
	 *(1)、当NT=0，用堆栈中保存的值恢复EFLAGS、CS和EIP，执行常规的中断返回操作；
	 *(2)、当NT=1，通过任务转换实现中断返回。
	 */
	__asm__("pushfl ; andl $0xffffbfff,(%esp) ; popfl");
	ltr(0);	//加载tss0地址到tr寄存器
	lldt(0);	//加载ldt0地址到ldt寄存器
	//设置8253定时器
	outb_p(0x36, 0x43);
	//通道0，工作方式3
	//1193180为8253的工作频率，这里需要每10毫秒响应一次，
	//那么将数字设置为1193180/100
	outb_p(LATCH & 0xff, 0x40);
	//定时值低字节
	outb(LATCH >> 8, 0x40);
	//定时值高字节
	//void time_print();
	set_intr_gate(0x20, &timer_interrupt);
	//设置时钟中断门
	//set_intr_gate(0x20,&time_print);
	outb(inb_p(0x21)&~0x01, 0x21);
	//允许时钟中断
	set_system_gate(0x80, &system_call);
	//设置系统调用中断门
}

//void time_print() {
//	__asm__("movb $0x20,%%al\n\t"
//			"outb %%al,$0x20"::);
//	write_char('t');
//}
void init_task1() {
	set_tss_desc(gdt+2+FIRST_TSS_ENTRY, &(test_task_1.task.tss));
	set_ldt_desc(gdt+2+FIRST_LDT_ENTRY, &(test_task_1.task.ldt));
	void task1();
	task[1]->tss.eip = &task1;
	task[1]->tss.eflags = 0x200;
	task[1]->state = TASK_RUNNING;
}

void task1() {
//	while (1)
	//user_write_char('v');
		printf("v");
		while (1){}
}


void sleep_on(struct task_struct **p)
{
	struct task_struct *tmp;

	if (!p)
		return;
	if (current == &(init_task.task))
		panic("task[0] trying to sleep");
	tmp = *p;
	*p = current;
	current->state = TASK_UNINTERRUPTIBLE;
	schedule();
	if (tmp)
		tmp->state=0;
}

void interruptible_sleep_on(struct task_struct **p)
{
	struct task_struct *tmp;

	if (!p)
		return;
	if (current == &(init_task.task))
		panic("task[0] trying to sleep");
	tmp=*p;
	*p=current;
repeat:	current->state = TASK_INTERRUPTIBLE;
	schedule();
	if (*p && *p != current) {
		(**p).state=0;
		goto repeat;
	}
	*p=NULL;
	if (tmp)
		tmp->state=0;
}

void wake_up(struct task_struct **p) {
	if (p && *p) {
		(**p).state = 0;
		*p = NULL;
	}
}

void do_timer(long cpl) {
//	extern int beepcount;
//	extern void sysbeepstop(void);
//
//	if (beepcount)
//		if (!--beepcount)
//			sysbeepstop();

	if (cpl)
		current->utime++;
	else
		current->stime++;
	/* 注销部分为0.11的定时器代码
	 if (next_timer) {
	 next_timer->jiffies--;
	 while (next_timer && next_timer->jiffies <= 0) {
	 void (*fn)(void);

	 fn = next_timer->fn;
	 next_timer->fn = NULL;
	 next_timer = next_timer->next;
	 (fn)();
	 }
	 }
	 if (current_DOR & 0xf0)
	 do_floppy_timer();*/
	if ((--current->counter) > 0)
		return;
	current->counter = 0;
	if (!cpl)
		return;
	schedule();
}

void schedule(void) {
	int i, next, c;
	struct task_struct ** p;

	/* check alarm, wake up any interruptible tasks that have got a signal */

	for (p = &LAST_TASK; p > &FIRST_TASK; --p)
		if (*p) {
			if ((*p)->alarm && (*p)->alarm < jiffies) {	//报警时间已经到了，因为小于滴答数（jiffies）
				(*p)->signal |= (1 << (SIGALRM - 1));
				(*p)->alarm = 0;
			}
			if (((*p)->signal & ~(_BLOCKABLE & (*p)->blocked))
					&& (*p)->state == TASK_INTERRUPTIBLE)
				(*p)->state = TASK_RUNNING;
		}

	/* this is the scheduler proper: */
//被注释都两行逻辑为task[0]不参与进程调度
	while (1) {
		c = -1;
		next = 0;
		i = NR_TASKS;
		p = &task[NR_TASKS];
		//while (--i) {
		while (i--) {
			if (!*--p)
				continue;
			if ((*p)->state == TASK_RUNNING && (*p)->counter > c)
				c = (*p)->counter, next = i;
		}
		if (c)
			break;
		//for(p = &LAST_TASK ; p > &FIRST_TASK ; --p)
		for (p = &LAST_TASK; p >= &FIRST_TASK; --p)
			if (*p)
				(*p)->counter = ((*p)->counter >> 1) + (*p)->priority;
	}
	switch_to(next);
}

