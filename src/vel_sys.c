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
 * vel_sys.c  --  system/shell built-in commands for vel
 *
 * BUGFIX:
 *   BUG #1  run_cmd_fds: child tidak tutup read-end pipe -> hang
 *   BUG #2  cmd_pipe: fd leak saat word list kosong
 *   BUG #3  $? tidak di-update setelah tiap eksekusi eksternal
 *
 * TAMBAHAN:
 *   exec    - jalankan program, capture output (seperti $() di bash)
 *   rmdir   - hapus direktori, -r untuk rekursif
 *   chmod   - ubah permission
 *   chown   - ubah pemilik (Unix)
 *   ln      - buat hard/symlink
 */

#include "vel_priv.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <time.h>

#ifndef WIN32
#  include <unistd.h>
#  include <dirent.h>
#  include <glob.h>
#  include <sys/wait.h>
#  include <fcntl.h>
#  include <pwd.h>
#  include <grp.h>
#endif

#ifdef WIN32
#  include <windows.h>
#  include <io.h>
#  include <direct.h>
#  define mkdir(p,m) _mkdir(p)
#  define getcwd     _getcwd
#  define chdir      _chdir
#endif

/* last exit-code: global, shared via extern di main.c */
int g_last_exit = 0;

/* Update variabel $? di root scope */
static void update_exit_var(vel_t vel, int code)
{
    vel_val_t v = vel_val_int((vel_int_t)code);
    vel_var_set(vel, "?", v, VEL_VAR_GLOBAL);
    vel_val_free(v);
}

/* ============================================================ exitcode */
static VELCB vel_val_t cmd_exitcode(vel_t vel, size_t argc, vel_val_t *argv)
{
    (void)vel; (void)argc; (void)argv;
    return vel_val_int((vel_int_t)g_last_exit);
}

/* ============================================================ envget / envset */
static VELCB vel_val_t cmd_envget(vel_t vel, size_t argc, vel_val_t *argv)
{
    const char *val;
    (void)vel;
    if (!argc) return NULL;
    val = getenv(vel_str(argv[0]));
    if (val) return vel_val_str(val);
    if (argc >= 2) return vel_val_clone(argv[1]);
    return NULL;
}

static VELCB vel_val_t cmd_envset(vel_t vel, size_t argc, vel_val_t *argv)
{
#ifdef WIN32
    (void)vel; (void)argc; (void)argv; return NULL;
#else
    if (argc < 2) return NULL;
    if (setenv(vel_str(argv[0]), vel_str(argv[1]), 1) != 0)
        vel_error_set(vel, strerror(errno));
    return NULL;
#endif
}

/* ============================================================ glob */
static VELCB vel_val_t cmd_glob(vel_t vel, size_t argc, vel_val_t *argv)
{
#ifdef WIN32
    (void)vel; (void)argc; (void)argv;
    vel_error_set(vel, "glob not supported on Windows"); return NULL;
#else
    glob_t g; vel_list_t list; vel_val_t r; size_t i;
    (void)vel;
    if (!argc) return NULL;
    memset(&g, 0, sizeof(g));
    for (i = 0; i < argc; i++) {
        int flags = (i == 0) ? GLOB_TILDE : (GLOB_TILDE | GLOB_APPEND);
        glob(vel_str(argv[i]), flags, NULL, &g);
    }
    list = vel_list_new();
    for (i = 0; i < g.gl_pathc; i++)
        vel_list_push(list, vel_val_str(g.gl_pathv[i]));
    globfree(&g);
    r = vel_list_pack(list, 1); vel_list_free(list); return r;
#endif
}

/* ============================================================
 * run_cmd_fds - helper internal untuk jalankan proses
 *
 * BUG #1 FIX: child harus tutup pipefd[0] setelah dup2 ke stdout,
 * sehingga parent dapat membaca EOF ketika child selesai. Tanpa ini,
 * child yang di-exec mewarisi read-end pipe dan parent hang selamanya.
 * ========================================================= */
#ifndef WIN32
static int run_cmd_fds(const char **argv, int in_fd, int out_fd,
                       char **out_buf, size_t *out_len, int capture)
{
    pid_t pid;
    int   pipefd[2] = {-1, -1};
    int   status = 0;

    if (capture && pipe(pipefd) != 0) return -1;

    pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        /* --- child --- */
        if (in_fd >= 0)  { dup2(in_fd, STDIN_FILENO);   close(in_fd);  }
        if (capture) {
            close(pipefd[0]);                 /* BUG #1 FIX: tutup read-end */
            dup2(pipefd[1], STDOUT_FILENO);
            close(pipefd[1]);
        } else if (out_fd >= 0) {
            dup2(out_fd, STDOUT_FILENO);
            close(out_fd);
        }
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    /* --- parent --- */
    if (in_fd  >= 0) close(in_fd);
    if (out_fd >= 0) close(out_fd);

    if (capture) {
        char    buf[4096]; size_t sz = 0; char *acc = NULL; ssize_t n;
        close(pipefd[1]);
        while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
            acc = realloc(acc, sz + (size_t)n);
            memcpy(acc + sz, buf, (size_t)n);
            sz += (size_t)n;
        }
        close(pipefd[0]);
        if (!acc) { acc = malloc(1); sz = 0; }
        acc = realloc(acc, sz + 1); acc[sz] = '\0';
        *out_buf = acc; *out_len = sz;
    }

    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
#endif

/* ============================================================
 * run_cmd_passthrough - jalankan proses dengan stdin/stdout/stderr langsung
 * ke terminal (tidak di-capture). Dipakai oleh cmd_run dan auto-catcher.
 * ========================================================= */
#ifndef WIN32
static int run_cmd_passthrough(const char **argv)
{
    pid_t pid = fork();
    int   status = 0;

    if (pid < 0) return -1;

    if (pid == 0) {
        /* child: jalankan langsung, warisi stdin/stdout/stderr */
        execvp(argv[0], (char *const *)argv);
        /* Jika exec gagal, cetak error ke stderr dan exit */
        fprintf(stderr, "vel: %s: command not found\n", argv[0]);
        _exit(127);
    }

    /* parent: tunggu hingga child selesai */
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
#endif

/* ============================================================
 * run - jalankan program eksternal dengan output langsung ke terminal
 *
 * Berbeda dengan exec yang meng-capture output, run mewarisi
 * stdin/stdout/stderr sehingga cocok untuk: make, cmake, cargo,
 * zig build, ./build.sh, dan semua program interaktif.
 *
 *   run make
 *   run make clean
 *   run cargo build --release
 *   run cmake --build .
 *   run ./build.sh
 * ========================================================= */
static VELCB vel_val_t cmd_run(vel_t vel, size_t argc, vel_val_t *argv)
{
#ifdef WIN32
    (void)vel; (void)argc; (void)argv;
    vel_error_set(vel, "run not supported on Windows"); return NULL;
#else
    vel_list_t words; size_t wc, j; const char **av;
    int status;

    if (!argc) return NULL;

    /* Jika argumen tunggal, tokenize dulu (mendukung run {make clean}) */
    if (argc == 1)
        words = vel_subst_list(vel, argv[0]);
    else {
        words = vel_list_new();
        for (j = 0; j < argc; j++)
            vel_list_push(words, vel_val_clone(argv[j]));
    }

    wc = vel_list_len(words);
    if (!wc) { vel_list_free(words); return vel_val_int(0); }

    av = malloc(sizeof(char *) * (wc + 1));
    for (j = 0; j < wc; j++) av[j] = vel_str(vel_list_get(words, j));
    av[wc] = NULL;

    status = run_cmd_passthrough(av);
    g_last_exit = status;
    update_exit_var(vel, status);

    free(av); vel_list_free(words);
    return vel_val_int((vel_int_t)status);
#endif
}

/* ============================================================
 * sh - jalankan string command via /bin/sh -c dengan live output
 *
 *   sh {make clean && make}
 *   sh {cargo build --release 2>&1}
 *   sh {./build.sh}
 * ========================================================= */
/*
 * NOTE: cmd_sh (capture output via /bin/sh -c) ada di vel_jobs.c.
 * Di sini hanya ada cmd_run (live output) dan cmd_exec (capture langsung).
 */

/* ============================================================
 * exec - jalankan program eksternal, capture output
 *   exec ls -la
 *   exec {grep foo} file
 *   set out [exec git log --oneline -5]
 * ========================================================= */
static VELCB vel_val_t cmd_exec(vel_t vel, size_t argc, vel_val_t *argv)
{
#ifdef WIN32
    (void)vel; (void)argc; (void)argv;
    vel_error_set(vel, "exec not supported on Windows"); return NULL;
#else
    vel_list_t words; size_t wc, j; const char **av;
    char *out_buf = NULL; size_t out_len = 0; int status; vel_val_t result;

    if (!argc) return NULL;

    if (argc == 1)
        words = vel_subst_list(vel, argv[0]);
    else {
        words = vel_list_new();
        for (j = 0; j < argc; j++)
            vel_list_push(words, vel_val_clone(argv[j]));
    }

    wc = vel_list_len(words);
    if (!wc) { vel_list_free(words); return val_make(NULL); }

    av = malloc(sizeof(char *) * (wc + 1));
    for (j = 0; j < wc; j++) av[j] = vel_str(vel_list_get(words, j));
    av[wc] = NULL;

    status = run_cmd_fds(av, -1, -1, &out_buf, &out_len, 1);
    g_last_exit = status;
    update_exit_var(vel, status);

    free(av); vel_list_free(words);

    if (out_buf) {
        /* strip trailing newlines (bash $() behavior) */
        while (out_len > 0 &&
               (out_buf[out_len-1] == '\n' || out_buf[out_len-1] == '\r'))
            out_buf[--out_len] = '\0';
        result = val_make_len(out_buf, out_len);
        free(out_buf);
    } else {
        result = val_make(NULL);
    }
    return result;
#endif
}

/* ============================================================
 * pipe - pipeline antar program (builtin + external)
 *
 * Mendukung pipeline antar vel builtin dan external command.
 * Output tiap segment di-capture lalu diumpan ke segment berikutnya.
 * Segment terakhir ditulis ke stdout via vel_write.
 * ========================================================= */

/* capture buffer untuk output builtin vel dalam pipeline */
typedef struct { char *buf; size_t len; } pipe_cap_t;

static VELCB void pipe_cap_write(vel_t v, const char *msg)
{
    pipe_cap_t *c = (pipe_cap_t *)vel_get_data(v);
    size_t mlen = strlen(msg);
    c->buf = realloc(c->buf, c->len + mlen + 1);
    memcpy(c->buf + c->len, msg, mlen + 1);
    c->len += mlen;
}

static VELCB vel_val_t cmd_pipe(vel_t vel, size_t argc, vel_val_t *argv)
{
#ifdef WIN32
    (void)vel; (void)argc; (void)argv;
    vel_error_set(vel, "pipe not supported on Windows"); return NULL;
#else
    if (!argc) return NULL;

    /*
     * Heuristic: if the FIRST argv has no spaces (bare command name),
     * treat all argv as one command + its arguments (exec-style).
     *   pipe echo "hello world"   -> exec ["echo","hello world"]
     *   pipe {echo hello} {cat}   -> echo hello | cat  (pipeline)
     */
    int first_is_bare = (strchr(vel_str(argv[0]), ' ') == NULL &&
                         strchr(vel_str(argv[0]), '\t') == NULL);

    if (first_is_bare && argc >= 1) {
        /* exec-style: run all args as one command */
        size_t k;
        const char **av = malloc(sizeof(char *) * (argc + 1));
        for (k = 0; k < argc; k++) av[k] = vel_str(argv[k]);
        av[argc] = NULL;
        char *out_buf = NULL; size_t out_len = 0;
        int status = run_cmd_fds(av, -1, -1, &out_buf, &out_len, 1);
        free(av);
        g_last_exit = status;
        update_exit_var(vel, status);
        if (out_buf) {
            while (out_len > 0 && (out_buf[out_len-1]=='\n'||out_buf[out_len-1]=='\r'))
                out_buf[--out_len] = '\0';
            vel_val_t r = val_make_len(out_buf, out_len);
            free(out_buf);
            return r;
        }
        return val_make(NULL);
    }

    /* Pipeline mode: each argv is a stage */
    char  *prev_buf = NULL;
    size_t prev_len = 0;
    size_t i;

    for (i = 0; i < argc; i++) {
        int is_last = (i == argc - 1);

        vel_list_t words = vel_subst_list(vel, argv[i]);
        size_t wc = vel_list_len(words);

        if (!wc) {
            vel_list_free(words);
            free(prev_buf); prev_buf = NULL; prev_len = 0;
            break;
        }

        const char *cmd0 = vel_str(vel_list_get(words, 0));
        int is_builtin = (!strchr(cmd0, '/') && mem_find_fn(vel, cmd0)) ? 1 : 0;

        char  *out_buf = NULL;
        size_t out_len = 0;
        int    status  = 0;

        if (is_builtin) {
            pipe_cap_t   cap      = { NULL, 0 };
            void        *old_data = vel_get_data(vel);
            vel_cb_t     old_cb   = vel->cb[VEL_CB_WRITE];

            if (prev_buf) {
                vel_val_t pv = vel_val_str(prev_buf);
                vel_var_set(vel, "_pipe_in", pv, VEL_VAR_GLOBAL);
                vel_val_free(pv);
            }

            vel_set_data(vel, &cap);
            vel_set_callback(vel, VEL_CB_WRITE, (vel_cb_t)pipe_cap_write);

            vel_val_t code = vel_list_pack(words, 1);
            vel_val_t r    = vel_parse_val(vel, code, 0);
            vel_val_free(code);

            if (r && vel_str(r)[0]) {
                const char *rs = vel_str(r);
                size_t rlen = strlen(rs);
                cap.buf = realloc(cap.buf, cap.len + rlen + 1);
                memcpy(cap.buf + cap.len, rs, rlen + 1);
                cap.len += rlen;
            }
            vel_val_free(r);

            vel_set_callback(vel, VEL_CB_WRITE, old_cb);
            vel_set_data(vel, old_data);

            out_buf = cap.buf ? cap.buf : malloc(1);
            if (!cap.buf) out_buf[0] = '\0';
            out_len = cap.len;
            status  = 0;
        } else {
            const char **av = malloc(sizeof(char *) * (wc + 1));
            size_t j;
            for (j = 0; j < wc; j++) av[j] = vel_str(vel_list_get(words, j));
            av[wc] = NULL;

            if (prev_buf && prev_len > 0) {
                int in_pfd[2], out_pfd[2] = {-1,-1};
                pipe(in_pfd);
                if (!is_last) pipe(out_pfd);

                pid_t pid = fork();
                if (pid == 0) {
                    close(in_pfd[1]);
                    dup2(in_pfd[0], STDIN_FILENO);
                    close(in_pfd[0]);
                    if (!is_last) {
                        close(out_pfd[0]);
                        dup2(out_pfd[1], STDOUT_FILENO);
                        close(out_pfd[1]);
                    }
                    execvp(av[0], (char *const *)av);
                    _exit(127);
                }
                close(in_pfd[0]);
                write(in_pfd[1], prev_buf, prev_len);
                close(in_pfd[1]);

                if (!is_last) {
                    close(out_pfd[1]);
                    char rbuf[4096]; ssize_t n;
                    while ((n = read(out_pfd[0], rbuf, sizeof(rbuf))) > 0) {
                        out_buf = realloc(out_buf, out_len + (size_t)n);
                        memcpy(out_buf + out_len, rbuf, (size_t)n);
                        out_len += (size_t)n;
                    }
                    close(out_pfd[0]);
                }
                int st = 0;
                if (pid > 0) waitpid(pid, &st, 0);
                status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
            } else {
                status = run_cmd_fds(av, -1, -1, &out_buf, &out_len, 1);
            }
            free(av);
        }

        g_last_exit = status;
        update_exit_var(vel, status);
        vel_list_free(words);

        free(prev_buf);
        prev_buf = out_buf;
        prev_len = out_len;
    }

    vel_val_t result = NULL;
    if (prev_buf) {
        while (prev_len > 0 &&
               (prev_buf[prev_len-1] == '\n' || prev_buf[prev_len-1] == '\r'))
            prev_buf[--prev_len] = '\0';
        if (prev_len > 0) {
            prev_buf[prev_len] = '\n';
            prev_buf = realloc(prev_buf, prev_len + 2);
            prev_buf[prev_len + 1] = '\0';
            vel_write(vel, prev_buf);
            prev_buf[prev_len] = '\0';
        }
        result = val_make_len(prev_buf, prev_len);
        free(prev_buf);
    } else {
        result = val_make(NULL);
    }
    return result;
#endif
}

/* ============================================================ redirect */

/* write-to-FILE callback used when redirecting a vel built-in */
typedef struct { FILE *fp; } redir_ctx_t;

static VELCB void redir_write_cb(vel_t v, const char *msg)
{
    redir_ctx_t *ctx = (redir_ctx_t *)vel_get_data(v);
    if (ctx && ctx->fp) fputs(msg, ctx->fp);
}

static VELCB vel_val_t cmd_redirect(vel_t vel, size_t argc, vel_val_t *argv)
{
#ifdef WIN32
    (void)vel; (void)argc; (void)argv;
    vel_error_set(vel, "redirect not supported on Windows"); return NULL;
#else
    const char *filename; int flags, fd, status;
    int do_stderr = 0;
    int do_append = 0;

    if (argc < 2) { vel_error_set(vel, "redirect: need {cmd} {file}"); return NULL; }

    filename = vel_str(argv[1]);
    flags    = O_WRONLY | O_CREAT | O_TRUNC;
    if (argc >= 3 && !strcmp(vel_str(argv[2]), "append")) {
        flags = O_WRONLY | O_CREAT | O_APPEND;
        do_append = 1;
    }
    if (argc >= 3 && !strcmp(vel_str(argv[2]), "stderr"))
        do_stderr = 1;

    /* ----------------------------------------------------------
     * If the command is a vel built-in (not stderr redirect),
     * run it inside vel with the write callback redirected to file.
     * ---------------------------------------------------------- */
    if (!do_stderr) {
        vel_list_t words = vel_subst_list(vel, argv[0]);
        int is_builtin = 0;

        if (vel_list_len(words) > 0) {
            const char *cmd0 = vel_str(vel_list_get(words, 0));
            if (!strchr(cmd0, '/') && mem_find_fn(vel, cmd0))
                is_builtin = 1;
        }

        if (is_builtin) {
            const char *fmode = do_append ? "ab" : "wb";
            FILE *fp = fopen(filename, fmode);
            if (!fp) {
                char msg[256];
                snprintf(msg, sizeof(msg), "redirect: cannot open '%s': %s",
                         filename, strerror(errno));
                vel_error_set(vel, msg);
                vel_list_free(words);
                return NULL;
            }

            redir_ctx_t  ctx      = { fp };
            void        *old_data = vel_get_data(vel);
            vel_cb_t    old_write = vel->cb[VEL_CB_WRITE];

            vel_set_data(vel, &ctx);
            vel_set_callback(vel, VEL_CB_WRITE, (vel_cb_t)redir_write_cb);

            vel_val_t code = vel_list_pack(words, 1);
            vel_val_t r    = vel_parse_val(vel, code, 0);
            vel_val_free(code);

            /* FIX: builtin yang return value (seperti ls, cat, grep, dll.)
             * tidak memanggil vel_write() -- mereka return string.
             * Kita harus tulis return value ke file secara eksplisit. */
            if (r && vel_str(r)[0])
                fputs(vel_str(r), fp);

            vel_set_callback(vel, VEL_CB_WRITE, old_write);
            vel_set_data(vel, old_data);
            fclose(fp);
            vel_list_free(words);
            return r;
        }
        vel_list_free(words);
    }

    /* ----------------------------------------------------------
     * External command path: fork + exec with fd redirect
     * ---------------------------------------------------------- */
    fd = open(filename, flags, 0666);
    if (fd < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "redirect: cannot open '%s': %s",
                 filename, strerror(errno));
        vel_error_set(vel, msg); return NULL;
    }

    {
        vel_list_t words = vel_subst_list(vel, argv[0]);
        size_t wc = vel_list_len(words);
        const char **av = malloc(sizeof(char *) * (wc + 1));
        size_t j;
        for (j = 0; j < wc; j++) av[j] = vel_str(vel_list_get(words, j));
        av[wc] = NULL;

        if (do_stderr) {
            pid_t pid = fork();
            if (pid == 0) {
                dup2(fd, STDERR_FILENO);
                close(fd);
                execvp(av[0], (char *const *)av);
                _exit(127);
            }
            close(fd);
            int st = 0;
            if (pid > 0) waitpid(pid, &st, 0);
            status = (pid > 0 && WIFEXITED(st)) ? WEXITSTATUS(st) : -1;
        } else {
            status = run_cmd_fds(av, -1, fd, NULL, NULL, 0);
        }
        g_last_exit = status;
        update_exit_var(vel, status);

        free(av); vel_list_free(words);
    }
    return vel_val_int((vel_int_t)status);
#endif
}

/* ============================================================ redirect_in
 * Menangani:  cmd < file
 * Buka file sebagai stdin untuk external command.
 * ========================================================= */
#ifndef WIN32
static VELCB vel_val_t cmd_redirect_in(vel_t vel, size_t argc, vel_val_t *argv)
{
    const char *filename;
    int fd, status;

    if (argc < 2) { vel_error_set(vel, "redirect_in: need {cmd} {file}"); return NULL; }

    filename = vel_str(argv[1]);
    fd = open(filename, O_RDONLY);
    if (fd < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "redirect_in: cannot open '%s': %s",
                 filename, strerror(errno));
        vel_error_set(vel, msg);
        return NULL;
    }

    vel_list_t words = vel_subst_list(vel, argv[0]);
    size_t wc = vel_list_len(words);
    const char **av = malloc(sizeof(char *) * (wc + 1));
    size_t j;
    for (j = 0; j < wc; j++) av[j] = vel_str(vel_list_get(words, j));
    av[wc] = NULL;

    /* run_cmd_fds dengan in_fd=fd, out_fd=-1 (ke stdout normal) */
    status = run_cmd_fds(av, fd, -1, NULL, NULL, 0);
    g_last_exit = status;
    update_exit_var(vel, status);

    free(av); vel_list_free(words);
    return vel_val_int((vel_int_t)status);
}
#endif

/* ============================================================ redirect2
 *
 * BUG 2 FIX: new command emitted by lex_tokenize when BOTH stdout and
 * stderr redirects appear in the same statement:
 *   cmd > out.txt 2> err.txt
 *   cmd >> out.txt 2> err.txt
 *
 * Syntax:  redirect2 {cmd} stdout_file mode stderr_file
 *   mode = "append" -> O_APPEND for stdout, otherwise O_TRUNC
 *
 * Only the external-command fork+exec path is implemented here; vel
 * built-ins that write to stdout are redirected via the VEL_CB_WRITE
 * hook, but they cannot simultaneously redirect stderr (which is a
 * FILE* at the C level).  For built-ins, only stdout is captured.
 * ========================================================= */
#ifndef WIN32
static VELCB vel_val_t cmd_redirect2(vel_t vel, size_t argc, vel_val_t *argv)
{
    const char *stdout_file, *stderr_file;
    int do_append, out_flags, out_fd, err_fd, status;
    vel_list_t cmd_words;
    size_t wc, j;
    const char **av;
    pid_t pid;

    if (argc < 4) {
        vel_error_set(vel, "redirect2: need {cmd} {stdout} {mode} {stderr}");
        return NULL;
    }

    stdout_file = vel_str(argv[1]);
    do_append   = !strcmp(vel_str(argv[2]), "append");
    stderr_file = vel_str(argv[3]);

    out_flags = O_WRONLY | O_CREAT | (do_append ? O_APPEND : O_TRUNC);
    out_fd = open(stdout_file, out_flags, 0666);
    if (out_fd < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "redirect2: cannot open '%s': %s",
                 stdout_file, strerror(errno));
        vel_error_set(vel, msg);
        return NULL;
    }

    err_fd = open(stderr_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (err_fd < 0) {
        close(out_fd);
        char msg[256];
        snprintf(msg, sizeof(msg), "redirect2: cannot open '%s': %s",
                 stderr_file, strerror(errno));
        vel_error_set(vel, msg);
        return NULL;
    }

    cmd_words = vel_subst_list(vel, argv[0]);
    wc = vel_list_len(cmd_words);
    av = malloc(sizeof(char *) * (wc + 1));
    if (!av) { close(out_fd); close(err_fd); vel_list_free(cmd_words); return NULL; }
    for (j = 0; j < wc; j++) av[j] = vel_str(vel_list_get(cmd_words, j));
    av[wc] = NULL;

    status = 0;
    pid = fork();
    if (pid == 0) {
        dup2(out_fd, STDOUT_FILENO);
        dup2(err_fd, STDERR_FILENO);
        close(out_fd);
        close(err_fd);
        execvp(av[0], (char *const *)av);
        _exit(127);
    }
    close(out_fd);
    close(err_fd);
    if (pid > 0) {
        waitpid(pid, &status, 0);
        g_last_exit = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        update_exit_var(vel, g_last_exit);
    }

    free(av);
    vel_list_free(cmd_words);
    return vel_val_int((vel_int_t)g_last_exit);
}
#endif


static VELCB vel_val_t cmd_exists(vel_t vel, size_t argc, vel_val_t *argv)
{
    struct stat st; const char *path, *kind;
    (void)vel;
    if (!argc) return NULL;
    path = vel_str(argv[0]); kind = (argc >= 2) ? vel_str(argv[1]) : "";
    if (stat(path, &st) != 0) return NULL;
    if (!kind[0] || !strcmp(kind, "any")) return vel_val_int(1);
    if (!strcmp(kind, "file") && S_ISREG(st.st_mode)) return vel_val_int(1);
    if (!strcmp(kind, "dir")  && S_ISDIR(st.st_mode)) return vel_val_int(1);
#ifndef WIN32
    if (!strcmp(kind, "symlink")) {
        struct stat lst;
        if (lstat(path, &lst) == 0 && S_ISLNK(lst.st_mode)) return vel_val_int(1);
    }
    if (!strcmp(kind, "exec") && access(path, X_OK) == 0)
        return vel_val_int(1);
#endif
    return NULL;
}

/* ============================================================ listdir */
static VELCB vel_val_t cmd_listdir(vel_t vel, size_t argc, vel_val_t *argv)
{
#ifdef WIN32
    (void)vel; (void)argc; (void)argv;
    vel_error_set(vel, "listdir not supported on Windows"); return NULL;
#else
    DIR *d; struct dirent *ent; vel_list_t list; vel_val_t r; const char *path;
    (void)vel;
    path = (argc > 0) ? vel_str(argv[0]) : ".";
    d = opendir(path);
    if (!d) {
        char msg[256];
        snprintf(msg, sizeof(msg), "listdir: cannot open '%s': %s",
                 path, strerror(errno));
        vel_error_set(vel, msg); return NULL;
    }
    list = vel_list_new();
    while ((ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        vel_list_push(list, vel_val_str(ent->d_name));
    }
    closedir(d);
    r = vel_list_pack(list, 1); vel_list_free(list); return r;
#endif
}

/* ============================================================ mkdir */
#ifndef WIN32
static int mkdir_p(const char *path)
{
    char *tmp = vel_strdup(path); size_t len = strlen(tmp), i;
    if (len && tmp[len-1] == '/') tmp[--len] = '\0';
    for (i = 1; i <= len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\0') {
            char saved = tmp[i]; tmp[i] = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) { free(tmp); return -1; }
            tmp[i] = saved;
        }
    }
    free(tmp); return 0;
}
#endif

static VELCB vel_val_t cmd_mkdir(vel_t vel, size_t argc, vel_val_t *argv)
{
    size_t start = 0; int mkpar = 0; size_t i;
    if (!argc) return NULL;
    if (!strcmp(vel_str(argv[0]), "-p")) { start = 1; mkpar = 1; }
    if (argc <= start) return NULL;
    for (i = start; i < argc; i++) {
        const char *path = vel_str(argv[i]);
        int rc;
#ifdef WIN32
        rc = _mkdir(path);
#else
        rc = mkpar ? mkdir_p(path) : mkdir(path, 0755);
#endif
        if (rc != 0 && errno != EEXIST) {
            char msg[256];
            snprintf(msg, sizeof(msg), "mkdir: '%s': %s", path, strerror(errno));
            vel_error_set(vel, msg); return NULL;
        }
    }
    return NULL;
}

/* ============================================================
 * rmdir - hapus direktori
 *   rmdir dir1 dir2
 *   rmdir -r dir    -> rekursif (rm -rf equivalent)
 * ========================================================= */
#ifndef WIN32
static int rmdir_recursive(const char *path)
{
    DIR *d = opendir(path); struct dirent *ent;
    if (!d) return -1;
    while ((ent = readdir(d)) != NULL) {
        struct stat st; char fp[4096];
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        snprintf(fp, sizeof(fp), "%s/%s", path, ent->d_name);
        if (lstat(fp, &st) != 0) { closedir(d); return -1; }
        if (S_ISDIR(st.st_mode)) { if (rmdir_recursive(fp) != 0) { closedir(d); return -1; } }
        else if (unlink(fp) != 0) { closedir(d); return -1; }
    }
    closedir(d);
    return rmdir(path);
}
#endif

static VELCB vel_val_t cmd_rmdir(vel_t vel, size_t argc, vel_val_t *argv)
{
#ifdef WIN32
    (void)vel; (void)argc; (void)argv;
    vel_error_set(vel, "rmdir not supported on Windows"); return NULL;
#else
    int recursive = 0; size_t start = 0, i;
    if (!argc) return NULL;
    if (!strcmp(vel_str(argv[0]), "-r") || !strcmp(vel_str(argv[0]), "-rf"))
        { recursive = 1; start = 1; }
    if (argc <= start) return NULL;
    for (i = start; i < argc; i++) {
        const char *path = vel_str(argv[i]);
        int rc = recursive ? rmdir_recursive(path) : rmdir(path);
        if (rc != 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "rmdir: '%s': %s", path, strerror(errno));
            vel_error_set(vel, msg); return NULL;
        }
    }
    return NULL;
#endif
}

/* ============================================================ remove */
static VELCB vel_val_t cmd_remove(vel_t vel, size_t argc, vel_val_t *argv)
{
    size_t i;
    if (!argc) return NULL;
    for (i = 0; i < argc; i++) {
        const char *path = vel_str(argv[i]);
        if (remove(path) != 0) {
#ifndef WIN32
            struct stat st;
            if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "remove: '%s' is a directory (use rmdir)", path);
                vel_error_set(vel, msg); return NULL;
            }
#endif
            char msg[256];
            snprintf(msg, sizeof(msg), "remove: '%s': %s", path, strerror(errno));
            vel_error_set(vel, msg); return NULL;
        }
    }
    return NULL;
}

/* ============================================================
 * chmod - ubah permission file
 *   chmod 755 script.sh
 *   chmod 644 file1 file2
 * ========================================================= */
static VELCB vel_val_t cmd_chmod(vel_t vel, size_t argc, vel_val_t *argv)
{
#ifdef WIN32
    (void)vel; (void)argc; (void)argv;
    vel_error_set(vel, "chmod not supported on Windows"); return NULL;
#else
    mode_t mode; size_t i;
    if (argc < 2) { vel_error_set(vel, "chmod: need mode file..."); return NULL; }
    mode = (mode_t)strtol(vel_str(argv[0]), NULL, 8);
    for (i = 1; i < argc; i++) {
        const char *path = vel_str(argv[i]);
        if (chmod(path, mode) != 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "chmod: '%s': %s", path, strerror(errno));
            vel_error_set(vel, msg); return NULL;
        }
    }
    return NULL;
#endif
}

/* ============================================================
 * chown - ubah pemilik file
 *   chown user file
 *   chown user:group file1 file2
 * ========================================================= */
static VELCB vel_val_t cmd_chown(vel_t vel, size_t argc, vel_val_t *argv)
{
#ifdef WIN32
    (void)vel; (void)argc; (void)argv;
    vel_error_set(vel, "chown not supported on Windows"); return NULL;
#else
    const char *spec; char *uname, *colon;
    uid_t uid = (uid_t)-1; gid_t gid = (gid_t)-1; size_t i;

    if (argc < 2) { vel_error_set(vel, "chown: need user[:group] file..."); return NULL; }

    spec  = vel_str(argv[0]);
    uname = vel_strdup(spec);
    colon = strchr(uname, ':');

    if (colon) {
        *colon = '\0';
        if (*(colon+1)) {
            struct group *gr = getgrnam(colon + 1);
            if (!gr) {
                char msg[128];
                snprintf(msg, sizeof(msg), "chown: unknown group '%s'", colon+1);
                free(uname); vel_error_set(vel, msg); return NULL;
            }
            gid = gr->gr_gid;
        }
    }

    if (uname[0]) {
        struct passwd *pw = getpwnam(uname);
        if (!pw) {
            char msg[128];
            snprintf(msg, sizeof(msg), "chown: unknown user '%s'", uname);
            free(uname); vel_error_set(vel, msg); return NULL;
        }
        uid = pw->pw_uid;
        if (gid == (gid_t)-1) gid = pw->pw_gid;
    }
    free(uname);

    for (i = 1; i < argc; i++) {
        if (lchown(vel_str(argv[i]), uid, gid) != 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "chown: '%s': %s",
                     vel_str(argv[i]), strerror(errno));
            vel_error_set(vel, msg); return NULL;
        }
    }
    return NULL;
#endif
}

/* ============================================================
 * ln - buat link
 *   ln -s target linkname   -> symbolic link
 *   ln target linkname      -> hard link
 * ========================================================= */
static VELCB vel_val_t cmd_ln(vel_t vel, size_t argc, vel_val_t *argv)
{
#ifdef WIN32
    (void)vel; (void)argc; (void)argv;
    vel_error_set(vel, "ln not supported on Windows"); return NULL;
#else
    int symbolic = 0; size_t start = 0; const char *target, *linkname;
    if (!argc) return NULL;
    if (!strcmp(vel_str(argv[0]), "-s")) { symbolic = 1; start = 1; }
    if (argc < start + 2) { vel_error_set(vel, "ln: need target linkname"); return NULL; }
    target   = vel_str(argv[start]);
    linkname = vel_str(argv[start+1]);
    if (symbolic ? symlink(target, linkname) : link(target, linkname)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "ln: '%s' -> '%s': %s",
                 target, linkname, strerror(errno));
        vel_error_set(vel, msg);
    }
    return NULL;
#endif
}

/* ============================================================ copy / move */
static VELCB vel_val_t cmd_copy(vel_t vel, size_t argc, vel_val_t *argv)
{
    const char *src, *dst;
    FILE       *fsrc, *fdst;
    char        buf[8192];
    size_t      n;
    char        msg[512];

    if (argc < 2) {
        vel_error_set(vel, "copy: need src dst");
        return NULL;
    }

    src  = vel_str(argv[0]);
    dst  = vel_str(argv[1]);

    fsrc = fopen(src, "rb");
    if (!fsrc) {
        snprintf(msg, sizeof(msg), "copy: cannot open '%s': %s", src, strerror(errno));
        vel_error_set(vel, msg);
        return NULL;
    }

    fdst = fopen(dst, "wb");
    if (!fdst) {
        snprintf(msg, sizeof(msg), "copy: cannot create '%s': %s", dst, strerror(errno));
        fclose(fsrc);
        vel_error_set(vel, msg);
        return NULL;
    }

    while ((n = fread(buf, 1, sizeof(buf), fsrc)) > 0) {
        if (fwrite(buf, 1, n, fdst) != n) {
            snprintf(msg, sizeof(msg), "copy: write error '%s': %s", dst, strerror(errno));
            fclose(fsrc);
            fclose(fdst);
            vel_error_set(vel, msg);
            return NULL;
        }
    }

    /*
     * BUG FIX: fread() returns 0 on both EOF (normal) and read error.
     * Without this check, a mid-file disk error silently produces a
     * truncated destination file with no error reported to the caller.
     */
    if (ferror(fsrc)) {
        snprintf(msg, sizeof(msg), "copy: read error '%s': %s", src, strerror(errno));
        fclose(fsrc);
        fclose(fdst);
        vel_error_set(vel, msg);
        return NULL;
    }

    fclose(fsrc);
    fclose(fdst);
    return NULL;
}

static VELCB vel_val_t cmd_move(vel_t vel, size_t argc, vel_val_t *argv)
{
    const char *src, *dst;
    char        msg[512];

    if (argc < 2) { vel_error_set(vel, "move: need src dst"); return NULL; }

    src = vel_str(argv[0]);
    dst = vel_str(argv[1]);

    if (rename(src, dst) != 0) {
        snprintf(msg, sizeof(msg), "move: '%s'->'%s': %s", src, dst, strerror(errno));
        vel_error_set(vel, msg);
    }
    return NULL;
}

/* ============================================================ cd / pwd / getpid */
static VELCB vel_val_t cmd_cd(vel_t vel, size_t argc, vel_val_t *argv)
{
    const char *path;
    char        msg[512];

    if (!argc || !vel_str(argv[0])[0]) {
        path = getenv("HOME");
        if (!path) path = ".";
    } else {
        path = vel_str(argv[0]);
    }

    if (chdir(path) != 0) {
        snprintf(msg, sizeof(msg), "cd: '%s': %s", path, strerror(errno));
        vel_error_set(vel, msg);
    }
    return NULL;
}

static VELCB vel_val_t cmd_pwd(vel_t vel, size_t argc, vel_val_t *argv)
{
    char buf[4096]; (void)argc; (void)argv;
    if (!getcwd(buf, sizeof(buf))) { vel_error_set(vel, strerror(errno)); return NULL; }
    return vel_val_str(buf);
}

static VELCB vel_val_t cmd_getpid(vel_t vel, size_t argc, vel_val_t *argv)
{
    (void)vel; (void)argc; (void)argv;
#ifdef WIN32
    return vel_val_int((vel_int_t)GetCurrentProcessId());
#else
    return vel_val_int((vel_int_t)getpid());
#endif
}

static VELCB vel_val_t cmd_getwd(vel_t vel, size_t argc, vel_val_t *argv)
{ return cmd_pwd(vel, argc, argv); }

/* ============================================================ format */
static VELCB vel_val_t cmd_format(vel_t vel, size_t argc, vel_val_t *argv)
{
    const char *fmt;
    vel_val_t   r;
    size_t      ai = 1;
    const char *p;

    if (!argc) return NULL;

    fmt = vel_str(argv[0]);
    r   = val_make(NULL);

    for (p = fmt; *p; p++) {
        if (*p != '%') {
            vel_val_cat_ch(r, *p);
            continue;
        }
        p++;

        /* ---- parse flags ---- */
        char   flags[8] = {0};
        size_t fi = 0;
        while (*p && (*p == '-' || *p == '+' || *p == ' ' || *p == '0' || *p == '#'))
            if (fi < 7) flags[fi++] = *p++;

        /* ---- parse width ---- */
        char   width[16] = {0};
        size_t wi = 0;
        while (*p && isdigit((unsigned char)*p))
            if (wi < 15) width[wi++] = *p++;

        /* ---- parse precision ---- */
        char   prec[16] = {0};
        size_t pi = 0;
        if (*p == '.') {
            p++;
            while (*p && isdigit((unsigned char)*p))
                if (pi < 15) prec[pi++] = *p++;
        }

        /* ---- build sub-format string ---- */
        char sub[64];
        snprintf(sub, sizeof(sub), "%%%s%s%s%s%c",
                 flags, width, pi ? "." : "", prec, *p ? *p : 's');

        if (!*p) {
            vel_val_cat_str(r, sub);
            break;
        }

        char      tmp[256];
        vel_val_t arg = (ai < argc) ? argv[ai++] : vel->empty;

        switch (*p) {
        case 's': {
            const char *s   = vel_str(arg);
            size_t      sn  = strlen(s) + strlen(flags) + strlen(width) + strlen(prec) + 8;
            char       *big = malloc(sn + 256);
            snprintf(big, sn + 256, sub, s);
            vel_val_cat_str(r, big);
            free(big);
            break;
        }
        case 'd':
        case 'i':
            snprintf(tmp, sizeof(tmp), sub, (int)vel_int(arg));
            vel_val_cat_str(r, tmp);
            break;
        case 'u':
            snprintf(tmp, sizeof(tmp), sub, (unsigned int)vel_int(arg));
            vel_val_cat_str(r, tmp);
            break;
        case 'f':
        case 'g':
        case 'e':
        case 'E':
        case 'G':
            snprintf(tmp, sizeof(tmp), sub, vel_dbl(arg));
            vel_val_cat_str(r, tmp);
            break;
        case 'x':
        case 'X':
            snprintf(tmp, sizeof(tmp), sub, (unsigned int)vel_int(arg));
            vel_val_cat_str(r, tmp);
            break;
        case 'o':
            snprintf(tmp, sizeof(tmp), sub, (unsigned int)vel_int(arg));
            vel_val_cat_str(r, tmp);
            break;
        case 'c':
            snprintf(tmp, sizeof(tmp), "%c", (char)vel_int(arg));
            vel_val_cat_str(r, tmp);
            break;
        case '%':
            vel_val_cat_ch(r, '%');
            ai--;
            break;
        default:
            vel_val_cat_str(r, sub);
            break;
        }
    }
    return r;
}

/* ============================================================ basename / dirname */
static VELCB vel_val_t cmd_basename(vel_t vel, size_t argc, vel_val_t *argv)
{
    const char *path,*slash,*base,*ext; (void)vel;
    if (!argc) return NULL;
    path=vel_str(argv[0]); size_t len=strlen(path);
    while(len>1&&path[len-1]=='/') len--;
    slash=NULL; { size_t i; for(i=0;i<len;i++) if(path[i]=='/') slash=path+i; }
    base=slash?slash+1:path;
    if (argc>=2) { ext=vel_str(argv[1]); size_t blen=strlen(base),elen=strlen(ext);
        if(elen&&blen>elen&&!strcmp(base+blen-elen,ext)) {
            char *tmp2=malloc(blen-elen+1); memcpy(tmp2,base,blen-elen); tmp2[blen-elen]='\0';
            vel_val_t r2=vel_val_str(tmp2); free(tmp2); return r2; } }
    return vel_val_str(base);
}

static VELCB vel_val_t cmd_dirname(vel_t vel, size_t argc, vel_val_t *argv)
{
    const char *path; char *tmp; size_t len,i,last=0; int found=0; vel_val_t r; (void)vel;
    if (!argc) return vel_val_str(".");
    path=vel_str(argv[0]); len=strlen(path);
    while(len>1&&path[len-1]=='/') len--;
    for(i=0;i<len;i++) if(path[i]=='/'){last=i;found=1;}
    if(!found) return vel_val_str(".");
    if(last==0) return vel_val_str("/");
    tmp=malloc(last+1); memcpy(tmp,path,last); tmp[last]='\0';
    r=vel_val_str(tmp); free(tmp); return r;
}

/* ============================================================ switch / date / bg
 *
 * FIX: dua bug diperbaiki:
 *   1. Body sekarang di-execute via vel_parse_val(), bukan di-clone sebagai string.
 *      Sebelumnya `switch $x {a} { write hello }` hanya mengembalikan teks
 *      "{ write hello }" tanpa menjalankannya.
 *   2. Mendukung syntax satu-block (Tcl-style):
 *        switch $val { pat1 {body1} pat2 {body2} default {bodyN} }
 *      agar pattern berupa bare word (seperti "all") tidak dievaluasi
 *      sebagai command sebelum mencapai cmd_switch.
 *
 * Syntax yang didukung:
 *   switch VAL {pat1} {body1} {pat2} {body2} ...
 *   switch VAL { pat1 {body1} pat2 {body2} }   <- satu block, Tcl-style
 *
 * Kata kunci "default" cocok dengan nilai apa pun (fallthrough).
 */
static VELCB vel_val_t cmd_switch(vel_t vel, size_t argc, vel_val_t *argv)
{
    const char *val;
    size_t i;

    if (argc < 2) return NULL;
    val = vel_str(argv[0]);

    /*
     * Syntax satu-block: switch VAL { pat1 {body1} pat2 {body2} }
     * Kenali jika argc == 2 dan argv[1] adalah block (diurai ulang sebagai
     * flat list oleh vel_subst_list).
     */
    if (argc == 2) {
        vel_list_t lst = vel_subst_list(vel, argv[1]);
        vel_val_t  result = NULL;
        size_t     n;

        if (!lst || vel->err_code) return NULL;
        n = vel_list_len(lst);

        for (i = 0; i + 1 < n; i += 2) {
            vel_val_t pat_v  = vel_list_get(lst, i);
            vel_val_t body_v = vel_list_get(lst, i + 1);
            const char *pat  = pat_v ? vel_str(pat_v) : "";

            if (!strcmp(pat, "default") || !strcmp(pat, val)) {
                result = vel_parse_val(vel, body_v, 0);
                break;
            }
        }
        vel_list_free(lst);
        return result;
    }

    /*
     * Syntax flat: switch VAL {pat1} {body1} {pat2} {body2} ...
     * Pattern sudah berupa string literal (karena dikirim dalam {} oleh
     * pemanggil); body di-execute langsung oleh vel_parse_val.
     */
    for (i = 1; i + 1 < argc; i += 2) {
        const char *pat = vel_str(argv[i]);
        if (!strcmp(pat, "default") || !strcmp(pat, val))
            return vel_parse_val(vel, argv[i + 1], 0);
    }
    return NULL;
}

static VELCB vel_val_t cmd_date(vel_t vel, size_t argc, vel_val_t *argv)
{
    time_t      now = time(NULL);
    struct tm  *tm_info;
    char        buf[256];
    const char *fmt;
    (void)vel;

    if (argc && !strcmp(vel_str(argv[0]), "epoch"))
        return vel_val_int((vel_int_t)now);

    tm_info = localtime(&now);
    fmt     = (argc > 0 && vel_str(argv[0])[0]) ? vel_str(argv[0]) : "%Y-%m-%d %H:%M:%S";
    strftime(buf, sizeof(buf), fmt, tm_info);
    return vel_val_str(buf);
}

/* ============================================================
 * writefile / readfile — Tcl-style convenience I/O
 * ========================================================= */
static VELCB vel_val_t cmd_writefile(vel_t vel, size_t argc, vel_val_t *argv)
{
    const char *path, *data; FILE *f; size_t dlen;
    if (argc < 2) { vel_error_set(vel, "writefile: path data"); return NULL; }
    path = vel_str(argv[0]); data = vel_str(argv[1]); dlen = strlen(data);
    f = fopen(path, "wb");
    if (!f) { char msg[256]; snprintf(msg,sizeof(msg),"writefile: cannot open '%s': %s",path,strerror(errno)); vel_error_set(vel,msg); return NULL; }
    if (dlen && fwrite(data,1,dlen,f)!=dlen) { char msg[256]; snprintf(msg,sizeof(msg),"writefile: write error '%s': %s",path,strerror(errno)); fclose(f); vel_error_set(vel,msg); return NULL; }
    fclose(f);
    return vel_val_clone(argv[1]);
}

static VELCB vel_val_t cmd_readfile(vel_t vel, size_t argc, vel_val_t *argv)
{
    FILE *f; long sz_raw; size_t sz; char *buf; vel_val_t r;
    if (!argc) { vel_error_set(vel,"readfile: path required"); return NULL; }
    f = fopen(vel_str(argv[0]),"rb");
    if (!f) { char msg[256]; snprintf(msg,sizeof(msg),"readfile: cannot open '%s': %s",vel_str(argv[0]),strerror(errno)); vel_error_set(vel,msg); return NULL; }
    fseek(f,0,SEEK_END); sz_raw=ftell(f); fseek(f,0,SEEK_SET);
    if (sz_raw<0){fclose(f);return NULL;} sz=(size_t)sz_raw;
    buf=malloc(sz+1); if(!buf){fclose(f);return NULL;}
    buf[fread(buf,1,sz,f)]='\0'; fclose(f);
    r=vel_val_str(buf); free(buf); return r;
}

/* ============================================================
 * file — unified file info command (Tcl-compatible)
 *
 *   file exists   path     -> 1 or ""
 *   file isfile   path     -> 1 or ""
 *   file isdir    path     -> 1 or ""
 *   file size     path     -> size in bytes
 *   file extension path    -> ".ext" or ""
 *   file tail     path     -> basename
 *   file dir      path     -> dirname
 *   file dirname  path     -> dirname (alias)
 *   file basename path     -> basename (alias)
 *   file readable path     -> 1 or ""
 *   file writable path     -> 1 or ""
 * ========================================================= */
static VELCB vel_val_t cmd_file(vel_t vel, size_t argc, vel_val_t *argv)
{
    const char *op, *path;
    struct stat st;

    if (argc < 2) { vel_error_set(vel, "file: subcommand path required"); return NULL; }
    op   = vel_str(argv[0]);
    path = vel_str(argv[1]);

    if (!strcmp(op, "exists")) {
        return (stat(path, &st) == 0) ? vel_val_int(1) : vel_val_int(0);
    }
    if (!strcmp(op, "isfile") || !strcmp(op, "isregular")) {
        if (stat(path, &st) != 0) return vel_val_int(0);
        return S_ISREG(st.st_mode) ? vel_val_int(1) : vel_val_int(0);
    }
    if (!strcmp(op, "isdir") || !strcmp(op, "isdirectory")) {
        if (stat(path, &st) != 0) return vel_val_int(0);
        return S_ISDIR(st.st_mode) ? vel_val_int(1) : vel_val_int(0);
    }
    if (!strcmp(op, "size")) {
        if (stat(path, &st) != 0) {
            char msg[256]; snprintf(msg, sizeof(msg), "file size: cannot stat '%s': %s", path, strerror(errno));
            vel_error_set(vel, msg); return NULL;
        }
        return vel_val_int((vel_int_t)st.st_size);
    }
    if (!strcmp(op, "extension") || !strcmp(op, "ext")) {
        const char *dot = strrchr(path, '.'), *slash = strrchr(path, '/');
        if (!dot || (slash && dot < slash)) return vel_val_str("");
        return vel_val_str(dot);
    }
    if (!strcmp(op, "tail") || !strcmp(op, "basename")) {
        const char *slash = strrchr(path, '/');
        return vel_val_str(slash ? slash + 1 : path);
    }
    if (!strcmp(op, "dir") || !strcmp(op, "dirname") || !strcmp(op, "directory")) {
        const char *slash = strrchr(path, '/');
        if (!slash) return vel_val_str(".");
        if (slash == path) return vel_val_str("/");
        {
            char *tmp = malloc((size_t)(slash - path) + 1);
            memcpy(tmp, path, (size_t)(slash - path));
            tmp[slash - path] = '\0';
            vel_val_t r = vel_val_str(tmp);
            free(tmp);
            return r;
        }
    }
    if (!strcmp(op, "join")) {
        /* file join path1 path2 ... */
        vel_val_t r = val_make(NULL);
        size_t i;
        for (i = 1; i < argc; i++) {
            const char *s = vel_str(argv[i]);
            if (s[0] == '/') {
                /* absolute path: restart */
                r->len = 0; r->data[0] = '\0';
                vel_val_cat_str(r, s);
            } else {
                if (r->len && r->data[r->len - 1] != '/') vel_val_cat_ch(r, '/');
                vel_val_cat_str(r, s);
            }
        }
        return r;
    }
#ifndef WIN32
    if (!strcmp(op, "readable")) {
        return (access(path, R_OK) == 0) ? vel_val_int(1) : vel_val_int(0);
    }
    if (!strcmp(op, "writable") || !strcmp(op, "writeable")) {
        return (access(path, W_OK) == 0) ? vel_val_int(1) : vel_val_int(0);
    }
    if (!strcmp(op, "executable")) {
        return (access(path, X_OK) == 0) ? vel_val_int(1) : vel_val_int(0);
    }
#endif
    if (!strcmp(op, "mtime")) {
        if (stat(path, &st) != 0) return NULL;
        return vel_val_int((vel_int_t)st.st_mtime);
    }
    {
        char msg[128]; snprintf(msg, sizeof(msg), "file: unknown subcommand '%s'", op);
        vel_error_set(vel, msg); return NULL;
    }
}


void register_sys_builtins(vel_t vel)
{
    vel_register(vel, "exitcode",  cmd_exitcode);
    vel_register(vel, "envget",    cmd_envget);
    vel_register(vel, "envset",    cmd_envset);
    vel_register(vel, "glob",      cmd_glob);
    vel_register(vel, "run",       cmd_run);
    vel_register(vel, "exec",      cmd_exec);
    vel_register(vel, "pipe",      cmd_pipe);
    vel_register(vel, "redirect",  cmd_redirect);
#ifndef WIN32
    vel_register(vel, "redirect2", cmd_redirect2);
    vel_register(vel, "redirect_in", cmd_redirect_in);
#endif
    vel_register(vel, "exists",    cmd_exists);
    vel_register(vel, "listdir",   cmd_listdir);
    vel_register(vel, "mkdir",     cmd_mkdir);
    vel_register(vel, "rmdir",     cmd_rmdir);
    vel_register(vel, "remove",    cmd_remove);
    vel_register(vel, "chmod",     cmd_chmod);
    vel_register(vel, "chown",     cmd_chown);
    vel_register(vel, "ln",        cmd_ln);
    vel_register(vel, "copy",      cmd_copy);
    vel_register(vel, "move",      cmd_move);
    vel_register(vel, "format",    cmd_format);
    vel_register(vel, "basename",  cmd_basename);
    vel_register(vel, "dirname",   cmd_dirname);
    vel_register(vel, "cd",        cmd_cd);
    vel_register(vel, "pwd",       cmd_pwd);
    vel_register(vel, "getpid",    cmd_getpid);
    vel_register(vel, "getwd",     cmd_getwd);
    vel_register(vel, "switch",    cmd_switch);
    vel_register(vel, "date",      cmd_date);
    vel_register(vel, "file",      cmd_file);
    vel_register(vel, "writefile", cmd_writefile);
    vel_register(vel, "readfile",  cmd_readfile);
    /* NOTE: "bg" sekarang didaftarkan oleh vel_jobs.c sebagai alias spawn,
     * sehingga proses background ter-track di job table dan bisa diakses
     * via jobs/fg/wait. Dulu cmd_bg di sini fork tanpa mendaftarkan ke
     * job table — inconsistency itu sudah dihapus. */
}
