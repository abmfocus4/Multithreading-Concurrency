
CC = gcc
CFLAGS = -Wall -std=gnu99 -g # "curl-config --cflags" output is empty
LD = gcc
LDFLAGS = -std=gnu99 -g
LDLIBS = -lcurl -lz -pthread# "curl-config --libs" output

SRCS   = paster.c crc.c zutil.c
OBJS  = paster.o zutil.o crc.o
#OBJS2  = main_write_callback.o
#OBJS3  = main_write_header_cb.o
TARGETS= paster

all: ${TARGETS}

paster: $(OBJS)
	$(LD) -o $@ $^ $(LDLIBS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $<

%.d: %.c
	gcc -MM -MF $@ $<

-include $(SRCS:.c=.d)

.PHONY: clean
clean:
	rm -f *~ *.d *.o $(TARGETS) *.png
