# Makefile for vel - Minimalist & Fast Version
CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c99 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L
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

# Compile binary
$(TARGET): $(OBJS)
	@echo "Linking $(TARGET)..."
	@$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Compile objects
%.o: %.c vel.h vel_priv.h vel_jobs.h
	@$(CC) $(CFLAGS) -c -o $@ $<

# INSTALL: Cuma copy file ke /usr/local/bin (Sangat Cepat)
install: $(TARGET)
	@echo "Installing $(TARGET) to $(BINDIR)..."
	@install -Dm755 $(TARGET) $(BINDIR)/vel
	@echo "Success! You can now run 'vel' from terminal."

# UNINSTALL: Cuma hapus file
uninstall:
	@echo "Removing $(TARGET) from $(BINDIR)..."
	@rm -f $(BINDIR)/vel
	@echo "Uninstalled."

clean:
	@rm -f $(OBJS) $(TARGET)
	@echo "Cleaned up."

.PHONY: all install uninstall clean