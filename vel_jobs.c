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

/*
 * vel_jobs.c  --  job control dan shell pipeline untuk vel
 *
 * Implementasi lengkap:
 *   sh        - jalankan perintah via /bin/sh -c, capture output
 *   shpipe    - pipeline Unix: cmd1 | cmd2 | cmd3
 *   spawn     - fork+exec, kembalikan pid
 *   wait      - tunggu pid atau job id tertentu
 *   waitall   - tunggu semua background job selesai
 *   killjob   - kirim sinyal ke pid
 *   jobs      - tampilkan daftar background job
 *   fg        - bawa job ke foreground
 *   jobstatus - ambil exit code job yang sudah selesai
 *   sighandle - pasang handler sinyal (vel code string)
 *
 * Referensi arsitektur: dash jobs.c / eval.c (forkshell, evalpipe, waitforjob)
 *   - Setiap proses anak dalam pipeline mendapat process group sendiri (pgid)
 *   - Parent menyimpan semua job di g_jobs[]
 *   - SIGCHLD handler meng-update status job secara async
 *   - fg me-restore terminal ke pgid job lalu waitpid
 *   - Setelah fg selesai, terminal dikembalikan ke shell (pgid shell)
 */


#ifndef WIN32

#include "vel_priv.h"
#include "vel_jobs.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <termios.h>

extern int g_last_exit;  /* defined in vel_sys.c, shared with main.c */

/* ============================================================
 * Global job table
 * ========================================================= */

vel_job_t g_jobs[VEL_MAX_JOBS];
int       g_njobs        = 0;
int       g_job_control  = 0;

/* PID dan PGID shell induk — diset di vel_jobs_init() */
static pid_t  g_shell_pid  = 0;
static pid_t  g_shell_pgid = 0;
static int    g_ttyfd      = -1;   /* fd terminal yang dikontrol */

/* Sinyal handler vel: map sinyal -> vel code string */
#define MAX_SIG_HANDLERS 32
typedef struct {
    int    signo;
    char  *code;
    vel_t  vel;
} sig_handler_entry_t;

static sig_handler_entry_t g_sighandlers[MAX_SIG_HANDLERS];
static int                 g_nsighandlers = 0;

/*
 * FIX: async-signal-safe pending signal flags.
 *
 * Signal handler TIDAK BOLEH memanggil malloc/realloc/vel_parse karena
 * fungsi-fungsi tersebut tidak async-signal-safe (POSIX.1-2017 §2.4.3).
 * Memanggil mereka dari signal handler adalah undefined behavior dan bisa
 * menyebabkan deadlock atau heap corruption.
 *
 * Solusi: signal handler hanya set flag (sig_atomic_t), lalu
 * vel_jobs_dispatch_signals() dipanggil dari main loop (konteks aman)
 * untuk menjalankan vel code yang sesuai.
 */
#ifndef VEL_MAX_SIG
#  ifdef NSIG
#    define VEL_MAX_SIG NSIG
#  else
#    define VEL_MAX_SIG 64
#  endif
#endif
static volatile sig_atomic_t g_pending_sigs[VEL_MAX_SIG];

/* ============================================================
 * Utility
 * ========================================================= */

/* alokasikan jid baru (1-based, cari slot kosong) */
static int alloc_jid(void)
{
    int jid, i;
    for (jid = 1; jid <= VEL_MAX_JOBS; jid++) {
        int used = 0;
        for (i = 0; i < g_njobs; i++) {
            if (g_jobs[i].state != VJOB_DONE && g_jobs[i].jid == jid) {
                used = 1;
                break;
            }
        }
        if (!used) return jid;
    }
    return -1;
}

/* tambah job ke tabel; kembalikan indeks slot */
static int job_add(pid_t pid, int pgid, const char *cmd, int state)
{
    int i;
    int jid = alloc_jid();
    if (jid < 0) return -1;

    /* cari slot kosong (state DONE boleh di-reuse) */
    for (i = 0; i < g_njobs; i++) {
        if (g_jobs[i].state == VJOB_DONE) goto fill;
    }
    if (g_njobs >= VEL_MAX_JOBS) return -1;
    i = g_njobs++;

fill:
    g_jobs[i].jid       = jid;
    g_jobs[i].pid       = pid;
    g_jobs[i].pgid      = pgid;
    g_jobs[i].state     = state;
    g_jobs[i].exit_code = -1;
    free(g_jobs[i].cmd);
    g_jobs[i].cmd = cmd ? strdup(cmd) : strdup("?");
    return i;
}

/* cari job by jid */
static vel_job_t *job_by_jid(int jid)
{
    int i;
    for (i = 0; i < g_njobs; i++)
        if (g_jobs[i].jid == jid && g_jobs[i].state != VJOB_DONE)
            return &g_jobs[i];
    return NULL;
}

/* cari job by pid */
static vel_job_t *job_by_pid(pid_t pid)
{
    int i;
    for (i = 0; i < g_njobs; i++)
        if (g_jobs[i].pid == pid)
            return &g_jobs[i];
    return NULL;
}

/* ============================================================
 * SIGCHLD handler — reap finished children async
 * ========================================================= */

static void sigchld_handler(int sig)
{
    (void)sig;
}

void vel_jobs_reap(void)
{
    int    status;
    pid_t  pid;

    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
        vel_job_t *j = job_by_pid(pid);
        if (!j) continue;

        if (WIFCONTINUED(status)) {
            j->state = VJOB_RUNNING;
        } else if (WIFSTOPPED(status)) {
            j->state     = VJOB_STOPPED;
            j->exit_code = WSTOPSIG(status);
        } else if (WIFEXITED(status)) {
            j->state     = VJOB_DONE;
            j->exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            j->state     = VJOB_DONE;
            j->exit_code = 128 + WTERMSIG(status);
        }
    }
}

/* ============================================================
 * Init / cleanup
 * ========================================================= */

void vel_jobs_init(void)
{
    struct sigaction sa;
    int i;

    memset(g_jobs, 0, sizeof(g_jobs));
    g_njobs = 0;

    g_shell_pid  = getpid();
    g_shell_pgid = getpgrp();

    /* coba dapatkan kontrol terminal */
    if (isatty(STDIN_FILENO)) {
        g_ttyfd = STDIN_FILENO;

        /* masuk ke foreground process group */
        while (tcgetpgrp(g_ttyfd) != (g_shell_pgid = getpgrp())) {
            kill(-g_shell_pgid, SIGTTIN);
        }

        /* buat shell jadi process group leader */
        setpgid(0, 0);
        g_shell_pgid = getpid();
        tcsetpgrp(g_ttyfd, g_shell_pgid);

        g_job_control = 1;
    }

    /* pasang SIGCHLD handler */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;   /* removed SA_NOCLDSTOP: we want SIGCHLD for stop/continue */
    sigaction(SIGCHLD, &sa, NULL);

    /* ignore SIGTTOU/SIGTTIN/SIGTSTP agar shell tidak bisa di-stop */
    if (g_job_control) {
        signal(SIGTTOU, SIG_IGN);
        signal(SIGTTIN, SIG_IGN);
        signal(SIGTSTP, SIG_IGN);
    }

    for (i = 0; i < MAX_SIG_HANDLERS; i++) {
        g_sighandlers[i].signo = 0;
        g_sighandlers[i].code  = NULL;
        g_sighandlers[i].vel   = NULL;
    }
}

void vel_jobs_cleanup(void)
{
    int i;
    for (i = 0; i < g_njobs; i++) {
        free(g_jobs[i].cmd);
        g_jobs[i].cmd = NULL;
    }
    for (i = 0; i < MAX_SIG_HANDLERS; i++) {
        free(g_sighandlers[i].code);
        g_sighandlers[i].code = NULL;
    }
    if (g_job_control && g_ttyfd >= 0)
        tcsetpgrp(g_ttyfd, g_shell_pgid);
}

/* ============================================================
 * Helpers: set terminal pgrp dan restore
 * ========================================================= */

static void give_terminal_to(pid_t pgid)
{
    if (g_job_control && g_ttyfd >= 0) {
        signal(SIGTTOU, SIG_IGN);
        tcsetpgrp(g_ttyfd, pgid);
        signal(SIGTTOU, SIG_IGN);
    }
}

static void take_terminal_back(void)
{
    if (g_job_control && g_ttyfd >= 0) {
        signal(SIGTTOU, SIG_IGN);
        tcsetpgrp(g_ttyfd, g_shell_pgid);
        signal(SIGTTOU, SIG_IGN);
    }
}

/* ============================================================
 * Internal: build argv dari vel_list
 * ========================================================= */

static char **list_to_argv(vel_list_t words)
{
    size_t  n  = vel_list_len(words);
    char  **av = malloc(sizeof(char *) * (n + 1));
    size_t  i;
    for (i = 0; i < n; i++)
        av[i] = (char *)vel_str(vel_list_get(words, i));
    av[n] = NULL;
    return av;
}

/* ============================================================
 * cmd_sh — sh {command string}
 *   Jalankan string via /bin/sh -c, capture stdout, kembalikan output.
 *   sh {ls -la}
 *   sh {grep foo bar.txt} stdin  <- baca dari stdin
 * ========================================================= */

static VELCB vel_val_t cmd_sh(vel_t vel, size_t argc, vel_val_t *argv)
{
    const char *cmdstr;
    int         pipefd[2];
    pid_t       pid;
    int         status;
    char       *buf = NULL;
    size_t      sz  = 0;

    if (!argc) return NULL;
    cmdstr = vel_str(argv[0]);

    if (pipe(pipefd) < 0) {
        vel_error_set(vel, "sh: pipe() failed");
        return NULL;
    }

    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        vel_error_set(vel, "sh: fork() failed");
        return NULL;
    }

    if (pid == 0) {
        /* anak */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        if (g_job_control) {
            setpgid(0, 0);
            signal(SIGTTOU, SIG_DFL);
            signal(SIGTTIN, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
        }
        execl("/bin/sh", "sh", "-c", cmdstr, (char *)NULL);
        _exit(127);
    }

    /* parent */
    close(pipefd[1]);

    {
        char    rbuf[4096];
        ssize_t n;
        while ((n = read(pipefd[0], rbuf, sizeof(rbuf))) > 0) {
            buf = realloc(buf, sz + (size_t)n);
            memcpy(buf + sz, rbuf, (size_t)n);
            sz += (size_t)n;
        }
    }
    close(pipefd[0]);

    waitpid(pid, &status, 0);
    g_last_exit = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    if (!buf) { buf = malloc(1); sz = 0; }
    buf = realloc(buf, sz + 1);
    buf[sz] = '\0';

    /* strip trailing newline */
    while (sz > 0 && (buf[sz-1] == '\n' || buf[sz-1] == '\r'))
        buf[--sz] = '\0';

    {
        vel_val_t r = vel_val_str(buf);
        free(buf);
        return r;
    }
}

/* ============================================================
 * cmd_shpipe — shpipe {cmd1 args} {cmd2 args} ... {cmdN args}
 *
 * Bangun pipeline Unix:  cmd1 | cmd2 | ... | cmdN
 * Mirip dash evalpipe(): setiap proses adalah child dari shell,
 * stdin/stdout di-dup2 sesuai urutan pipe.
 * Output cmdN di-capture dan dikembalikan sebagai string.
 *
 * Contoh:
 *   set out [shpipe {ls /etc} {grep conf} {wc -l}]
 * ========================================================= */


/* ============================================================
 * cmd_shpipe — versi bersih dengan capture output stage terakhir
 *
 * Kita build pipeline secara bertahap:
 *   1. Buat pipe capture [cap_r, cap_w]
 *   2. Fork semua stage dari kiri ke kanan
 *   3. Stage terakhir stdout -> cap_w
 *   4. Parent baca dari cap_r
 * ========================================================= */

static VELCB vel_val_t cmd_shpipe2(vel_t vel, size_t argc, vel_val_t *argv)
{
    int       prevfd     = -1;
    int       cap[2]     = {-1, -1};
    pid_t    *pids;
    size_t    i;
    int       last_status = 0;
    char     *buf         = NULL;
    size_t    sz          = 0;
    vel_val_t result;

    if (!argc) return NULL;

    pids = calloc(argc, sizeof(pid_t));

    /* buat capture pipe untuk output stage terakhir */
    if (pipe(cap) < 0) {
        free(pids);
        vel_error_set(vel, "shpipe: pipe() failed");
        return NULL;
    }

    for (i = 0; i < argc; i++) {
        vel_list_t words   = vel_subst_list(vel, argv[i]);
        size_t     wc      = vel_list_len(words);
        int        is_last = (i == argc - 1);
        int        pip[2]  = {-1, -1};
        char     **av;

        if (!wc) {
            vel_list_free(words);
            continue;
        }
        av = list_to_argv(words);

        if (!is_last) {
            if (pipe(pip) < 0) {
                vel_error_set(vel, "shpipe: pipe() failed");
                free(av);
                vel_list_free(words);
                /* FIX: tutup prevfd sebelum goto agar tidak leak */
                if (prevfd >= 0) { close(prevfd); prevfd = -1; }
                goto done;
            }
        }

        pids[i] = fork();
        if (pids[i] < 0) {
            if (!is_last) { close(pip[0]); close(pip[1]); }
            vel_error_set(vel, "shpipe: fork() failed");
            free(av);
            vel_list_free(words);
            if (prevfd >= 0) { close(prevfd); prevfd = -1; }
            goto done;
        }

        if (pids[i] == 0) {
            /* ===== anak ===== */

            /* reset sinyal ke default */
            signal(SIGINT,  SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            signal(SIGPIPE, SIG_DFL);

            if (g_job_control) {
                pid_t pg = (i == 0) ? getpid() : pids[0];
                setpgid(0, pg);
                signal(SIGTTOU, SIG_DFL);
                signal(SIGTTIN, SIG_DFL);
                signal(SIGTSTP, SIG_DFL);
            }

            /* stdin dari stage sebelumnya */
            if (prevfd >= 0) {
                dup2(prevfd, STDIN_FILENO);
                close(prevfd);
            }
            /* tutup semua pipe yang tidak dipakai anak ini */
            close(cap[0]);

            if (!is_last) {
                /* stdout -> pip[1], tutup pip[0] */
                close(pip[0]);
                dup2(pip[1], STDOUT_FILENO);
                close(pip[1]);
                /* anak tengah tidak memakai cap[1] */
                close(cap[1]);
            } else {
                /* stage terakhir: stdout -> cap[1] (parent baca dari cap[0]) */
                dup2(cap[1], STDOUT_FILENO);
                close(cap[1]);
                /* pip[] tidak dibuka di stage terakhir, tidak perlu ditutup */
            }

            execvp(av[0], av);
            fprintf(stderr, "shpipe: %s: %s\n", av[0], strerror(errno));
            _exit(127);
        }

        /* ===== parent ===== */
        if (g_job_control) {
            if (i == 0) setpgid(pids[i], pids[i]);
            else        setpgid(pids[i], pids[0]);
        }

        /* FIX: tutup prevfd setelah dup2 di child; jika error, tutup juga sebelum goto */
        if (prevfd >= 0) close(prevfd);
        prevfd = is_last ? -1 : pip[0];
        if (!is_last) close(pip[1]);

        free(av);
        vel_list_free(words);
    }

    /* tutup write-end capture pipe di parent, lalu baca */
    close(cap[1]);
    cap[1] = -1;

    {
        char    rbuf[4096];
        ssize_t n;
        while ((n = read(cap[0], rbuf, sizeof(rbuf))) > 0) {
            buf = realloc(buf, sz + (size_t)n);
            memcpy(buf + sz, rbuf, (size_t)n);
            sz += (size_t)n;
        }
    }
    close(cap[0]);
    cap[0] = -1;

done:
    if (cap[0] >= 0) close(cap[0]);
    if (cap[1] >= 0) close(cap[1]);

    /* wait semua anak */
    {
        int    wstatus;
        size_t k;
        for (k = 0; k < argc; k++) {
            if (pids[k] > 0) {
                waitpid(pids[k], &wstatus, 0);
                if (k == argc - 1)
                    last_status = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;
            }
        }
    }
    g_last_exit = last_status;
    free(pids);

    if (!buf) { buf = malloc(1); sz = 0; }
    buf = realloc(buf, sz + 1);
    buf[sz] = '\0';

    /* strip trailing newlines */
    while (sz > 0 && (buf[sz-1] == '\n' || buf[sz-1] == '\r'))
        buf[--sz] = '\0';

    result = vel_val_str(buf);
    free(buf);
    return result;
}

/* ============================================================
 * cmd_spawn — spawn {cmd} [arg1 arg2 ...]
 *   Fork+exec command, jalan di background.
 *   Kembalikan jid (bukan pid langsung).
 *
 *   set jid [spawn {ls} -la /tmp]
 * ========================================================= */

static VELCB vel_val_t cmd_spawn(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_list_t  words;
    size_t      wc;
    char      **av;
    pid_t       pid;
    int         jidx;
    char        cmdbuf[256];
    extern int  g_last_exit;

    if (!argc) return NULL;

    /* gabung semua arg menjadi argv */
    words = vel_list_new();
    {
        size_t i;
        for (i = 0; i < argc; i++) {
            vel_list_t w = vel_subst_list(vel, argv[i]);
            size_t     j;
            for (j = 0; j < vel_list_len(w); j++)
                vel_list_push(words, vel_val_clone(vel_list_get(w, j)));
            vel_list_free(w);
        }
    }

    wc = vel_list_len(words);
    if (!wc) { vel_list_free(words); return NULL; }

    av = list_to_argv(words);

    /* buat command string untuk display */
    {
        size_t off = 0, i;
        for (i = 0; i < wc && off < sizeof(cmdbuf) - 2; i++) {
            if (i) cmdbuf[off++] = ' ';
            size_t l = strlen(av[i]);
            if (off + l >= sizeof(cmdbuf) - 1) l = sizeof(cmdbuf) - 1 - off;
            memcpy(cmdbuf + off, av[i], l);
            off += l;
        }
        cmdbuf[off] = '\0';
    }

    pid = fork();
    if (pid < 0) {
        free(av);
        vel_list_free(words);
        vel_error_set(vel, "spawn: fork() failed");
        return NULL;
    }

    if (pid == 0) {
        /* anak */
        if (g_job_control) {
            setpgid(0, 0);
            signal(SIGTTOU, SIG_DFL);
            signal(SIGTTIN, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
        }
        signal(SIGINT,  SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);

        execvp(av[0], av);
        fprintf(stderr, "spawn: %s: %s\n", av[0], strerror(errno));
        _exit(127);
    }

    /* parent */
    if (g_job_control)
        setpgid(pid, pid);

    jidx = job_add(pid, g_job_control ? pid : 0, cmdbuf, VJOB_RUNNING);
    free(av);
    vel_list_free(words);

    if (jidx < 0) {
        /* tabel penuh, tidak bisa track — kembalikan pid saja */
        return vel_val_int((vel_int_t)pid);
    }

    return vel_val_int((vel_int_t)g_jobs[jidx].jid);
}

/* ============================================================
 * cmd_wait — wait [jid]
 *   Tunggu job tertentu (jika diberi jid) atau job terdepan.
 *   Kembalikan exit code.
 *
 *   set code [wait $jid]
 * ========================================================= */

static VELCB vel_val_t cmd_wait_job(vel_t vel, size_t argc, vel_val_t *argv)
{
    (void)vel;
    vel_job_t *j;
    int        wstatus;
    pid_t      waited;

    vel_jobs_reap();   /* reap dulu yang sudah selesai */

    if (!argc) {
        /* tunggu semua */
        int status = 0;
        while ((waited = waitpid(-1, &wstatus, 0)) > 0) {
            vel_job_t *jj = job_by_pid(waited);
            if (jj) {
                jj->state     = VJOB_DONE;
                jj->exit_code = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;
                status = jj->exit_code;
            }
        }
        g_last_exit = status;
        return vel_val_int((vel_int_t)status);
    }

    {
        int jid = (int)vel_int(argv[0]);
        j = job_by_jid(jid);
        if (!j) {
            /* coba interpretasi sebagai pid langsung */
            pid_t pid2 = (pid_t)jid;
            waitpid(pid2, &wstatus, 0);
            g_last_exit = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;
            return vel_val_int((vel_int_t)g_last_exit);
        }
    }

    if (j->state == VJOB_DONE)
        return vel_val_int((vel_int_t)j->exit_code);

    /* berikan terminal ke job jika dia fg */
    waitpid(j->pid, &wstatus, 0);
    j->state     = VJOB_DONE;
    j->exit_code = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;
    g_last_exit  = j->exit_code;
    return vel_val_int((vel_int_t)j->exit_code);
}

/* ============================================================
 * cmd_waitall — tunggu semua background job
 * ========================================================= */

static VELCB vel_val_t cmd_waitall(vel_t vel, size_t argc, vel_val_t *argv)
{
    (void)argc; (void)argv;
    return cmd_wait_job(vel, 0, NULL);
}

/* ============================================================
 * cmd_fg — fg [jid]
 *   Bawa job ke foreground.
 *   Jika job control aktif: berikan terminal ke process group job,
 *   kirim SIGCONT, waitpid, lalu ambil kembali terminal.
 *
 *   fg $jid
 * ========================================================= */

static VELCB vel_val_t cmd_fg(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_job_t *j;
    int        jid;
    int        wstatus;

    vel_jobs_reap();

    if (!argc) {
        /* cari job terakhir yang running/stopped */
        int k;
        j = NULL;
        for (k = g_njobs - 1; k >= 0; k--) {
            if (g_jobs[k].state != VJOB_DONE) { j = &g_jobs[k]; break; }
        }
        if (!j) { vel_error_set(vel, "fg: no current job"); return NULL; }
    } else {
        jid = (int)vel_int(argv[0]);
        j   = job_by_jid(jid);
        if (!j) {
            vel_error_set(vel, "fg: no such job");
            return NULL;
        }
    }

    if (j->state == VJOB_DONE) {
        vel_error_set(vel, "fg: job already done");
        return NULL;
    }

    /* berikan terminal ke job */
    give_terminal_to((pid_t)j->pgid > 0 ? (pid_t)j->pgid : j->pid);

    /* lanjutkan jika stopped */
    if (j->state == VJOB_STOPPED) {
        j->state = VJOB_RUNNING;
        killpg(j->pgid > 0 ? j->pgid : j->pid, SIGCONT);
    }

    /* tunggu */
    waitpid(j->pid, &wstatus, WUNTRACED);

    if (WIFSTOPPED(wstatus)) {
        j->state     = VJOB_STOPPED;
        j->exit_code = WSTOPSIG(wstatus);
        fprintf(stderr, "\n[%d]+ Stopped\t\t%s\n", j->jid, j->cmd ? j->cmd : "");
    } else {
        j->state     = VJOB_DONE;
        j->exit_code = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;
    }

    g_last_exit = j->exit_code;

    /* ambil kembali terminal */
    take_terminal_back();

    return vel_val_int((vel_int_t)j->exit_code);
}

/* ============================================================
 * cmd_jobs — tampilkan daftar background job
 *   jobs          -> print ke stdout, kembalikan ""
 *   jobs list     -> kembalikan string daftar job
 * ========================================================= */

static VELCB vel_val_t cmd_jobs(vel_t vel, size_t argc, vel_val_t *argv)
{
    int       i;
    (void)vel;
    int       as_list = (argc > 0 && !strcmp(vel_str(argv[0]), "list"));
    vel_val_t r       = as_list ? val_make(NULL) : NULL;
    char      line[512];

    vel_jobs_reap();

    for (i = 0; i < g_njobs; i++) {
        vel_job_t *j = &g_jobs[i];
        const char *state_str;

        if (j->state == VJOB_DONE && !as_list) continue;

        switch (j->state) {
        case VJOB_RUNNING: state_str = "Running";  break;
        case VJOB_STOPPED: state_str = "Stopped";  break;
        case VJOB_DONE:    state_str = "Done";     break;
        default:           state_str = "Unknown";  break;
        }

        snprintf(line, sizeof(line), "[%d] %s\t\t%s",
                 j->jid, state_str, j->cmd ? j->cmd : "");

        if (as_list) {
            vel_val_cat_str(r, line);
            vel_val_cat_ch(r, '\n');
        } else {
            printf("%s\n", line);
        }
    }
    return r ? r : val_make(NULL);
}

/* ============================================================
 * cmd_killjob — killjob jid_or_pid [signal]
 *   Kirim sinyal ke job/pid.
 *   Signal default: SIGTERM (15)
 *
 *   killjob $jid
 *   killjob $jid 9
 *   killjob $jid KILL
 * ========================================================= */

static VELCB vel_val_t cmd_killjob(vel_t vel, size_t argc, vel_val_t *argv)
{
    int       signo = SIGTERM;
    int       jid;
    vel_job_t *j;
    pid_t     target;

    if (!argc) { vel_error_set(vel, "killjob: need jid"); return NULL; }

    if (argc >= 2) {
        const char *sname = vel_str(argv[1]);
        if (isdigit((unsigned char)sname[0])) {
            signo = atoi(sname);
        } else {
            /* nama sinyal tanpa "SIG" prefix, e.g. "KILL", "TERM", "INT" */
            char fullname[32];
            snprintf(fullname, sizeof(fullname), "%s%s",
                     strncmp(sname, "SIG", 3) == 0 ? "" : "SIG", sname);
            /* map nama sederhana */
            if      (!strcmp(fullname, "SIGKILL"))  signo = SIGKILL;
            else if (!strcmp(fullname, "SIGTERM"))  signo = SIGTERM;
            else if (!strcmp(fullname, "SIGINT"))   signo = SIGINT;
            else if (!strcmp(fullname, "SIGSTOP"))  signo = SIGSTOP;
            else if (!strcmp(fullname, "SIGCONT"))  signo = SIGCONT;
            else if (!strcmp(fullname, "SIGHUP"))   signo = SIGHUP;
            else if (!strcmp(fullname, "SIGQUIT"))  signo = SIGQUIT;
            else if (!strcmp(fullname, "SIGUSR1"))  signo = SIGUSR1;
            else if (!strcmp(fullname, "SIGUSR2"))  signo = SIGUSR2;
            else {
                vel_error_set(vel, "killjob: unknown signal");
                return NULL;
            }
        }
    }

    jid = (int)vel_int(argv[0]);
    j   = job_by_jid(jid);

    if (j) {
        /* kirim ke process group */
        target = j->pgid > 0 ? -(j->pgid) : j->pid;
    } else {
        /* anggap sebagai pid langsung */
        target = (pid_t)jid;
    }

    if (kill(target, signo) < 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "killjob: kill(%d, %d): %s",
                 (int)target, signo, strerror(errno));
        vel_error_set(vel, msg);
        return NULL;
    }

    g_last_exit = 0;
    return val_make(NULL);
}

/* ============================================================
 * cmd_jobstatus — jobstatus jid
 *   Kembalikan: "running" | "stopped" | "done:N"
 *   di mana N adalah exit code.
 *
 *   set s [jobstatus $jid]
 * ========================================================= */

static VELCB vel_val_t cmd_jobstatus(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_job_t *j;
    int        jid;
    char       buf[64];

    vel_jobs_reap();

    if (!argc) { vel_error_set(vel, "jobstatus: need jid"); return NULL; }

    jid = (int)vel_int(argv[0]);
    j   = job_by_jid(jid);
    if (!j) {
        /* cari di tabel termasuk yang done */
        int i;
        for (i = 0; i < g_njobs; i++) {
            if (g_jobs[i].jid == jid) { j = &g_jobs[i]; break; }
        }
        if (!j) {
            vel_error_set(vel, "jobstatus: no such job");
            return NULL;
        }
    }

    switch (j->state) {
    case VJOB_RUNNING: return vel_val_str("running");
    case VJOB_STOPPED: return vel_val_str("stopped");
    case VJOB_DONE:
        snprintf(buf, sizeof(buf), "done:%d", j->exit_code);
        return vel_val_str(buf);
    }
    return vel_val_str("unknown");
}

/* ============================================================
 * cmd_sighandle — sighandle SIGNAL {vel code}
 *   Pasang handler sinyal.
 *   Ketika sinyal datang, jalankan vel code di vel instance ini.
 *
 *   sighandle INT  {print "Interrupted!"}
 *   sighandle TERM {print "Terminating"; exit 0}
 *   sighandle INT  ""    # reset ke default
 * ========================================================= */

/*
 * FIX: generic_sig_handler sekarang hanya set flag — async-signal-safe.
 * vel_parse() (malloc, realloc, dll.) TIDAK dipanggil dari sini.
 */
static void generic_sig_handler(int signo)
{
    if (signo > 0 && signo < VEL_MAX_SIG)
        g_pending_sigs[signo] = 1;
}

/*
 * vel_jobs_dispatch_signals — proses semua pending signal dari main loop.
 * Dipanggil di setiap iterasi REPL setelah vel_jobs_reap().
 */
void vel_jobs_dispatch_signals(vel_t vel)
{
    int i;
    (void)vel;   /* tiap handler menyimpan vel instance-nya sendiri */
    for (i = 0; i < g_nsighandlers; i++) {
        int signo = g_sighandlers[i].signo;
        if (signo <= 0 || signo >= VEL_MAX_SIG) continue;
        if (!g_pending_sigs[signo]) continue;
        g_pending_sigs[signo] = 0;
        if (g_sighandlers[i].code && g_sighandlers[i].vel)
            vel_val_free(vel_parse(g_sighandlers[i].vel,
                                   g_sighandlers[i].code, 0, 1));
    }
}

static VELCB vel_val_t cmd_sighandle(vel_t vel, size_t argc, vel_val_t *argv)
{
    const char *sname;
    int         signo;
    const char *code;
    int         i;

    if (argc < 2) { vel_error_set(vel, "sighandle: need SIGNAL code"); return NULL; }

    sname = vel_str(argv[0]);
    code  = vel_str(argv[1]);

    /* parse nama sinyal */
    if (isdigit((unsigned char)sname[0])) {
        signo = atoi(sname);
    } else {
        if      (!strcasecmp(sname, "INT")  || !strcasecmp(sname, "SIGINT"))  signo = SIGINT;
        else if (!strcasecmp(sname, "TERM") || !strcasecmp(sname, "SIGTERM")) signo = SIGTERM;
        else if (!strcasecmp(sname, "HUP")  || !strcasecmp(sname, "SIGHUP"))  signo = SIGHUP;
        else if (!strcasecmp(sname, "QUIT") || !strcasecmp(sname, "SIGQUIT")) signo = SIGQUIT;
        else if (!strcasecmp(sname, "USR1") || !strcasecmp(sname, "SIGUSR1")) signo = SIGUSR1;
        else if (!strcasecmp(sname, "USR2") || !strcasecmp(sname, "SIGUSR2")) signo = SIGUSR2;
        else if (!strcasecmp(sname, "PIPE") || !strcasecmp(sname, "SIGPIPE")) signo = SIGPIPE;
        else if (!strcasecmp(sname, "WINCH")|| !strcasecmp(sname, "SIGWINCH"))signo = SIGWINCH;
        else {
            vel_error_set(vel, "sighandle: unknown signal name");
            return NULL;
        }
    }

    /* reset ke default jika code kosong */
    if (!code[0]) {
        for (i = 0; i < g_nsighandlers; i++) {
            if (g_sighandlers[i].signo == signo) {
                free(g_sighandlers[i].code);
                g_sighandlers[i].code  = NULL;
                g_sighandlers[i].signo = 0;
                g_sighandlers[i].vel   = NULL;
            }
        }
        signal(signo, SIG_DFL);
        return NULL;
    }

    /* cari slot yang ada atau buat baru */
    for (i = 0; i < g_nsighandlers; i++) {
        if (g_sighandlers[i].signo == signo) {
            free(g_sighandlers[i].code);
            g_sighandlers[i].code = strdup(code);
            g_sighandlers[i].vel  = vel;
            goto install;
        }
    }

    if (g_nsighandlers >= MAX_SIG_HANDLERS) {
        vel_error_set(vel, "sighandle: too many signal handlers");
        return NULL;
    }

    i = g_nsighandlers++;
    g_sighandlers[i].signo = signo;
    g_sighandlers[i].code  = strdup(code);
    g_sighandlers[i].vel   = vel;

install:
    signal(signo, generic_sig_handler);
    return NULL;
}

/* ============================================================
 * cmd_bglist — bglist
 *   Alias untuk `jobs list`, kembalikan string daftar job.
 * ========================================================= */

static VELCB vel_val_t cmd_bglist(vel_t vel, size_t argc, vel_val_t *argv)
{
    (void)argc; (void)argv;
    vel_val_t listarg = vel_val_str("list");
    vel_val_t r = cmd_jobs(vel, 1, &listarg);
    vel_val_free(listarg);
    return r;
}

/* ============================================================
 * cmd_bgpid — bgpid jid
 *   Kembalikan PID dari jid.
 * ========================================================= */

static VELCB vel_val_t cmd_bgpid(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_job_t *j;
    int jid;

    if (!argc) { vel_error_set(vel, "bgpid: need jid"); return NULL; }
    jid = (int)vel_int(argv[0]);
    j   = job_by_jid(jid);
    if (!j) {
        vel_error_set(vel, "bgpid: no such job");
        return NULL;
    }
    return vel_val_int((vel_int_t)j->pid);
}

/* ============================================================
 * Registration
 * ========================================================= */

void register_job_builtins(vel_t vel)
{
    vel_register(vel, "sh",        cmd_sh);
    vel_register(vel, "shpipe",    cmd_shpipe2);   /* versi bersih */
    vel_register(vel, "spawn",     cmd_spawn);
    /* "bg" adalah alias spawn: mendaftarkan proses ke job table sehingga
     * bisa ditrack via jobs/fg/wait. Sebelumnya bg ada di vel_sys.c tanpa
     * job table registration — sudah dihapus dari sana. */
    vel_register(vel, "bg",        cmd_spawn);
    vel_register(vel, "wait",      cmd_wait_job);
    vel_register(vel, "waitall",   cmd_waitall);
    vel_register(vel, "fg",        cmd_fg);
    vel_register(vel, "jobs",      cmd_jobs);
    vel_register(vel, "killjob",   cmd_killjob);
    vel_register(vel, "jobstatus", cmd_jobstatus);
    vel_register(vel, "sighandle", cmd_sighandle);
    vel_register(vel, "bglist",    cmd_bglist);
    vel_register(vel, "bgpid",     cmd_bgpid);
}

#else /* WIN32 */

#include "vel_priv.h"
#include "vel_jobs.h"

void vel_jobs_init(void)    {}
void vel_jobs_cleanup(void) {}
void vel_jobs_reap(void)    {}

void register_job_builtins(vel_t vel)
{
    /* stub kosong untuk Windows */
    (void)vel;
}

#endif /* !WIN32 */
