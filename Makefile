# minidvd - tiny DVD player (static binary with uClibc)
#
#   make              configure + build the bundled libraries, then minidvd
#   make libs         only build the bundled libraries
#   make clean        remove minidvd objects/binary
#   make libs-clean   run 'make clean' in every bundled library
#   make distclean    clean + remove all configure output from the libraries
#
# Overridable on the command line, e.g.
#   make CC=/opt/uclibc/bin/i386-linux-gcc HOST=i386-linux-uclibc
#
#   CC          compiler used for minidvd AND the bundled libraries
#   HOST        autoconf --host triplet, only needed when cross compiling
#   LIB_CFLAGS  CFLAGS for the bundled libraries
#   LIB_LDFLAGS LDFLAGS for the bundled libraries
#   X11_LIBS    X11 link line (defaults to pkg-config xrender xext x11, else -lXrender -lXext -lX11)
#   STATIC      '-static' to link statically; default only for uClibc compilers
#   SYNC        'old' builds the previous A/V timing (see minidvd.c)

CC ?= gcc
ifeq ($(origin CC),default)
CC = gcc
endif

BINDIR = /usr/bin
HOST   ?=

TARGET_MACHINE := $(shell $(CC) -dumpmachine)

ifeq ($(findstring x86_64,$(TARGET_MACHINE)),x86_64)

ELF_FORMAT = elf64-x86-64
ELF_ARCH   = i386:x86-64

else ifneq ($(findstring i386,$(TARGET_MACHINE)),)

ELF_ARCH   = i386
CFLAGS += -fno-builtin-pow -fno-builtin-exp

else ifneq ($(findstring i686,$(TARGET_MACHINE)),)

ELF_FORMAT = elf32-i386
ELF_ARCH   = i386

else

$(error Unsupported target: $(TARGET_MACHINE))

endif

# uClibc: fully static binary, and its clock_gettime lacks CLOCK_MONOTONIC.
# Everything else (glibc, ...): normal dynamically linked binary.
ifneq ($(findstring uclibc,$(TARGET_MACHINE)),)
STATIC ?= -static
CFLAGS += -DCLOCK_MONOTONIC=CLOCK_REALTIME
else
STATIC ?=
endif

# ---------------------------------------------------------------------------
# Bundled libraries
# ---------------------------------------------------------------------------

DVDCSS_DIR  = libdvdcss-1.4.3
DVDREAD_DIR = libdvdread-5.0.3
MPEG2_DIR   = libmpeg2-0.5.1
A52_DIR     = a52dec-0.8.0

DVDCSS_A       = $(DVDCSS_DIR)/.libs/libdvdcss.a
DVDREAD_A      = $(DVDREAD_DIR)/.libs/libdvdread.a
MPEG2_A        = $(MPEG2_DIR)/libmpeg2/.libs/libmpeg2.a
MPEG2CONVERT_A = $(MPEG2_DIR)/libmpeg2/convert/.libs/libmpeg2convert.a
A52_A          = $(A52_DIR)/liba52/.libs/liba52.a

# link order matters for static archives: users before providers
LIB_ARCHIVES = $(DVDREAD_A) $(DVDCSS_A) $(MPEG2CONVERT_A) $(MPEG2_A) $(A52_A)

LIB_CFLAGS  ?= -pipe -Os -Wall -D_BSD_SOURCE -D_GNU_SOURCE -D__KERNEL_STRICT_NAMES \
               -ffunction-sections -fdata-sections
LIB_LDFLAGS ?= $(STATIC) -Wl,--gc-sections,--sort-common,-s

# Common configure invocation for all libraries (run inside the lib dir).
CONFIGURE = CC="$(CC)" CFLAGS="$(LIB_CFLAGS)" LDFLAGS="$(LIB_LDFLAGS)" \
            $(SHELL) ./configure --prefix=/usr --disable-silent-rules \
            --disable-shared --enable-static $(if $(HOST),--host=$(HOST))

# git checkouts scramble timestamps; never let the generated Makefiles try
# to re-run aclocal/automake/autoconf/autoheader.
NO_AUTOTOOLS = ACLOCAL=true AUTOMAKE=true AUTOCONF=true AUTOHEADER=true

# Anything that changes how the libraries are configured goes in here. When it
# changes, the libraries are reconfigured automatically.
LIB_CONFIG_SIG = $(CC)|$(HOST)|$(LIB_CFLAGS)|$(LIB_LDFLAGS)
LIB_STAMP_SIG  = .minidvd-config
LIB_STAMP      = .minidvd-configured

# ---------------------------------------------------------------------------
# minidvd
# ---------------------------------------------------------------------------

SRCS = minidvd.c audio/audio.c audio/pcm.c

# audio/audio.c: backends + tinyalsa buffer geometry used by minidvd
# (period 4096 frames x 4, start after 8192 frames, stop below 16384)
AUDIO_CFLAGS = -DAUDIO_HAVE_TINYALSA -DAUDIO_HAVE_OSS \
		-DAUDIO_TINYALSA_PERIOD_SIZE=4096 -DAUDIO_TINYALSA_PERIOD_COUNT=4 \
		-DAUDIO_TINYALSA_START_THRESHOLD=8192 -DAUDIO_TINYALSA_STOP_THRESHOLD=16384

OBJS= $(SRCS:.c=.o)

# A/V timing: 'make' = frames shown at their PTS against an audio-locked clock,
# 'make SYNC=old' = previous behaviour (video paced by blocking audio writes).
# Run 'make clean' when switching.
SYNC ?= new
ifeq ($(SYNC),old)
SYNC_CFLAGS = -DSYNC_OLD
endif

TARGET = minidvd

CFLAGS += -Wall -Wunused -std=gnu99 -I./$(DVDREAD_DIR)/src -I./ \
		-I./$(MPEG2_DIR)/include -I./$(A52_DIR)/include -I./audio \
		-Os -ffunction-sections -fdata-sections -Wno-deprecated-declarations \
		-ffast-math -fno-math-errno \
		$(AUDIO_CFLAGS) $(SYNC_CFLAGS) $(EXTRA_CFLAGS)

X11_LIBS ?= $(shell pkg-config $(if $(STATIC),--static) --libs xrender xext x11 2>/dev/null || echo -lXrender -lXext -lX11)

LDFLAGS = $(STATIC) -Wl,--gc-sections,--sort-common,-s
LDLIBS  = $(LIB_ARCHIVES) $(X11_LIBS) -lm

.PHONY: all libs clean libs-clean distclean install test-ifo FORCE

all: $(TARGET)

$(TARGET): $(OBJS) $(LIB_ARCHIVES)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)

# library headers must be in place (and configured) before compiling
$(OBJS): | $(DVDCSS_DIR)/$(LIB_STAMP) $(DVDREAD_DIR)/$(LIB_STAMP) \
           $(MPEG2_DIR)/$(LIB_STAMP) $(A52_DIR)/$(LIB_STAMP)

libs: $(LIB_ARCHIVES)

# --- configuration signature: rewritten only when the settings change ------

%/$(LIB_STAMP_SIG): FORCE
	@echo '$(LIB_CONFIG_SIG)' | cmp -s - $@ || echo '$(LIB_CONFIG_SIG)' > $@

# --- libdvdcss ---------------------------------------------------------------

$(DVDCSS_DIR)/$(LIB_STAMP): $(DVDCSS_DIR)/$(LIB_STAMP_SIG)
	cd $(DVDCSS_DIR) && $(CONFIGURE) ac_cv_func_dlopen=no
	touch $@

$(DVDCSS_A): $(DVDCSS_DIR)/$(LIB_STAMP) FORCE
	$(MAKE) -C $(DVDCSS_DIR) $(NO_AUTOTOOLS) libdvdcss.la

# --- libdvdread (linked directly against the bundled libdvdcss) ---------------

$(DVDREAD_DIR)/$(LIB_STAMP): $(DVDREAD_DIR)/$(LIB_STAMP_SIG) $(DVDCSS_DIR)/$(LIB_STAMP)
	cd $(DVDREAD_DIR) && CPPFLAGS="-I$(CURDIR)/$(DVDCSS_DIR)/src" \
		$(CONFIGURE) --with-libdvdcss \
		CSS_CFLAGS="-I$(CURDIR)/$(DVDCSS_DIR)/src" \
		CSS_LIBS="-L$(CURDIR)/$(DVDCSS_DIR)/.libs -ldvdcss"
	touch $@

# libdvdread links against the bundled libdvdcss (CSS_LIBS): build that first
$(DVDREAD_A): $(DVDREAD_DIR)/$(LIB_STAMP) $(DVDCSS_A) FORCE
	$(MAKE) -C $(DVDREAD_DIR) $(NO_AUTOTOOLS) libdvdread.la

# --- libmpeg2 + libmpeg2convert ---------------------------------------------

$(MPEG2_DIR)/$(LIB_STAMP): $(MPEG2_DIR)/$(LIB_STAMP_SIG)
	cd $(MPEG2_DIR) && $(CONFIGURE) --disable-sdl --without-x
	touch $@

$(MPEG2_A): $(MPEG2_DIR)/$(LIB_STAMP) FORCE
	$(MAKE) -C $(MPEG2_DIR)/libmpeg2 $(NO_AUTOTOOLS)

# built by the same sub-make as libmpeg2.a (one recipe, safe with -j)
$(MPEG2CONVERT_A): $(MPEG2_A) ;

# --- liba52 -------------------------------------------------------------------

$(A52_DIR)/$(LIB_STAMP): $(A52_DIR)/$(LIB_STAMP_SIG)
	cd $(A52_DIR) && $(CONFIGURE)
	touch $@

$(A52_A): $(A52_DIR)/$(LIB_STAMP) FORCE
	$(MAKE) -C $(A52_DIR)/liba52 $(NO_AUTOTOOLS)

# ---------------------------------------------------------------------------

# unit test: title map (cells/chapters) built from in-memory IFO tables
test-ifo: $(LIB_ARCHIVES)
	$(CC) $(CFLAGS) -Wno-pointer-sign -o tests/ifo_map_test tests/ifo_map_test.c \
		audio/audio.c audio/pcm.c $(LDLIBS)
	./tests/ifo_map_test

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/

clean:
	rm -f $(TARGET) $(OBJS) tests/ifo_map_test

libs-clean:
	for d in $(DVDCSS_DIR) $(DVDREAD_DIR) $(MPEG2_DIR) $(A52_DIR); do \
		if [ -f $$d/Makefile ]; then $(MAKE) -C $$d $(NO_AUTOTOOLS) clean; fi; \
	done

distclean: clean
	for d in $(DVDCSS_DIR) $(DVDREAD_DIR) $(MPEG2_DIR) $(A52_DIR); do \
		if [ -f $$d/Makefile ]; then $(MAKE) -C $$d $(NO_AUTOTOOLS) distclean; fi; \
		rm -f $$d/$(LIB_STAMP) $$d/$(LIB_STAMP_SIG); \
	done

FORCE:
