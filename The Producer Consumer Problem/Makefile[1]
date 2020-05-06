# Makefile, ECE252  
# Yiqing Huang, 2018/11/02

CC = gcc 
CFLAGS = -Wall -std=gnu99 -g # "curl-config --cflags" output is empty  
LD = gcc
LDFLAGS = -std=gnu99 -g -pthread
LDLIBS = -lcurl -lz -lrt# "curl-config --libs" output 

SRCS   = paster2.c crc.c zutil.c shm_stack.c
OBJS  = paster2.o zutil.o crc.o shm_stack.o
#OBJS2  = main_write_callback.o
#OBJS3  = main_write_header_cb.o
TARGETS= paster2

all: ${TARGETS}

paster2: $(OBJS) 
	$(LD) -o $@ $^ $(LDLIBS) $(LDFLAGS) 

%.o: %.c 
	$(CC) $(CFLAGS) -c $< 

%.d: %.c
	gcc -MM -MF $@ $<

-include $(SRCS:.c=.d)

.PHONY: clean
clean:
	rm -f *~ *.d *.o $(TARGETS) *.png
	../tools/clean_ipcs.sh
