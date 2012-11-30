####################################
# Makefile for playos's kernel     #
# author:ke.chenk                  #
# mail:lingqi1818@gmail.com        # 
####################################

LD	=ld
LD_FLAGS	=-x -r
CPP	=gcc -E -nostdinc -I../include
OBJS=	traps.o printk.o vsprintf.o
CC		= gcc
## -fno-stack-protector去掉对栈的保护，否则可能出现：undefined reference to `__stack_chk_fail'
CFLAGS		= -c -fno-stack-protector 
.c.o:
	$(CC) $(CFLAGS) \
	-nostdinc -fno-builtin -I../include -O -o $*.o $<
	
kernel.o: $(OBJS)
	$(LD) ${LD_FLAGS} -o kernel.o $(OBJS)
	
clean:
	rm -rf $(OBJS)
	
dep:
	sed '/\#\#\# Dependencies/q' < Makefile > tmp_make
	(for i in *.c;do $(CPP) -M $$i;done) >> tmp_make
	cp tmp_make Makefile
	
### Dependencies:
printk.o: printk.c ../include/stdarg.h
traps.o: traps.c ../include/asm/system.h
vsprintf.o: vsprintf.c ../include/stdarg.h ../include/string.h