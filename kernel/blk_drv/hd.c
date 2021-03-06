/**
 * 硬盘驱动
 */
#define MAJOR_NR 3
#include <linux/sched.h>
#include <asm/io.h>
#include <linux/hdreg.h>
#include <asm/system.h>
#include <linux/fs.h>
#include "blk.h"

#define CMOS_READ(addr) ({ \
outb_p(0x80|addr,0x70); \
inb_p(0x71); \
})

#define MAX_ERRORS	7 //每个扇区最大读写失败数
#define MAX_HD		2

static void recal_intr(void);
extern void hd_interrupt(void);

static int recalibrate = 1;
static int reset = 1;

/**
 * 读取温盘请求结果状态
 */
static int win_result(void)
{
	int i=inb_p(HD_STATUS);

	if ((i & (BUSY_STAT | READY_STAT | WRERR_STAT | SEEK_STAT | ERR_STAT))
		== (READY_STAT | SEEK_STAT))
		return(0); /* ok */
	if (i&1) i=inb(HD_ERROR);
	return (1);
}

/**
 * 硬盘在BIOS中的信息
 */
struct hd_i_struct {
	int head,sect,cyl,wpcom,lzone,ctl;
	};
#ifdef HD_TYPE
struct hd_i_struct hd_info[] = { HD_TYPE };
#define NR_HD ((sizeof (hd_info))/(sizeof (struct hd_i_struct)))
#else
struct hd_i_struct hd_info[] = { {0,0,0,0,0,0},{0,0,0,0,0,0} };
static int NR_HD = 0;
#endif

/**
 * 硬盘分区表
 */
static struct hd_struct {
	long start_sect;
	long nr_sects;
} hd[5*MAX_HD]={{0,0},};

#define port_read(port,buf,nr) \
__asm__("cld;rep;insw"::"d" (port),"D" (buf),"c" (nr))

#define port_write(port,buf,nr) \
__asm__("cld;rep;outsw"::"d" (port),"S" (buf),"c" (nr))

void unexpected_hd_interrupt(void)
{
	printk("Unexpected HD interrupt\n\r");
}

/*
 * 循环判断磁盘控制器就绪
 * inb_p(HD_STATUS)返回结果位7为控制器忙位，位6为磁盘就绪
 */
static int controller_ready(void)
{
	int retries=10000;

	while (--retries && (inb_p(HD_STATUS)&0xc0)!=0x40);
	return (retries);
}

static void hd_out(unsigned int drive,unsigned int nsect,unsigned int sect,
		unsigned int head,unsigned int cyl,unsigned int cmd,
		void (*intr_addr)(void))
{
	register int port asm("dx");

	if (drive>1 || head>15)
		panic("Trying to write bad sector");
	if (!controller_ready())
		panic("HD controller not ready");
	do_hd = intr_addr;
	outb_p(hd_info[drive].ctl,HD_CMD);
	port=HD_DATA;
	outb_p(hd_info[drive].wpcom>>2,++port);
	outb_p(nsect,++port);
	outb_p(sect,++port);
	outb_p(cyl,++port);
	outb_p(cyl>>8,++port);
	outb_p(0xA0|(drive<<4)|head,++port);
	outb(cmd,++port);
}

static int drive_busy(void)
{
	unsigned int i;

	for (i = 0; i < 10000; i++)
		if (READY_STAT == (inb_p(HD_STATUS) & (BUSY_STAT|READY_STAT)))
			break;
	i = inb(HD_STATUS);
	i &= BUSY_STAT | READY_STAT | SEEK_STAT;
	if (i == READY_STAT | SEEK_STAT)
		return(0);
	printk("HD controller times out\n\r");
	return(1);
}

static void reset_controller(void)
{
	int	i;

	outb(4,HD_CMD);
	for(i = 0; i < 100; i++) nop();
	outb(hd_info[0].ctl & 0x0f ,HD_CMD);
	if (drive_busy())
		printk("HD-controller still busy\n\r");
	if ((i = inb(HD_ERROR)) != 1)
		printk("HD-controller reset failed: %02x\n\r",i);
}

static void reset_hd(int nr)
{
	reset_controller();
	hd_out(nr,hd_info[nr].sect,hd_info[nr].sect,hd_info[nr].head-1,
		hd_info[nr].cyl,WIN_SPECIFY,&recal_intr);
}

int sys_setup(void * BIOS){
	static int callable = 1;
		int i,drive;
		unsigned char cmos_disks;
		struct partition *p;
		struct buffer_head * bh;

		if (!callable)
			return -1;
		callable = 0;
	#ifndef HD_TYPE
		for (drive=0 ; drive<2 ; drive++) {
			hd_info[drive].cyl = *(unsigned short *) BIOS;
			hd_info[drive].head = *(unsigned char *) (2+BIOS);
			hd_info[drive].wpcom = *(unsigned short *) (5+BIOS);
			hd_info[drive].ctl = *(unsigned char *) (8+BIOS);
			hd_info[drive].lzone = *(unsigned short *) (12+BIOS);
			hd_info[drive].sect = *(unsigned char *) (14+BIOS);
			BIOS += 16;
		}
		if (hd_info[1].cyl)
			NR_HD=2;
		else
			NR_HD=1;
	#endif
		for (i=0 ; i<NR_HD ; i++) {
			hd[i*5].start_sect = 0;
			hd[i*5].nr_sects = hd_info[i].head*
					hd_info[i].sect*hd_info[i].cyl;
		}

		//读取CMOS 0x12的值，判断到底有几个硬盘
		if ((cmos_disks = CMOS_READ(0x12)) & 0xf0)
			if (cmos_disks & 0x0f)
				NR_HD = 2;
			else
				NR_HD = 1;
		else
			NR_HD = 0;
		for (i = NR_HD ; i < 2 ; i++) {
			hd[i*5].start_sect = 0;
			hd[i*5].nr_sects = 0;
		}

		printk("hd0.start_sect->%d\n",hd[0].start_sect);
		printk("hd0.nr_sects->%d\n",hd[0].nr_sects);
		for (drive=0 ; drive<NR_HD ; drive++) {
				if (!(bh = bread(0x300 + drive*5,0))) {
					printk("Unable to read partition table of drive %d\n\r",
						drive);
					panic("");
				}
				if (bh->b_data[510] != 0x55 || (unsigned char)
				    bh->b_data[511] != 0xAA) {
					printk("Bad partition table on drive %d\n\r",drive);
					panic("");
				}
				p = 0x1BE + (void *)bh->b_data;//分区表是从第一个扇区的0x1BE开始的
				for (i=1;i<5;i++,p++) {
					hd[i+5*drive].start_sect = p->start_sect;
					hd[i+5*drive].nr_sects = p->nr_sects;
				}
				brelse(bh);//用完之后释放缓冲区
			}
		printk("hd0[1].start_sect->%d\n",hd[1].start_sect);
		printk("hd0[1].nr_sects->%d\n",hd[1].nr_sects);
		printk("hd0[2].start_sect->%d\n",hd[2].start_sect);
		printk("hd0[2].nr_sects->%d\n",hd[2].nr_sects);
		printk("hd0[3].start_sect->%d\n",hd[3].start_sect);
		printk("hd0[3].nr_sects->%d\n",hd[3].nr_sects);
		if (NR_HD)
			printk("Partition table%s ok.\n\r",(NR_HD>1)?"s":"");
		//TODO 虚拟盘
		mount_root();
		return (0);
}

static void bad_rw_intr(void)
{
	if (++CURRENT->errors >= MAX_ERRORS)//如果请求出错大于MAX,则结束请求
		end_request(0);
	if (CURRENT->errors > MAX_ERRORS/2)//如果已经出错了3次，则先重置磁盘
		reset = 1;
}

static void read_intr(void)
{
	if (win_result()) {
		bad_rw_intr();
		do_hd_request();
		return;
	}
	port_read(HD_DATA,CURRENT->buffer,256);
	CURRENT->errors = 0;
	CURRENT->buffer += 512;
	CURRENT->sector++;
	if (--CURRENT->nr_sectors) {
		do_hd = &read_intr;
		return;
	}
	end_request(1);
	do_hd_request();
}

static void write_intr(void)
{
	if (win_result()) {
		bad_rw_intr();
		do_hd_request();
		return;
	}
	if (--CURRENT->nr_sectors) {
		CURRENT->sector++;
		CURRENT->buffer += 512;
		do_hd = &write_intr;
		port_write(HD_DATA,CURRENT->buffer,256);
		return;
	}
	end_request(1);
	do_hd_request();
}

static void recal_intr(void)
{
	if (win_result())
		bad_rw_intr();
	do_hd_request();
}

/**
 * 处理硬盘读写请求
 */
void do_hd_request(void)
{
	int i,r;
	unsigned int block,dev;
	unsigned int sec,head,cyl;
	unsigned int nsect;

	INIT_REQUEST;
	dev = MINOR(CURRENT->dev);
	block = CURRENT->sector;
	if (dev >= 5*NR_HD || block+2 > hd[dev].nr_sects) {
		end_request(0);
		goto repeat;
	}
	block += hd[dev].start_sect;
	dev /= 5;
	__asm__("divl %4":"=a" (block),"=d" (sec):"0" (block),"1" (0),
		"r" (hd_info[dev].sect));
	__asm__("divl %4":"=a" (cyl),"=d" (head):"0" (block),"1" (0),
		"r" (hd_info[dev].head));
	sec++;
	nsect = CURRENT->nr_sectors;
	if (reset) {
		reset = 0;
		recalibrate = 1;
		reset_hd(CURRENT_DEV);
		return;
	}
	if (recalibrate) {
		recalibrate = 0;
		hd_out(dev,hd_info[CURRENT_DEV].sect,0,0,0,
			WIN_RESTORE,&recal_intr);
		return;
	}
	if (CURRENT->cmd == WRITE) {
		hd_out(dev,nsect,sec,head,cyl,WIN_WRITE,&write_intr);
		for(i=0 ; i<3000 && !(r=inb_p(HD_STATUS)&DRQ_STAT) ; i++)
			/* nothing */ ;
		if (!r) {
			bad_rw_intr();
			goto repeat;
		}
		port_write(HD_DATA,CURRENT->buffer,256);
	} else if (CURRENT->cmd == READ) {
		hd_out(dev,nsect,sec,head,cyl,WIN_READ,&read_intr);
	} else
		panic("unknown hd-command");
}

void hd_init(void)
{
	blk_dev[MAJOR_NR].request_fn = DEVICE_REQUEST;
	set_intr_gate(0x2E,&hd_interrupt);//设置硬盘中断
	outb_p(inb_p(0x21)&0xfb,0x21);//允许从8259A发送中断
	outb(inb_p(0xA1)&0xbf,0xA1);//允许硬盘发送中断
}
