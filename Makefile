# Makefile for vel - Transparent Build Version
CC      = gcc
SRCDIR  = src
INCDIR  = include
# Pindah ke C11 jika kamu mau coba fitur baru, atau tetap C99
CFLAGS  = -Wall -Wextra -O2 -std=c11 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -I$(INCDIR)
LDFLAGS = -lm

ifeq ($(READLINE),1)
  CFLAGS  += -DVEL_USE_READLINE
  LDFLAGS += -lreadline
endif

SRCS = $(SRCDIR)/main.c $(SRCDIR)/vel_cmd.c $(SRCDIR)/vel_sys.c $(SRCDIR)/vel_jobs.c \
       $(SRCDIR)/vel_run.c $(SRCDIR)/vel_lex.c $(SRCDIR)/vel_expr.c $(SRCDIR)/vel_mem.c \
       $(SRCDIR)/vel_map.c $(SRCDIR)/vel_tmpl.c $(SRCDIR)/vel_extra.c $(SRCDIR)/vel_newcmds.c
OBJS = $(SRCS:.c=.o)
TARGET = vel

# Configuration
PREFIX ?= /usr/local
BINDIR  = $(DESTDIR)$(PREFIX)/bin

HEADERS = $(INCDIR)/vel.h $(INCDIR)/vel_priv.h $(INCDIR)/vel_jobs.h

all: $(TARGET)

# Menampilkan informasi build di awal
$(TARGET): $(OBJS)
	@echo "--------------------------------------"
	@echo "Linking $(TARGET) with $(LDFLAGS)..."
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Build complete: $(TARGET)"
	@echo "--------------------------------------"

# Compile objects (Hapus @ agar baris perintah terlihat)
$(SRCDIR)/%.o: $(SRCDIR)/%.c $(HEADERS)
	$(CC) $(CFLAGS) -c -o $@ $<

# INSTALL
install: $(TARGET)
	@echo "Installing $(TARGET) to $(BINDIR)..."
	install -Dm755 $(TARGET) $(BINDIR)/vel
	@echo "Success! You can now run 'vel' from terminal."

# UNINSTALL
uninstall:
	@echo "Removing $(TARGET) from $(BINDIR)..."
	rm -f $(BINDIR)/vel
	@echo "Uninstalled."

clean:
	@echo "Cleaning objects and binary..."
	rm -f $(OBJS) $(TARGET)
	@echo "Cleaned up."

.PHONY: all install uninstall clean
