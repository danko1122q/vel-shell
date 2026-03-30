/*Copyright (c) 2026, danko1122q
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#ifndef WIN32
#  include <unistd.h>
#  include <sys/wait.h>
#  include <sys/stat.h>
#endif
#ifdef __MINGW32__
#  undef __STRICT_ANSI__
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include "vel.h"
#include "vel_jobs.h"

/* ---- optional readline support ---- */
#ifdef VEL_USE_READLINE
#  include <readline/readline.h>
#  include <readline/history.h>
#  define HAS_READLINE 1
#else
#  define HAS_READLINE 0
#endif

/* ---- ANSI color codes ---- */
static int g_color = 0;

#define COL_RESET   (g_color ? "\033[0m"  : "")
#define COL_CYAN    (g_color ? "\033[36m" : "")
#define COL_GREEN   (g_color ? "\033[32m" : "")
#define COL_YELLOW  (g_color ? "\033[33m" : "")
#define COL_RED     (g_color ? "\033[31m" : "")
#define COL_BOLD    (g_color ? "\033[1m"  : "")

/* ---- globals ---- */
static int running   = 1;
static int exit_code = 0;
extern int g_last_exit;

static VELCB void vel_on_exit(vel_t vel, vel_val_t val)
{
    (void)vel;
    running   = 0;
    exit_code = (int)vel_int(val);
}

/* ============================================================
 * Native commands
 * ========================================================= */

static VELCB vel_val_t cmd_writechar(vel_t vel, size_t argc, vel_val_t *argv)
{
    (void)vel;
    if (!argc) return NULL;
    printf("%c", (char)vel_int(argv[0]));
    return NULL;
}

static VELCB vel_val_t cmd_canread(vel_t vel, size_t argc, vel_val_t *argv)
{
    (void)vel; (void)argc; (void)argv;
    return (feof(stdin) || ferror(stdin)) ? NULL : vel_val_int(1);
}

static VELCB vel_val_t cmd_readline(vel_t vel, size_t argc, vel_val_t *argv)
{
    size_t    len = 0, cap = 64;
    char     *buf;
    int       ch;
    vel_val_t r;
    (void)argc; (void)argv;

    if (feof(stdin)) { vel_error_set(vel, "end of file"); return NULL; }

    buf = malloc(cap);
    for (;;) {
        ch = fgetc(stdin);
        if (ch == EOF) {
            if (ferror(stdin)) vel_error_set(vel, "stdin read error");
            break;
        }
        if (ch == '\r') continue;
        if (ch == '\n') break;
        if (len + 1 >= cap) { cap += 64; buf = realloc(buf, cap); }
        buf[len++] = (char)ch;
    }
    buf[len] = '\0';
    r = vel_val_str(buf);
    free(buf);
    return r;
}

#if !defined(WIN32) && !defined(WATCOMC)
/*
 * run_system — capture output dari external command menggunakan fork+exec+pipe.
 *
 * FIX 4: timeout mechanism via SIGALRM.
 *   Versi lama tidak memiliki batas waktu, sehingga command seperti
 *   "system yes" akan hang selamanya.  Sekarang:
 *   - Default timeout: 30 detik (VEL_SYSTEM_TIMEOUT_SEC).
 *   - Dapat di-override lewat environment variable VEL_SYSTEM_TIMEOUT
 *     (nilai 0 = tanpa batas, seperti perilaku lama).
 *   - Ketika waktu habis, child di-SIGKILL dan fungsi mengembalikan output
 *     yang sudah terkumpul sampai saat itu (bisa berupa string kosong).
 *   - g_last_exit di-set ke -1 saat timeout.
 */
#ifndef VEL_SYSTEM_TIMEOUT_SEC
#  define VEL_SYSTEM_TIMEOUT_SEC 30
#endif

/* FIX 4: globals for the SIGALRM timeout in run_system.
 * volatile sig_atomic_t is the only type safe to write from a signal handler.
 *
 * BUG 1 FIX: g_run_child_pid was volatile pid_t. pid_t is a signed integer
 * type whose width is implementation-defined; on some platforms it is wider
 * than sig_atomic_t. POSIX only guarantees that sig_atomic_t can be read and
 * written atomically from signal handlers. Using pid_t here is UB if the
 * signal fires during a non-atomic store. Changed to volatile sig_atomic_t;
 * all uses cast to pid_t when passed to kill() / waitpid(). */
static volatile sig_atomic_t g_run_timed_out = 0;
static volatile sig_atomic_t g_run_child_pid  = -1;

static void run_system_alarm_handler(int sig)
{
    (void)sig;
    g_run_timed_out = 1;
    if (g_run_child_pid > 0)
        kill((pid_t)g_run_child_pid, SIGKILL);
}

static char *run_system(size_t argc, const char **argv)
{
    int    pipefd[2];
    pid_t  pid;
    int    status;
    char  *out  = NULL;
    size_t size = 0;

    if (!argc || !argv || !argv[0]) return NULL;

    if (pipe(pipefd) != 0) return NULL;

    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return NULL;
    }

    if (pid == 0) {
        /* child: redirect stdout to pipe, exec directly */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    /* parent: set up timeout, read output */
    close(pipefd[1]);

    {
        /* FIX 4: compute timeout from environment or compile-time default.
         * Strategy: install a SIGALRM handler that kills the child, then
         * set alarm().  When alarm fires, the handler kills the child, which
         * closes the write-end of the pipe, causing read() to return 0 and
         * exit normally — no EINTR needed.  This is simpler and avoids the
         * SA_RESTART / EINTR ambiguity. */
        int timeout_sec = VEL_SYSTEM_TIMEOUT_SEC;
        const char *env_t = getenv("VEL_SYSTEM_TIMEOUT");
        if (env_t) timeout_sec = atoi(env_t);

        /* Store pid in a file-scope variable so the signal handler can reach it.
         * IMPORTANT: reset g_run_timed_out BEFORE arming the alarm so a stale
         * flag from a previous interrupted call cannot cause a false timeout.
         *
         * RACE FIX: block SIGALRM while we write g_run_child_pid and reset
         * g_run_timed_out.  If SIGALRM was already pending from a previous
         * interrupted call its handler would see g_run_child_pid still set to
         * the OLD child's pid (or -1) before we overwrite it, causing either a
         * spurious kill of the wrong process or a missed kill of this one.
         * Blocking the signal for the brief critical section prevents that. */
        {
            sigset_t block_alrm, old_mask;
            sigemptyset(&block_alrm);
            sigaddset(&block_alrm, SIGALRM);
            sigprocmask(SIG_BLOCK, &block_alrm, &old_mask);

            g_run_child_pid = (sig_atomic_t)pid;
            g_run_timed_out = 0;   /* reset BEFORE alarm() */

            sigprocmask(SIG_SETMASK, &old_mask, NULL);
        }

        if (timeout_sec > 0) {
            struct sigaction sa_alrm;
            memset(&sa_alrm, 0, sizeof(sa_alrm));
            sa_alrm.sa_handler = run_system_alarm_handler;
            sigemptyset(&sa_alrm.sa_mask);
            sa_alrm.sa_flags = 0; /* do NOT set SA_RESTART — let read() return */
            sigaction(SIGALRM, &sa_alrm, NULL);
            alarm((unsigned int)timeout_sec);
        }

        {
            char    buf[4096];
            ssize_t n;

            while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
                out = realloc(out, size + (size_t)n);
                memcpy(out + size, buf, (size_t)n);
                size += (size_t)n;
            }

            alarm(0); /* cancel any pending alarm */
            /* restore SIGALRM to default so it doesn't affect later code */
            {
                struct sigaction sa_dfl;
                memset(&sa_dfl, 0, sizeof(sa_dfl));
                sa_dfl.sa_handler = SIG_DFL;
                sigemptyset(&sa_dfl.sa_mask);
                sigaction(SIGALRM, &sa_dfl, NULL);
            }

            if (g_run_timed_out) {
                fprintf(stderr,
                        "vel: system: command '%s' timed out after %d second(s)"
                        " (set VEL_SYSTEM_TIMEOUT=0 to disable)\n",
                        argv[0], timeout_sec);
                g_last_exit     = -1;
                g_run_timed_out = 0;
                /* free partial output — timed-out output is unreliable */
                free(out);
                out  = NULL;
                size = 0;
            }
        }
        g_run_child_pid = -1;
    }
    close(pipefd[0]);

    waitpid((pid_t)pid, &status, 0);
    if (g_last_exit != -1)   /* don't overwrite timeout sentinel */
        g_last_exit = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    if (!out) { out = malloc(1); size = 0; }
    out = realloc(out, size + 1);
    out[size] = '\0';
    return out;
}
#endif

static VELCB vel_val_t cmd_system(vel_t vel, size_t argc, vel_val_t *argv)
{
    (void)vel;
#if defined(WIN32) || defined(WATCOMC)
    (void)argc; (void)argv; return NULL;
#else
    const char **sargv;
    vel_val_t    r = NULL;
    char        *out;
    size_t       i;
    if (!argc) return NULL;
    sargv = malloc(sizeof(char *) * (argc + 1));
    for (i = 0; i < argc; i++) sargv[i] = vel_str(argv[i]);
    sargv[argc] = NULL;
    out = run_system(argc, sargv);
    free(sargv);
    /* out is NULL on timeout or allocation failure — return empty string */
    if (out) { r = vel_val_str(out); free(out); }
    else      { r = vel_val_str(""); }
    return r;
#endif
}

static void register_native(vel_t vel)
{
    vel_register(vel, "writechar", cmd_writechar);
    vel_register(vel, "system",    cmd_system);
    vel_register(vel, "canread",   cmd_canread);
    vel_register(vel, "readline",  cmd_readline);
}

/* ============================================================
 * History
 * ========================================================= */

static char *history_path(void)
{
    static char path[512];
    const char *home = getenv("HOME");
    if (!home) home = ".";
    snprintf(path, sizeof(path), "%s/.vel_history", home);
    return path;
}

#define HIST_MAX 500

typedef struct {
    char  *entries[HIST_MAX];
    int    head;
    int    count;
    int    nav;
} simple_hist_t;

static simple_hist_t g_hist;

static void hist_init(void)
{
    memset(&g_hist, 0, sizeof(g_hist));
    g_hist.nav = -1;
    FILE *f = fopen(history_path(), "r");
    if (!f) return;
    char buf[4096];
    while (fgets(buf, sizeof(buf), f)) {
        size_t len = strlen(buf);
        while (len && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = 0;
        if (!len) continue;
        int idx = (g_hist.head + g_hist.count) % HIST_MAX;
        if (g_hist.count == HIST_MAX) {
            free(g_hist.entries[g_hist.head]);
            g_hist.head = (g_hist.head + 1) % HIST_MAX;
        } else {
            g_hist.count++;
        }
        g_hist.entries[idx] = strdup(buf);
    }
    fclose(f);
}

static void hist_add(const char *line)
{
    if (!line || !line[0]) return;
    if (g_hist.count > 0) {
        int last = (g_hist.head + g_hist.count - 1) % HIST_MAX;
        if (g_hist.entries[last] && !strcmp(g_hist.entries[last], line)) return;
    }
    int idx = (g_hist.head + g_hist.count) % HIST_MAX;
    if (g_hist.count == HIST_MAX) {
        free(g_hist.entries[g_hist.head]);
        g_hist.head = (g_hist.head + 1) % HIST_MAX;
    } else {
        g_hist.count++;
    }
    g_hist.entries[idx] = strdup(line);
}

static void hist_save(void)
{
    FILE *f = fopen(history_path(), "w");
    if (!f) return;
    int start = g_hist.count > 200 ? g_hist.count - 200 : 0;
    int i;
    for (i = start; i < g_hist.count; i++) {
        int idx = (g_hist.head + i) % HIST_MAX;
        if (g_hist.entries[idx])
            fprintf(f, "%s\n", g_hist.entries[idx]);
    }
    fclose(f);
}

/* ============================================================
 * Prompt
 * ========================================================= */

static void build_prompt(char *buf, size_t bufsz, int depth)
{
    char cwd[512]; char shortcwd[512];
    const char *dir = "."; size_t dirlen = 1;

    if (getcwd(cwd, sizeof(cwd))) {
        char *p = cwd, *last = cwd, *prev = cwd;
        while (*p) { if (*p == '/') { prev = last; last = p + 1; } p++; }
        if (prev != cwd && prev != last && last[0]) {
            size_t maxlast = sizeof(shortcwd) - 5;
            size_t lastlen = strlen(last);
            if (lastlen > maxlast) lastlen = maxlast;
            memcpy(shortcwd, "\xe2\x80\xa6/", 4);
            memcpy(shortcwd + 4, last, lastlen);
            shortcwd[4 + lastlen] = '\0';
            dir = shortcwd;
        } else { dir = cwd; }
        dirlen = strlen(dir);
    }

    if (depth == 0)
        snprintf(buf, bufsz, "%s%s%s %s\xc2\xbb%s ",
                 COL_CYAN, dir, COL_RESET, COL_GREEN, COL_RESET);
    else
        snprintf(buf, bufsz, "%s%*s\xe2\x80\xa6%s ",
                 COL_YELLOW, (int)(dirlen + 2), "", COL_RESET);
}

/*
 * count_depth — hitung kedalaman {}/[] yang belum ditutup di string s.
 *
 * Digunakan oleh REPL untuk memutuskan apakah baris input belum selesai
 * dan masih perlu baris lanjutan.
 *
 * Aturan yang dihandle:
 *   - String " ... "  dan ' ... '  : karakter di dalamnya tidak dihitung.
 *   - Komentar baris  # ...        : sisanya sampai akhir baris diabaikan.
 *   - Komentar blok  ## ... ##     : seluruh blok (termasuk isinya) diabaikan.
 *
 * BUGFIX: versi lama memproses ## sebagai token lalu meneruskan ke loop
 * utama; karakter { } di dalam blok komentar ikut terhitung sehingga REPL
 * bisa "stuck" menunggu } penutup padahal baris sudah lengkap.
 * Sekarang seluruh isi blok dilewati sebelum kembali ke loop utama.
 */
static int count_depth(const char *s)
{
    int  depth  = 0;
    int  in_str = 0;
    char str_ch = 0;

    for (; *s; s++) {
        /* ---- di dalam string literal ---- */
        if (in_str) {
            if (*s == '\\' && (str_ch == '"' || str_ch == '\'')) {
                s++;
                if (!*s) break;
                continue;
            }
            if (*s == str_ch) in_str = 0;
            continue;
        }

        /* ---- mulai string literal ---- */
        if (*s == '"' || *s == '\'') {
            in_str = 1;
            str_ch = *s;
            continue;
        }

        /* ---- komentar blok  ## ... ##  ---- */
        if (*s == '#' && s[1] == '#' && s[2] != '#') {
            /* skip the opening ## then scan for the closing ## */
            s += 2;
            while (*s) {
                if (*s == '#' && s[1] == '#' && s[2] != '#') {
                    s += 1; /* for-loop will advance one more to land after ## */
                    break;
                }
                s++;
            }
            if (!*s) break;
            continue;
        }

        /* ---- komentar baris  # ... ---- */
        if (*s == '#') break;

        /* ---- brace / bracket counting ---- */
        if (*s == '{' || *s == '[') depth++;
        if (*s == '}' || *s == ']') depth--;
    }
    return depth;
}

static char *read_line_rl(const char *prompt)
{
#if HAS_READLINE
    char *line = readline(prompt);
    if (line && *line) add_history(line);
    return line;
#else
    static char buf[65536];
    fputs(prompt, stdout); fflush(stdout);
    if (!fgets(buf, sizeof(buf), stdin)) return NULL;
    size_t len = strlen(buf);
    while (len && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = 0;

    /* Strip bracketed paste escape sequences: ESC[200~ (start) dan ESC[201~ (end).
     * Terminal mengirim ini saat paste jika mode belum dinonaktifkan.
     * Format: \033[200~ di awal dan \033[201~ di akhir — hapus keduanya. */
    {
        char *p; char tmp[65536];
        /* hapus semua kemunculan \033[200~ dan \033[201~ */
        const char *seq[] = { "\033[200~", "\033[201~", NULL };
        int changed = 1;
        while (changed) {
            changed = 0;
            for (int si = 0; seq[si]; si++) {
                size_t slen = strlen(seq[si]);
                if ((p = strstr(buf, seq[si])) != NULL) {
                    size_t pre = (size_t)(p - buf);
                    size_t rest = strlen(p + slen);
                    memcpy(tmp, buf, pre);
                    memcpy(tmp + pre, p + slen, rest + 1);
                    memcpy(buf, tmp, pre + rest + 1);
                    len = pre + rest;
                    changed = 1;
                }
            }
        }
    }

    return buf[0] ? strdup(buf) : strdup("");
#endif
}

/* ============================================================
 * REPL
 * ========================================================= */

static int repl(void)
{
    vel_t vel = vel_new();

    register_native(vel);
    vel_set_callback(vel, VEL_CB_EXIT, (vel_cb_t)vel_on_exit);
    vel_jobs_init();

#ifndef WIN32
    g_color = isatty(fileno(stdout));
    /* Nonaktifkan bracketed paste mode agar karakter ESC[200~ / ESC[201~
     * tidak muncul di input saat pengguna paste teks ke REPL.
     * ESC[?2004l  = disable bracketed paste (xterm, VTE, iTerm2, dst.) */
    if (g_color) fputs("\033[?2004l", stdout);
#endif

#if HAS_READLINE
    read_history(history_path());
    rl_bind_key('\t', rl_insert);
#else
    hist_init();
#endif

    while (running) {
        char   prompt[256];
        char  *accum   = NULL;
        size_t acclen  = 0;
        int    depth   = 0;
        int    lineno  = 0;

        for (;;) {
            build_prompt(prompt, sizeof(prompt), lineno);
            char *line = read_line_rl(prompt);

            if (!line) {
                running = 0; free(accum); accum = NULL;
                goto repl_exit;
            }

            size_t linelen = strlen(line);
            accum = realloc(accum, acclen + linelen + 2);
            memcpy(accum + acclen, line, linelen);
            accum[acclen + linelen]     = '\n';
            accum[acclen + linelen + 1] = '\0';
            acclen += linelen + 1;

            free(line);
            depth += count_depth(accum + (acclen - linelen - 1));
            lineno++;
            if (depth <= 0) break;
        }

        if (!accum || !accum[0]) { free(accum); continue; }
        while (acclen > 0 && (accum[acclen-1] == '\n' || accum[acclen-1] == ' '))
            accum[--acclen] = '\0';
        if (!accum[0]) { free(accum); continue; }

#if !HAS_READLINE
        hist_add(accum);
#endif

        {
            vel_val_t   result; const char *s, *err; size_t pos;
            result = vel_parse(vel, accum, 0, 0);
            s      = vel_str(result);
            if (s && s[0])
                printf("%s%s%s\n", COL_GREEN, s, COL_RESET);
            vel_val_free(result);
            if (vel_error_get(vel, &err, &pos)) {
                char trace[2048];
                vel_stack_trace(vel, trace, sizeof(trace));
                printf("%serror%s at %d: %s\n", COL_RED, COL_RESET, (int)pos, err);
                if (trace[0])
                    printf("%s%s%s", COL_YELLOW, trace, COL_RESET);
            }
            vel_jobs_reap();
            vel_jobs_dispatch_signals(vel);  /* FIX: proses signal pending dengan aman */
        }
        free(accum);
    }

repl_exit:
#if HAS_READLINE
    write_history(history_path());
#else
    hist_save();
#endif
    vel_jobs_cleanup();
    vel_free(vel);
    return exit_code;
}

/* ============================================================
 * run_stdin — baca semua stdin dan eksekusi sebagai script
 *
 * Dipakai ketika stdin bukan TTY:
 *   echo "print hello" | vel
 *   vel < script.vel
 *
 * Ini adalah fitur kritis untuk penggunaan vel di pipeline Unix.
 * ========================================================= */
#ifndef WIN32
static int run_stdin(void)
{
    vel_t     vel      = vel_new();
    const char *err;
    size_t     pos;
    char      *accum   = NULL;
    size_t     acclen  = 0;
    char       buf[65536];
    size_t     n;
    vel_val_t  result;

    register_native(vel);
    vel_set_callback(vel, VEL_CB_EXIT, (vel_cb_t)vel_on_exit);
    vel_jobs_init();

    /* baca seluruh stdin */
    while ((n = fread(buf, 1, sizeof(buf) - 1, stdin)) > 0) {
        buf[n] = '\0';
        accum = realloc(accum, acclen + n + 1);
        memcpy(accum + acclen, buf, n);
        acclen += n;
        accum[acclen] = '\0';
    }

    if (!accum || !acclen) {
        free(accum);
        vel_jobs_cleanup();
        vel_free(vel);
        return 0;
    }

    result = vel_parse(vel, accum, 0, 1);
    free(accum);
    vel_val_free(result);

    /* BUG 3 FIX: reap zombie children spawned as background jobs from the
     * script.  Without this, background jobs never get their exit codes
     * collected and remain zombies until the parent vel process exits. */
    vel_jobs_reap();
    vel_jobs_dispatch_signals(vel);

    if (vel_error_get(vel, &err, &pos)) {
        char trace[2048];
        vel_stack_trace(vel, trace, sizeof(trace));
        fprintf(stderr, "vel: error at %d: %s\n", (int)pos, err);
        if (trace[0])
            fprintf(stderr, "%s", trace);
    }

    vel_jobs_cleanup();
    vel_free(vel);
    return exit_code;
}
#endif

/* ============================================================
 * File runner
 * ========================================================= */

static int run_file(int argc, const char *argv[])
{
    vel_t       vel      = vel_new();
    const char *filename = argv[1];
    const char *err;
    size_t      pos;
    vel_list_t  arglist  = vel_list_new();
    vel_val_t   args, result;
    int         i;

    /* --- baca file langsung ke buffer --- */
    FILE  *f = fopen(filename, "r");
    if (!f) {
        fprintf(stderr, "vel: cannot open '%s': %s\n", filename, strerror(errno));
        vel_free(vel);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    rewind(f);
    char *code = malloc((size_t)fsz + 2);
    if (!code) { fclose(f); vel_free(vel); return 1; }
    size_t nread = fread(code, 1, (size_t)fsz, f);
    fclose(f);
    code[nread] = '\0';

    register_native(vel);
    vel_set_callback(vel, VEL_CB_EXIT, (vel_cb_t)vel_on_exit);
    vel_jobs_init();

    for (i = 2; i < argc; i++)
        vel_list_push(arglist, vel_val_str(argv[i]));
    args = vel_list_pack(arglist, 1);
    vel_list_free(arglist);
    vel_var_set(vel, "argv", args, VEL_VAR_GLOBAL);
    vel_val_free(args);

    /*
     * Shebang support: lexer menangani '#' sebagai line comment, sehingga
     * baris #!/usr/bin/env vel otomatis dilewati tanpa handling khusus.
     */
    result = vel_parse(vel, code, 0, 1);
    free(code);
    vel_val_free(result);

    /* BUG 3 FIX: reap background jobs spawned from the script file */
    vel_jobs_reap();
    vel_jobs_dispatch_signals(vel);

    if (vel_error_get(vel, &err, &pos)) {
        char trace[2048];
        vel_stack_trace(vel, trace, sizeof(trace));
        fprintf(stderr, "vel: error at %d: %s\n", (int)pos, err);
        if (trace[0])
            fprintf(stderr, "%s", trace);
    }

    vel_jobs_cleanup();
    vel_free(vel);
    return exit_code;
}

/* ============================================================
 * main
 * ========================================================= */

int main(int argc, const char *argv[])
{
    if (argc < 2) {
#ifndef WIN32
        /* Jika stdin bukan terminal (pipe/redirect), jalankan sebagai script */
        if (!isatty(fileno(stdin)))
            return run_stdin();
#endif
        return repl();
    }

    /*
     * Mendukung dua cara pemanggilan:
     *   1. vel script.vel [args...]     -- cara biasa
     *   2. ./script.vel [args...]       -- via shebang #!/usr/bin/env vel
     *
     * Ketika kernel mengeksekusi ./script.vel dengan shebang, kernel memanggil:
     *   /usr/bin/env vel ./script.vel
     * Sehingga argv[1] adalah path ke script, dan kita cukup run_file().
     *
     * Tidak diperlukan binfmt_misc — cukup pastikan:
     *   a) file punya shebang  #!/usr/bin/env vel
     *   b) file sudah chmod +x
     *   c) vel ada di PATH
     */
    return run_file(argc, argv);
}
