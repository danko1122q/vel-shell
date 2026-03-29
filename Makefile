# Makefile for vel - Transparent Build Version
CC      = gcc
# Pindah ke C11 jika kamu mau coba fitur baru, atau tetap C99
CFLAGS  = -Wall -Wextra -O2 -std=c11 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lm

ifeq ($(READLINE),1)
  CFLAGS  += -DVEL_USE_READLINE
  LDFLAGS += -lreadline
endif

SRCS = main.c vel_cmd.c vel_sys.c vel_jobs.c vel_run.c vel_lex.c \
       vel_expr.c vel_mem.c vel_map.c vel_tmpl.c vel_extra.c
OBJS = $(SRCS:.c=.o)
TARGET = vel

# Configuration
PREFIX ?= /usr/local
BINDIR  = $(DESTDIR)$(PREFIX)/bin

all: $(TARGET)

# Menampilkan informasi build di awal
$(TARGET): $(OBJS)
	@echo "--------------------------------------"
	@echo "Linking $(TARGET) with $(LDFLAGS)..."
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Build complete: $(TARGET)"
	@echo "--------------------------------------"

# Compile objects (Hapus @ agar baris perintah terlihat)
%.o: %.c vel.h vel_priv.h vel_jobs.h
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