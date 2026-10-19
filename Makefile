CC = gcc

BINDIR = /usr/bin

TARGET_MACHINE := $(shell $(CC) -dumpmachine)

ifeq ($(findstring x86_64,$(TARGET_MACHINE)),x86_64)

ELF_FORMAT = elf64-x86-64
ELF_ARCH   = i386:x86-64

else ifneq ($(findstring i386,$(TARGET_MACHINE)),)
	
ELF_ARCH   = i386
STATIC = -static
CFLAGS += -fno-builtin-pow -fno-builtin-exp -DCLOCK_MONOTONIC=CLOCK_REALTIME

else ifneq ($(findstring i686,$(TARGET_MACHINE)),)

ELF_FORMAT = elf32-i386
ELF_ARCH   = i386

else

$(error Unsupported target: $(TARGET_MACHINE))

endif

#audio/audio.c

SRCS = minidvd.c audio/pcm.c

OBJS= $(SRCS:.c=.o)

TARGET = minidvd

CFLAGS += -Wall -Wunused -std=gnu99 -I./libdvdread-5.0.3/src -I./ \
		-I./libmpeg2-0.5.1/include -I./a52dec-0.8.0/include -I./audio \
		-Os -ffunction-sections -fdata-sections -Wno-deprecated-declarations \
		-ffast-math -fno-math-errno -DCLOCK_MONOTONIC=CLOCK_REALTIME -DLOGGING

LDFLAGS =  -static  -Wl,--gc-sections,--sort-common,-s ./libdvdread-5.0.3/.libs/libdvdread.a \
			./libmpeg2-0.5.1/libmpeg2/.libs/libmpeg2.a \
			./libmpeg2-0.5.1/libmpeg2/convert/.libs/libmpeg2convert.a \
			./a52dec-0.8.0/liba52/.libs/liba52.a \
			./libdvdcss-1.4.3/.libs/libdvdcss.a -lX11 -lm
			
.PHONY: all clean install

all:  $(TARGET)

$(TARGET): $(OBJS) 
	$(CC) -o $@ $^ $(LDFLAGS) $(CFLAGS)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/
	
clean:
	rm -f $(TARGET) $(OBJS)
