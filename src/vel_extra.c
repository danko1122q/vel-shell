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
 * vel_extra.c  --  extra shell-like built-in commands for vel
 *
 * Commands:
 *   File/dir   : ls  tree  find  touch  stat
 *   Text       : cat  head  tail  wc  grep
 *   System     : uptime  whoami  hostname  uname  which  env
 *   Misc       : clear  echo  seq  yes  tee  df
 */

#include "vel_priv.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <time.h>
#ifndef WIN32
#  include <fnmatch.h>
#endif

#ifndef WIN32
#  include <unistd.h>
#  include <dirent.h>
#  include <sys/utsname.h>
#  include <pwd.h>
#  include <sys/statvfs.h>
#  include <sys/time.h>
#endif

/* forward declaration */
void register_extra_builtins(vel_t vel);

/* ============================================================
 * Internal helpers
 * ========================================================= */

/* human-readable file size: "1.2K", "3.4M", etc. */
static void fmt_size(char *buf, size_t bufsz, long long sz)
{
    if      (sz >= 1073741824LL) snprintf(buf, bufsz, "%4.1fG", sz / 1073741824.0);
    else if (sz >= 1048576LL)    snprintf(buf, bufsz, "%4.1fM", sz / 1048576.0);
    else if (sz >= 1024LL)       snprintf(buf, bufsz, "%4.1fK", sz / 1024.0);
    else                         snprintf(buf, bufsz, "%4lluB", (unsigned long long)sz);
}

/* permission string like "-rwxr-xr-x" */
#ifndef WIN32
static void fmt_perm(char *buf, mode_t mode)
{
    buf[0]  = S_ISDIR(mode)  ? 'd' : S_ISLNK(mode) ? 'l' : '-';
    buf[1]  = (mode & S_IRUSR) ? 'r' : '-';
    buf[2]  = (mode & S_IWUSR) ? 'w' : '-';
    buf[3]  = (mode & S_IXUSR) ? 'x' : '-';
    buf[4]  = (mode & S_IRGRP) ? 'r' : '-';
    buf[5]  = (mode & S_IWGRP) ? 'w' : '-';
    buf[6]  = (mode & S_IXGRP) ? 'x' : '-';
    buf[7]  = (mode & S_IROTH) ? 'r' : '-';
    buf[8]  = (mode & S_IWOTH) ? 'w' : '-';
    buf[9]  = (mode & S_IXOTH) ? 'x' : '-';
    buf[10] = '\0';
}
#endif

/* ============================================================
 * ls — list directory
 *
 *   ls               -> list current dir, short format
 *   ls -l            -> long format (perm size date name)
 *   ls -a            -> include hidden files
 *   ls -la / ls -al  -> long + hidden
 *   ls /path         -> list /path
 *   ls -l /path
 * ========================================================= */

static VELCB vel_val_t cmd_ls(vel_t vel, size_t argc, vel_val_t *argv)
{
#ifdef WIN32
    (void)vel; (void)argc; (void)argv;
    vel_error_set(vel, "ls not supported on Windows");
    return NULL;
#else
    int         opt_l    = 0;
    int         opt_a    = 0;
    const char *path     = ".";
    size_t      i;

    for (i = 0; i < argc; i++) {
        const char *s = vel_str(argv[i]);
        if (s[0] == '-') {
            const char *p;
            for (p = s + 1; *p; p++) {
                if (*p == 'l') opt_l = 1;
                else if (*p == 'a') opt_a = 1;
            }
        } else {
            path = s;
        }
    }

    DIR           *d = opendir(path);
    struct dirent *ent;
    vel_val_t      out;

    if (!d) {
        /* might be a file — just stat it */
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            char sizebuf[16], permbuf[12];
            out = val_make(NULL);
            if (opt_l) {
                fmt_perm(permbuf, st.st_mode);
                fmt_size(sizebuf, sizeof(sizebuf), (long long)st.st_size);
                char tbuf[32];
                struct tm *tm = localtime(&st.st_mtime);
                strftime(tbuf, sizeof(tbuf), "%b %e %H:%M", tm);
                char line[512];
                snprintf(line, sizeof(line), "%s %s %s %s\n",
                         permbuf, sizebuf, tbuf, path);
                vel_val_cat_str(out, line);
            } else {
                vel_val_cat_str(out, path);
                vel_val_cat_ch(out, '\n');
            }
            vel_write(vel, vel_str(out));
            vel_val_free(out);
            return NULL;
        }
        char msg[256];
        snprintf(msg, sizeof(msg), "ls: cannot access '%s': %s", path, strerror(errno));
        vel_error_set(vel, msg);
        return NULL;
    }

    /* collect entries */
    typedef struct { char name[512]; struct stat st; } entry_t;
    entry_t *entries = NULL;
    size_t   count   = 0, cap = 0;

    while ((ent = readdir(d)) != NULL) {
        if (!opt_a && ent->d_name[0] == '.') continue;

        if (count == cap) {
            cap  = cap ? cap * 2 : 32;
            entries = realloc(entries, sizeof(entry_t) * cap);
        }
        snprintf(entries[count].name, sizeof(entries[count].name),
                 "%s", ent->d_name);

        char fullpath[1024];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", path, ent->d_name);
        if (stat(fullpath, &entries[count].st) != 0)
            memset(&entries[count].st, 0, sizeof(struct stat));
        count++;
    }
    closedir(d);

    /* simple alphabetical sort (insertion sort — small N) */
    size_t j;
    for (i = 1; i < count; i++) {
        entry_t tmp = entries[i];
        for (j = i; j > 0 && strcmp(entries[j-1].name, tmp.name) > 0; j--)
            entries[j] = entries[j-1];
        entries[j] = tmp;
    }

    out = val_make(NULL);

    if (opt_l) {
        /* long format */
        for (i = 0; i < count; i++) {
            char permbuf[12], sizebuf[16], tbuf[32], line[1024];
            fmt_perm(permbuf, entries[i].st.st_mode);
            fmt_size(sizebuf, sizeof(sizebuf), (long long)entries[i].st.st_size);
            struct tm *tm = localtime(&entries[i].st.st_mtime);
            strftime(tbuf, sizeof(tbuf), "%b %e %H:%M", tm);

            /* append '/' for dirs, '*' for executables */
            char suffix[2] = {0, 0};
            if (S_ISDIR(entries[i].st.st_mode))             suffix[0] = '/';
            else if (entries[i].st.st_mode & S_IXUSR)        suffix[0] = '*';

            snprintf(line, sizeof(line), "%s %s %s %s%s\n",
                     permbuf, sizebuf, tbuf, entries[i].name, suffix);
            vel_val_cat_str(out, line);
        }
    } else {
        /* short format: names separated by newline */
        for (i = 0; i < count; i++) {
            vel_val_cat_str(out, entries[i].name);
            if (S_ISDIR(entries[i].st.st_mode))      vel_val_cat_ch(out, '/');
            else if (entries[i].st.st_mode & S_IXUSR) vel_val_cat_ch(out, '*');
            vel_val_cat_ch(out, '\n');
        }
    }

    free(entries);
    /* Tulis via vel_write agar output tampil di script mode dan redirect via
     * write callback yang di-hook oleh cmd_redirect. Return NULL seperti echo. */
    vel_write(vel, vel_str(out));
    vel_val_free(out);
    return NULL;
#endif
}

/* ============================================================
 * tree — recursive directory tree
 *
 *   tree           -> tree from current dir
 *   tree /path     -> tree from /path
 *   tree -L 2      -> max depth 2
 * ========================================================= */

#ifndef WIN32
static void do_tree(vel_t vel, vel_val_t out,
                    const char *path, const char *prefix,
                    int depth, int maxdepth,
                    size_t *ndirs, size_t *nfiles)
{
    DIR           *d = opendir(path);
    struct dirent *ent;
    typedef struct { char name[512]; int isdir; } te_t;
    te_t   *entries = NULL;
    size_t  count = 0, cap = 0, i, j;

    if (!d) return;

    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (count == cap) {
            cap = cap ? cap * 2 : 16;
            entries = realloc(entries, sizeof(te_t) * cap);
        }
        snprintf(entries[count].name, 512, "%s", ent->d_name);
        char fullpath[1024];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", path, ent->d_name);
        struct stat st;
        entries[count].isdir = (stat(fullpath, &st) == 0 && S_ISDIR(st.st_mode));
        count++;
    }
    closedir(d);

    /* sort */
    for (i = 1; i < count; i++) {
        te_t tmp = entries[i];
        for (j = i; j > 0 && strcmp(entries[j-1].name, tmp.name) > 0; j--)
            entries[j] = entries[j-1];
        entries[j] = tmp;
    }

    for (i = 0; i < count; i++) {
        int last = (i == count - 1);
        char line[2048];

        snprintf(line, sizeof(line), "%s%s%s%s\n",
                 prefix,
                 last ? "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 "   /* └── */
                      : "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 ",  /* ├── */
                 entries[i].name,
                 entries[i].isdir ? "/" : "");

        vel_val_cat_str(out, line);

        if (entries[i].isdir) {
            (*ndirs)++;
            if (maxdepth < 0 || depth < maxdepth) {
                char subpath[1024], subpfx[1024];
                snprintf(subpath, sizeof(subpath), "%s/%s", path, entries[i].name);
                snprintf(subpfx, sizeof(subpfx), "%s%s",
                         prefix,
                         last ? "    "
                              : "\xe2\x94\x82   "); /* │   */
                do_tree(vel, out, subpath, subpfx, depth + 1, maxdepth,
                        ndirs, nfiles);
            }
        } else {
            (*nfiles)++;
        }
    }
    free(entries);
}
#endif

static VELCB vel_val_t cmd_tree(vel_t vel, size_t argc, vel_val_t *argv)
{
#ifdef WIN32
    (void)vel; (void)argc; (void)argv;
    vel_error_set(vel, "tree not supported on Windows");
    return NULL;
#else
    const char *path     = ".";
    int         maxdepth = -1;
    size_t      i;

    for (i = 0; i < argc; i++) {
        const char *s = vel_str(argv[i]);
        if (!strcmp(s, "-L") && i + 1 < argc) {
            maxdepth = (int)vel_int(argv[++i]);
        } else if (s[0] != '-') {
            path = s;
        }
    }

    vel_val_t out = val_make(NULL);

    /* print root */
    char header[600];
    snprintf(header, sizeof(header), "%s\n", path);
    vel_val_cat_str(out, header);

    size_t ndirs = 0, nfiles = 0;
    do_tree(vel, out, path, "", 0, maxdepth, &ndirs, &nfiles);

    /* summary */
    char summary[128];
    snprintf(summary, sizeof(summary),
             "\n%zu director%s, %zu file%s\n",
             ndirs,  ndirs  == 1 ? "y" : "ies",
             nfiles, nfiles == 1 ? ""  : "s");
    vel_val_cat_str(out, summary);

    return out;
#endif
}

/* ============================================================
 * cat — print file(s) to output
 *   cat file1 file2 ...
 * ========================================================= */

static VELCB vel_val_t cmd_cat(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_val_t out = val_make(NULL);
    size_t    i;

    if (!argc) {
        vel_error_set(vel, "cat: no file specified");
        return NULL;
    }

    for (i = 0; i < argc; i++) {
        FILE   *f = fopen(vel_str(argv[i]), "rb");
        char    buf[4096];
        size_t  n;

        if (!f) {
            char msg[256];
            snprintf(msg, sizeof(msg), "cat: %s: %s", vel_str(argv[i]), strerror(errno));
            vel_error_set(vel, msg);
            vel_val_free(out);
            return NULL;
        }
        while ((n = fread(buf, 1, sizeof(buf) - 1, f)) > 0) {
            buf[n] = '\0';
            vel_val_cat_str(out, buf);
        }
        fclose(f);
    }
    vel_write(vel, vel_str(out));
    vel_val_free(out);
    return NULL;
}

/* ============================================================
 * head — first N lines of file(s)
 *   head file            -> first 10 lines
 *   head -n 5 file       -> first 5 lines
 * ========================================================= */

static VELCB vel_val_t cmd_head(vel_t vel, size_t argc, vel_val_t *argv)
{
    int    nlines = 10;
    size_t start  = 0;
    size_t i;

    if (argc >= 2 && !strcmp(vel_str(argv[0]), "-n")) {
        nlines = (int)vel_int(argv[1]);
        start  = 2;
    }
    vel_val_t out = val_make(NULL);

    if (start >= argc) {
        /* baca dari _pipe_in */
        vel_val_t pi = vel_var_get(vel, "_pipe_in");
        if (!pi || !vel_str(pi)[0]) {
            vel_val_free(pi); vel_val_free(out);
            vel_error_set(vel, "head: no file specified"); return NULL;
        }
        const char *p = vel_str(pi); int ln = 0;
        while (*p && ln < nlines) {
            const char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p + 1) : strlen(p);
            char buf[4096];
            if (len >= sizeof(buf)) len = sizeof(buf) - 1;
            memcpy(buf, p, len); buf[len] = '\0';
            vel_val_cat_str(out, buf);
            ln++;
            p = nl ? nl + 1 : p + len;
            if (!nl) break;
        }
        vel_val_free(pi);
    } else {
    for (i = start; i < argc; i++) {
        FILE *f = fopen(vel_str(argv[i]), "r");
        char  buf[4096];
        int   ln = 0;

        if (!f) {
            char msg[256];
            snprintf(msg, sizeof(msg), "head: %s: %s", vel_str(argv[i]), strerror(errno));
            vel_error_set(vel, msg);
            vel_val_free(out);
            return NULL;
        }
        while (ln < nlines && fgets(buf, sizeof(buf), f)) {
            vel_val_cat_str(out, buf);
            size_t blen = strlen(buf);
            if (blen > 0 && (buf[blen-1] == '\n' || feof(f))) ln++;
        }
        fclose(f);
    }
    } /* end else */
    vel_write(vel, vel_str(out));
    vel_val_free(out);
    return NULL;
}

/* ============================================================
 * tail — last N lines of file
 *   tail file            -> last 10 lines
 *   tail -n 5 file       -> last 5 lines
 * ========================================================= */

static VELCB vel_val_t cmd_tail(vel_t vel, size_t argc, vel_val_t *argv)
{
    int    nlines = 10;
    size_t start  = 0;

    if (argc >= 2 && !strcmp(vel_str(argv[0]), "-n")) {
        nlines = (int)vel_int(argv[1]);
        start  = 2;
    }
    if (nlines <= 0) return val_make(NULL);

    char  **ring  = calloc((size_t)nlines, sizeof(char *));
    size_t  rhead = 0, rcount = 0;
    char    buf[4096];

    if (start >= argc) {
        /* baca dari _pipe_in */
        vel_val_t pi = vel_var_get(vel, "_pipe_in");
        if (pi && vel_str(pi)[0]) {
            const char *p = vel_str(pi);
            while (*p) {
                const char *nl = strchr(p, '\n');
                size_t len = nl ? (size_t)(nl - p + 1) : strlen(p);
                if (len >= sizeof(buf)) len = sizeof(buf) - 1;
                memcpy(buf, p, len); buf[len] = '\0';
                free(ring[rhead]);
                ring[rhead] = vel_strdup(buf);
                rhead = (rhead + 1) % (size_t)nlines;
                if (rcount < (size_t)nlines) rcount++;
                p = nl ? nl + 1 : p + len;
                if (!nl) break;
            }
        }
        vel_val_free(pi);
    } else {
        FILE *f = fopen(vel_str(argv[start]), "r");
        if (!f) {
            char msg[256];
            snprintf(msg, sizeof(msg), "tail: %s: %s", vel_str(argv[start]), strerror(errno));
            vel_error_set(vel, msg);
            for (size_t jj = 0; jj < (size_t)nlines; jj++) free(ring[jj]);
            free(ring); return NULL;
        }
        while (fgets(buf, sizeof(buf), f)) {
            free(ring[rhead]);
            ring[rhead] = vel_strdup(buf);
            rhead = (rhead + 1) % (size_t)nlines;
            if (rcount < (size_t)nlines) rcount++;
        }
        fclose(f);
    }

    vel_val_t out = val_make(NULL);
    size_t start_idx = (rhead + (size_t)nlines - rcount) % (size_t)nlines;
    size_t j;
    for (j = 0; j < rcount; j++) {
        size_t idx = (start_idx + j) % (size_t)nlines;
        if (ring[idx]) vel_val_cat_str(out, ring[idx]);
    }
    for (j = 0; j < (size_t)nlines; j++) free(ring[j]);
    free(ring);

    vel_write(vel, vel_str(out));
    vel_val_free(out);
    return NULL;
}

/* ============================================================
 * wc — word/line/char count
 *   wc file              -> lines words chars filename
 *   wc -l file           -> lines only
 *   wc -w file           -> words only
 *   wc -c file           -> chars only
 * ========================================================= */

static VELCB vel_val_t cmd_wc(vel_t vel, size_t argc, vel_val_t *argv)
{
    int    opt_l = 0, opt_w = 0, opt_c = 0;
    size_t start = 0, i;

    for (i = 0; i < argc; i++) {
        const char *s = vel_str(argv[i]);
        if (s[0] == '-') {
            const char *p;
            for (p = s + 1; *p; p++) {
                if (*p == 'l') opt_l = 1;
                else if (*p == 'w') opt_w = 1;
                else if (*p == 'c') opt_c = 1;
            }
            start = i + 1;
        } else break;
    }

    /* default: all three */
    if (!opt_l && !opt_w && !opt_c) opt_l = opt_w = opt_c = 1;

    if (start >= argc) {
        vel_error_set(vel, "wc: no file specified");
        return NULL;
    }

    vel_val_t out = val_make(NULL);

    for (i = start; i < argc; i++) {
        FILE  *f = fopen(vel_str(argv[i]), "r");
        long long lines = 0, words = 0, chars = 0;
        int       in_word = 0;
        int       ch;

        if (!f) {
            char msg[256];
            snprintf(msg, sizeof(msg), "wc: %s: %s", vel_str(argv[i]), strerror(errno));
            vel_error_set(vel, msg);
            vel_val_free(out);
            return NULL;
        }
        while ((ch = fgetc(f)) != EOF) {
            chars++;
            if (ch == '\n') lines++;
            if (!isspace(ch)) {
                if (!in_word) { words++; in_word = 1; }
            } else {
                in_word = 0;
            }
        }
        fclose(f);

        char line[512];
        char result[64] = {0};
        if (opt_l) { char t[32]; snprintf(t, sizeof(t), "%8lld", lines); strncat(result, t, sizeof(result) - strlen(result) - 1); }
        if (opt_w) { char t[32]; snprintf(t, sizeof(t), "%8lld", words); strncat(result, t, sizeof(result) - strlen(result) - 1); }
        if (opt_c) { char t[32]; snprintf(t, sizeof(t), "%8lld", chars); strncat(result, t, sizeof(result) - strlen(result) - 1); }
        snprintf(line, sizeof(line), "%s %s\n", result, vel_str(argv[i]));
        vel_val_cat_str(out, line);
    }
    vel_write(vel, vel_str(out));
    vel_val_free(out);
    return NULL;
}

/* ============================================================
 * grep — search lines matching pattern (literal, no regex yet)
 *   grep pattern file
 *   grep -i pattern file   -> case insensitive
 *   grep -v pattern file   -> invert match
 *   grep -n pattern file   -> show line numbers
 *   grep -c pattern file   -> count matching lines
 * ========================================================= */

/*
 * grep_match_line — cocokkan satu baris terhadap pola.
 *
 * Mendukung:
 *   ^pola   — anchor awal baris
 *   pola$   — anchor akhir baris
 *   ^pola$  — exact match
 *   pola    — substring biasa (strstr)
 *
 * Parameter `line` boleh mengandung '\n' di akhir; fungsi ini
 * mengabaikannya saat menghitung panjang untuk anchor '$'.
 */
static int grep_match_line(const char *line, const char *pat,
                           int anchor_start, int anchor_end,
                           const char *inner, size_t inner_len)
{
    /* panjang baris tanpa trailing newline */
    size_t llen = strlen(line);
    while (llen > 0 && (line[llen-1] == '\n' || line[llen-1] == '\r'))
        llen--;

    (void)pat; /* tidak dipakai langsung, sudah dipecah ke inner */

    if (anchor_start && anchor_end) {
        /* exact: panjang harus sama dan isi cocok */
        return (llen == inner_len &&
                strncmp(line, inner, inner_len) == 0);
    }
    if (anchor_start) {
        /* harus dimulai dari karakter pertama */
        return (llen >= inner_len &&
                strncmp(line, inner, inner_len) == 0);
    }
    if (anchor_end) {
        /* harus berakhir tepat sebelum newline */
        if (llen < inner_len) return 0;
        return (strncmp(line + llen - inner_len, inner, inner_len) == 0);
    }
    /* substring biasa */
    if (inner_len == 0) return 1;   /* pola kosong cocok semua */
    return (strstr(line, inner) != NULL);
}

static VELCB vel_val_t cmd_grep(vel_t vel, size_t argc, vel_val_t *argv)
{
    /* ----------------------------------------------------------------
     * BUG #1 FIX: two-pass argument parsing.
     *
     * Parsing lama:  berhenti di argumen pertama non-flag → flag di
     * belakang pattern (grep "pat" -i file) tidak terdeteksi dan
     * dianggap nama file → fopen("-i") gagal.
     *
     * Perbaikan: pass-1 kumpulkan semua flag, pass-2 kumpulkan
     * pattern (non-flag pertama) dan nama file (non-flag berikutnya).
     * Urutan flag/pattern/file kini bebas.
     * ---------------------------------------------------------------- */
    int    opt_i = 0, opt_v = 0, opt_n = 0, opt_c = 0;
    size_t i;
    const char *pattern = NULL;

    /* Pass 1: kumpulkan flag */
    for (i = 0; i < argc; i++) {
        const char *s = vel_str(argv[i]);
        if (s[0] == '-' && s[1] != '\0') {
            const char *p;
            for (p = s + 1; *p; p++) {
                if      (*p == 'i') opt_i = 1;
                else if (*p == 'v') opt_v = 1;
                else if (*p == 'n') opt_n = 1;
                else if (*p == 'c') opt_c = 1;
            }
        }
    }

    /* Pass 2: pattern = non-flag pertama; file = non-flag berikutnya */
    size_t file_idx[256];
    size_t nfiles = 0;
    for (i = 0; i < argc && nfiles < 255; i++) {
        const char *s = vel_str(argv[i]);
        if (s[0] == '-' && s[1] != '\0') continue;
        if (!pattern) { pattern = s; continue; }
        file_idx[nfiles++] = i;
    }

    if (!pattern) { vel_error_set(vel, "grep: pattern required"); return NULL; }

    /* ----------------------------------------------------------------
     * BUG #3 FIX: anchor support.
     *
     * Kode lama hanya memakai strstr — tidak mengenal '^' atau '$'.
     * Perbaikan: deteksi anchor lalu pakai grep_match_line() yang
     * menangani ^, $, ^…$, dan substring biasa.
     * ---------------------------------------------------------------- */
    char  *pat_work = vel_strdup(pattern);
    size_t pat_len  = strlen(pat_work);

    /* turunkan ke huruf kecil jika -i */
    if (opt_i)
        for (i = 0; pat_work[i]; i++)
            pat_work[i] = (char)tolower((unsigned char)pat_work[i]);

    int    anchor_start = (pat_work[0] == '^');
    int    anchor_end   = (pat_len > 0 && pat_work[pat_len - 1] == '$');
    /* strip anchor karakter dari pola */
    size_t inner_start  = anchor_start ? 1u : 0u;
    size_t inner_end    = pat_len - (anchor_end ? 1u : 0u);
    size_t inner_len    = (inner_end > inner_start) ? (inner_end - inner_start) : 0u;
    const char *inner   = pat_work + inner_start;

    /* Macro: cocokkan satu baris (sudah di-lowercase jika opt_i) */
#define DO_MATCH(line) \
    grep_match_line((line), pat_work, anchor_start, anchor_end, inner, inner_len)

    /* Macro: proses satu baris mentah dari file/pipe */
#define PROC_LINE(raw, lineno, mcnt) do {                               \
    char  _lo[4096]; const char *_hay = (raw);                          \
    if (opt_i) {                                                         \
        size_t _k;                                                       \
        for (_k=0; (raw)[_k] && _k < sizeof(_lo)-1; _k++)               \
            _lo[_k] = (char)tolower((unsigned char)(raw)[_k]);           \
        _lo[_k] = '\0'; _hay = _lo;                                      \
    }                                                                    \
    int _found = DO_MATCH(_hay);                                         \
    if (opt_v) _found = !_found;                                         \
    if (_found) {                                                        \
        (mcnt)++;                                                        \
        if (!opt_c) {                                                    \
            if (opt_n) {                                                 \
                char _pfx[32];                                           \
                snprintf(_pfx, sizeof(_pfx), "%lld:", (long long)(lineno)); \
                vel_val_cat_str(out, _pfx);                              \
            }                                                            \
            vel_val_cat_str(out, (raw));                                 \
        }                                                                \
    }                                                                    \
} while(0)

    vel_val_t out = val_make(NULL);

    if (nfiles == 0) {
        /* baca dari _pipe_in */
        vel_val_t  pipe_in   = vel_var_get(vel, "_pipe_in");
        const char *src      = (pipe_in && vel_str(pipe_in)[0])
                               ? vel_str(pipe_in) : "";
        const char *p        = src;
        long long   lineno   = 0, mcnt = 0;
        char        buf[4096];

        while (*p) {
            const char *nl  = strchr(p, '\n');
            size_t      len = nl ? (size_t)(nl - p) : strlen(p);
            if (len >= sizeof(buf) - 1) len = sizeof(buf) - 2;
            memcpy(buf, p, len);
            buf[len] = '\n'; buf[len+1] = '\0';
            lineno++;
            PROC_LINE(buf, lineno, mcnt);
            p = nl ? nl + 1 : p + len;
            if (!nl) break;
        }
        if (opt_c) {
            char cnt[32]; snprintf(cnt, sizeof(cnt), "%lld\n", mcnt);
            vel_val_cat_str(out, cnt);
        }
        vel_val_free(pipe_in);
    } else {
        for (i = 0; i < nfiles; i++) {
            const char *fname = vel_str(argv[file_idx[i]]);
            FILE       *f     = fopen(fname, "r");
            char        buf[4096];
            long long   lineno = 0, mcnt = 0;

            if (!f) {
                char msg[256];
                snprintf(msg, sizeof(msg), "grep: %s: %s", fname, strerror(errno));
                vel_error_set(vel, msg);
                vel_val_free(out); free(pat_work); return NULL;
            }
            while (fgets(buf, sizeof(buf), f)) {
                lineno++;
                PROC_LINE(buf, lineno, mcnt);
            }
            fclose(f);
            if (opt_c) {
                char cnt[32]; snprintf(cnt, sizeof(cnt), "%lld\n", mcnt);
                vel_val_cat_str(out, cnt);
            }
        }
    }

#undef DO_MATCH
#undef PROC_LINE

    free(pat_work);

    /* Output: return out tanpa vel_write supaya:
     *   1. REPL interaktif  → REPL auto-print return value non-NULL (main.c)
     *   2. Capture [grep …] → nilai tersedia, $r tidak kosong lagi
     * Strip satu trailing newline agar REPL tidak mencetak baris kosong
     * ekstra (REPL sudah menambah \n sendiri lewat printf). */
    {
        const char *s = vel_str(out);
        size_t slen = strlen(s);
        if (slen > 0 && s[slen - 1] == '\n') {
            vel_val_t trimmed = vel_val_str("");
            vel_val_cat_str_len(trimmed, s, slen - 1);
            vel_val_free(out);
            return trimmed;
        }
    }
    return out;
}

/* ============================================================
 * find — recursive file search
 *   find /path -name "*.c"
 *   find /path -type f
 *   find /path -type d
 * ========================================================= */

#ifndef WIN32
static void do_find(vel_t vel, vel_val_t out,
                    const char *path,
                    const char *name_pat,
                    int type_filter)   /* 0=all, 1=file, 2=dir */
{
    DIR           *d = opendir(path);
    struct dirent *ent;
    if (!d) return;

    while ((ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;

        char fullpath[2048];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", path, ent->d_name);

        struct stat st;
        if (lstat(fullpath, &st) != 0) continue;

        int is_dir  = S_ISDIR(st.st_mode);
        int is_file = S_ISREG(st.st_mode);

        /* check type filter */
        int type_ok = (type_filter == 0)
                   || (type_filter == 1 && is_file)
                   || (type_filter == 2 && is_dir);

        /*
         * FIX: gunakan fnmatch(3) (POSIX) sebagai pengganti manual wildcard.
         * Mendukung *.c, *.c.bak, [abc]*, ?, dsb. — tidak terbatas hanya
         * leading/trailing star seperti implementasi sebelumnya.
         */
        int name_ok = 1;
        if (name_pat && name_pat[0])
            name_ok = (fnmatch(name_pat, ent->d_name, FNM_PERIOD) == 0);

        if (type_ok && name_ok) {
            vel_val_cat_str(out, fullpath);
            vel_val_cat_ch(out, '\n');
        }

        if (is_dir)
            do_find(vel, out, fullpath, name_pat, type_filter);
    }
    closedir(d);
}
#endif

static VELCB vel_val_t cmd_find(vel_t vel, size_t argc, vel_val_t *argv)
{
#ifdef WIN32
    (void)vel; (void)argc; (void)argv;
    vel_error_set(vel, "find not supported on Windows");
    return NULL;
#else
    const char *path      = ".";
    const char *name_pat  = NULL;
    int         type_flt  = 0;
    size_t      i;

    for (i = 0; i < argc; i++) {
        const char *s = vel_str(argv[i]);
        if (!strcmp(s, "-name") && i + 1 < argc)  { name_pat = vel_str(argv[++i]); }
        else if (!strcmp(s, "-type") && i + 1 < argc) {
            const char *t = vel_str(argv[++i]);
            if (!strcmp(t, "f")) type_flt = 1;
            else if (!strcmp(t, "d")) type_flt = 2;
        } else if (s[0] != '-') {
            path = s;
        }
    }

    vel_val_t out = val_make(NULL);
    do_find(vel, out, path, name_pat, type_flt);
    vel_write(vel, vel_str(out));
    vel_val_free(out);
    return NULL;
#endif
}

/* ============================================================
 * touch — create file or update mtime
 *   touch file1 file2 ...
 * ========================================================= */

static VELCB vel_val_t cmd_touch(vel_t vel, size_t argc, vel_val_t *argv)
{
    size_t i;
    if (!argc) { vel_error_set(vel, "touch: no file specified"); return NULL; }

    for (i = 0; i < argc; i++) {
        const char *path = vel_str(argv[i]);
        FILE *f = fopen(path, "ab");
        if (!f) {
            char msg[256];
            snprintf(msg, sizeof(msg), "touch: %s: %s", path, strerror(errno));
            vel_error_set(vel, msg);
            return NULL;
        }
        fclose(f);
#ifndef WIN32
        /* update timestamps */
        utimes(path, NULL);
#endif
    }
    return NULL;
}

/* ============================================================
 * stat — file status info
 *   stat file
 * ========================================================= */

static VELCB vel_val_t cmd_stat(vel_t vel, size_t argc, vel_val_t *argv)
{
#ifdef WIN32
    (void)vel; (void)argc; (void)argv;
    vel_error_set(vel, "stat not fully supported on Windows");
    return NULL;
#else
    if (!argc) { vel_error_set(vel, "stat: no file specified"); return NULL; }

    struct stat st;
    const char *path = vel_str(argv[0]);

    if (lstat(path, &st) != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "stat: %s: %s", path, strerror(errno));
        vel_error_set(vel, msg);
        return NULL;
    }

    char permbuf[12], timebuf[64];
    fmt_perm(permbuf, st.st_mode);
    struct tm *tm = localtime(&st.st_mtime);
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", tm);

    const char *type = S_ISDIR(st.st_mode)  ? "directory"
                     : S_ISLNK(st.st_mode)  ? "symbolic link"
                     : S_ISREG(st.st_mode)  ? "regular file"
                     : "other";

    char sizebuf[16];
    fmt_size(sizebuf, sizeof(sizebuf), (long long)st.st_size);

    char out[1024];
    snprintf(out, sizeof(out),
        "  File: %s\n"
        "  Type: %s\n"
        "  Size: %lld (%s)\n"
        " Inode: %llu\n"
        " Perms: %s\n"
        "  Modified: %s\n",
        path, type,
        (long long)st.st_size, sizebuf,
        (unsigned long long)st.st_ino,
        permbuf, timebuf);

    vel_write(vel, out);   /* write to output, do NOT also return — avoids double output */
    return NULL;
#endif
}

/* ============================================================
 * uptime — system uptime
 * ========================================================= */

static VELCB vel_val_t cmd_uptime(vel_t vel, size_t argc, vel_val_t *argv)
{
    (void)vel; (void)argc; (void)argv;
#ifdef WIN32
    vel_error_set(vel, "uptime not supported on Windows");
    return NULL;
#else
    double up = 0.0;

    FILE *f = fopen("/proc/uptime", "r");
    if (f) {
        if (fscanf(f, "%lf", &up) != 1) up = 0.0;
        fclose(f);
    } else {
        /* fallback: time since boot via clock */
        struct timespec ts;
        if (clock_gettime(CLOCK_BOOTTIME, &ts) == 0)
            up = (double)ts.tv_sec + ts.tv_nsec / 1e9;
    }

    long long total = (long long)up;
    long long days  = total / 86400;
    long long hours = (total % 86400) / 3600;
    long long mins  = (total % 3600) / 60;
    long long secs  = total % 60;

    char buf[128];
    if (days > 0)
        snprintf(buf, sizeof(buf), "up %lld day%s, %lld:%02lld:%02lld",
                 days, days == 1 ? "" : "s", hours, mins, secs);
    else
        snprintf(buf, sizeof(buf), "up %lld:%02lld:%02lld", hours, mins, secs);

    return vel_val_str(buf);
#endif
}

/* ============================================================
 * whoami — current user name
 * ========================================================= */

static VELCB vel_val_t cmd_whoami(vel_t vel, size_t argc, vel_val_t *argv)
{
    (void)vel; (void)argc; (void)argv;
#ifdef WIN32
    vel_error_set(vel, "whoami not supported");
    return NULL;
#else
    struct passwd *pw = getpwuid(getuid());
    const char    *name = pw ? pw->pw_name : getenv("USER");
    if (!name) name = "unknown";
    return vel_val_str(name);
#endif
}

/* ============================================================
 * hostname — machine hostname
 * ========================================================= */

static VELCB vel_val_t cmd_hostname(vel_t vel, size_t argc, vel_val_t *argv)
{
    (void)argc; (void)argv;
#ifdef WIN32
    vel_error_set(vel, "hostname not supported");
    return NULL;
#else
    char buf[256];
    if (gethostname(buf, sizeof(buf)) != 0) {
        vel_error_set(vel, strerror(errno));
        return NULL;
    }
    return vel_val_str(buf);
#endif
}

/* ============================================================
 * uname — system information
 *   uname          -> kernel name
 *   uname -a       -> all info
 *   uname -r       -> kernel release
 *   uname -m       -> machine type
 * ========================================================= */

static VELCB vel_val_t cmd_uname(vel_t vel, size_t argc, vel_val_t *argv)
{
#ifdef WIN32
    (void)vel; (void)argc; (void)argv;
    return vel_val_str("Windows");
#else
    struct utsname u;
    if (uname(&u) != 0) {
        vel_error_set(vel, strerror(errno));
        return NULL;
    }

    int opt_a = 0, opt_r = 0, opt_m = 0, opt_s = 0, opt_n = 0;
    size_t i;

    for (i = 0; i < argc; i++) {
        const char *s = vel_str(argv[i]);
        if (s[0] == '-') {
            const char *p;
            for (p = s + 1; *p; p++) {
                if (*p == 'a') opt_a = 1;
                else if (*p == 'r') opt_r = 1;
                else if (*p == 'm') opt_m = 1;
                else if (*p == 's') opt_s = 1;
                else if (*p == 'n') opt_n = 1;
            }
        }
    }

    char buf[512];
    if (opt_a) {
        snprintf(buf, sizeof(buf), "%s %s %s %s %s",
                 u.sysname, u.nodename, u.release, u.version, u.machine);
    } else if (opt_r) {
        snprintf(buf, sizeof(buf), "%s", u.release);
    } else if (opt_m) {
        snprintf(buf, sizeof(buf), "%s", u.machine);
    } else if (opt_n) {
        snprintf(buf, sizeof(buf), "%s", u.nodename);
    } else {
        /* default: sysname */
        snprintf(buf, sizeof(buf), "%s", u.sysname);
    }
    (void)opt_s;

    return vel_val_str(buf);
#endif
}

/* ============================================================
 * which — find command in PATH
 *   which ls
 *   which -a ls    -> all matches
 * ========================================================= */

static VELCB vel_val_t cmd_which(vel_t vel, size_t argc, vel_val_t *argv)
{
    (void)vel;
#ifdef WIN32
    (void)argc; (void)argv;
    vel_error_set(vel, "which not supported on Windows");
    return NULL;
#else
    int         opt_a = 0;
    size_t      start = 0;
    const char *path_env = getenv("PATH");
    vel_val_t   out;

    if (!argc) return NULL;
    if (!strcmp(vel_str(argv[0]), "-a")) { opt_a = 1; start = 1; }
    if (start >= argc) return NULL;

    if (!path_env) path_env = "/usr/bin:/bin";

    out = val_make(NULL);
    const char *cmd = vel_str(argv[start]);

    /* if it has a slash, check directly */
    if (strchr(cmd, '/')) {
        if (access(cmd, X_OK) == 0) {
            vel_val_cat_str(out, cmd);
            vel_val_cat_ch(out, '\n');
        }
        return out;
    }

    char *pathcopy = vel_strdup(path_env);
    char *saveptr  = NULL;
    char *dir      = strtok_r(pathcopy, ":", &saveptr);
    int   found    = 0;

    while (dir) {
        char fullpath[1024];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", dir, cmd);

        if (access(fullpath, X_OK) == 0) {
            vel_val_cat_str(out, fullpath);
            vel_val_cat_ch(out, '\n');
            found = 1;
            if (!opt_a) break;
        }
        dir = strtok_r(NULL, ":", &saveptr);
    }
    free(pathcopy);

    if (!found) {
        char msg[128];
        snprintf(msg, sizeof(msg), "which: %s not found\n", cmd);
        vel_val_cat_str(out, msg);
    }

    vel_write(vel, vel_str(out));
    vel_val_free(out);
    return NULL;
#endif
}

/* ============================================================
 * env — print all environment variables
 *   env
 *   env -u VAR     -> unset VAR then print
 * ========================================================= */

extern char **environ;

static VELCB vel_val_t cmd_env(vel_t vel, size_t argc, vel_val_t *argv)
{
#ifndef WIN32
    /* handle -u VAR: unset environment variable */
    size_t i = 0;
    while (i + 1 < argc && !strcmp(vel_str(argv[i]), "-u")) {
        unsetenv(vel_str(argv[i + 1]));
        i += 2;
    }
    (void)vel;
#else
    (void)vel; (void)argc; (void)argv;
#endif
    vel_val_t out = val_make(NULL);
    char **e;
    for (e = environ; e && *e; e++) {
        vel_val_cat_str(out, *e);
        vel_val_cat_ch(out, '\n');
    }
    vel_write(vel, vel_str(out));
    vel_val_free(out);
    return NULL;
}

/* ============================================================
 * echo — print arguments
 *   echo hello world
 *   echo -n hello       -> no trailing newline
 * ========================================================= */

static VELCB vel_val_t cmd_echo(vel_t vel, size_t argc, vel_val_t *argv)
{
    (void)vel;
    int    nonl  = 0;
    size_t start = 0;
    size_t i;

    if (argc && !strcmp(vel_str(argv[0]), "-n")) { nonl = 1; start = 1; }

    vel_val_t out = val_make(NULL);
    for (i = start; i < argc; i++) {
        if (i > start) {
            vel_val_cat_ch(out, ' ');
            }
        vel_val_cat(out, argv[i]);
    }
    if (!nonl)
        vel_val_cat_ch(out, '\n');

    /* BUG FIX: echo harus write ke stdout, bukan hanya return string.
     * Tanpa ini, echo tidak menghasilkan output dalam mode script. */
    vel_write(vel, vel_str(out));
    vel_val_free(out);
    return NULL;   /* seperti print, echo tidak return value */
}

/* ============================================================
 * seq — generate number sequence
 *   seq 5            -> 1 2 3 4 5
 *   seq 2 5          -> 2 3 4 5
 *   seq 1 2 10       -> 1 3 5 7 9
 *   seq -s , 1 5     -> 1,2,3,4,5
 * ========================================================= */

static VELCB vel_val_t cmd_seq(vel_t vel, size_t argc, vel_val_t *argv)
{
    const char *sep   = "\n";
    double      from, to, step;

    if (argc >= 2 && !strcmp(vel_str(argv[0]), "-s")) {
        sep   = vel_str(argv[1]);
        argc -= 2;
        argv += 2;
    }

    if      (argc == 1) { from = 1; step = 1; to = vel_dbl(argv[0]); }
    else if (argc == 2) { from = vel_dbl(argv[0]); step = 1; to = vel_dbl(argv[1]); }
    else if (argc >= 3) { from = vel_dbl(argv[0]); step = vel_dbl(argv[1]); to = vel_dbl(argv[2]); }
    else { vel_error_set(vel, "seq: too few arguments"); return NULL; }

    if (step == 0) { vel_error_set(vel, "seq: step cannot be zero"); return NULL; }

    vel_val_t out = val_make(NULL);
    int       first = 1;
    long long n;

    /* Use integer iteration to avoid FP drift accumulating across many steps */
    long long steps = (step > 0)
        ? (long long)((to - from) / step + 1e-9) + 1
        : (long long)((from - to) / (-step) + 1e-9) + 1;
    if (steps < 0) steps = 0;

    for (n = 0; n < steps; n++) {
        double v = from + step * (double)n;
        if (!first) {
            vel_val_cat_str(out, sep);
        }
        char buf[64];
        if (fmod(v, 1.0) == 0.0) snprintf(buf, sizeof(buf), "%lld", (long long)v);
        else                      snprintf(buf, sizeof(buf), "%g", v);
        vel_val_cat_str(out, buf);
        first = 0;
    }
    vel_val_cat_ch(out, '\n');
    vel_write(vel, vel_str(out));
    vel_val_free(out);
    return NULL;
}

/* ============================================================
 * tee — write stdin-like input to file AND return it
 *   tee file {content}
 * ========================================================= */

static VELCB vel_val_t cmd_tee(vel_t vel, size_t argc, vel_val_t *argv)
{
    int         opt_a = 0;
    size_t      i     = 0;
    const char *path;
    const char *data;

    if (argc >= 1 && !strcmp(vel_str(argv[0]), "-a")) { opt_a = 1; i = 1; }
    if (i + 1 >= argc) { vel_error_set(vel, "tee: need file content"); return NULL; }

    path = vel_str(argv[i]);
    data = vel_str(argv[i + 1]);

    FILE *f = fopen(path, opt_a ? "ab" : "wb");
    if (!f) {
        char msg[256];
        snprintf(msg, sizeof(msg), "tee: %s: %s", path, strerror(errno));
        vel_error_set(vel, msg);
        return NULL;
    }
    fwrite(data, 1, strlen(data), f);
    fclose(f);

    vel_write(vel, data);
    return vel_val_str(data);
}

/* ============================================================
 * clear — clear the terminal screen
 *   clear
 * ========================================================= */

static VELCB vel_val_t cmd_clear(vel_t vel, size_t argc, vel_val_t *argv)
{
    (void)argc; (void)argv;
#ifdef WIN32
    system("cls");
#else
    /* ANSI escape: move cursor to home and clear screen */
    vel_write(vel, "\033[H\033[2J");
#endif
    return NULL;
}

/* ============================================================
 * yes — repeatedly print a string (default "y") until interrupted
 *   yes            -> print "y" forever
 *   yes string     -> print string forever
 *   yes -n N str   -> print N times (vel extension to avoid infinite loop)
 * ========================================================= */

static VELCB vel_val_t cmd_yes(vel_t vel, size_t argc, vel_val_t *argv)
{
    long long count  = -1;   /* -1 = unlimited */
    size_t    start  = 0;
    const char *str  = "y";

    if (argc >= 2 && !strcmp(vel_str(argv[0]), "-n")) {
        count = (long long)vel_int(argv[1]);
        start = 2;
    }
    if (start < argc) str = vel_str(argv[start]);

    if (count < 0) {
        /*
         * Mode unlimited: tulis langsung ke output daripada akumulasi di memori.
         * Tanpa ini loop bisa menghabiskan seluruh RAM sebelum selesai.
         * Gunakan break/Ctrl-C untuk menghentikan.
         */
        char line[4096];
        snprintf(line, sizeof(line), "%s\n", str);
        while (!vel->env->stop)
            vel_write(vel, line);
        return NULL;
    }

    /* Mode bounded (-n N): akumulasi agar bisa di-capture dengan [...] */
    vel_val_t out = val_make(NULL);
    long long i;
    for (i = 0; i < count; i++) {
        vel_val_cat_str(out, str);
        vel_val_cat_ch(out, '\n');
        if (vel->env->stop) break;
    }
    return out;
}

/* ============================================================
 * df — disk free
 *   df              -> all mounted filesystems
 *   df /path        -> filesystem containing /path
 * ========================================================= */

static VELCB vel_val_t cmd_df(vel_t vel, size_t argc, vel_val_t *argv)
{
#ifdef WIN32
    (void)vel; (void)argc; (void)argv;
    vel_error_set(vel, "df not supported on Windows");
    return NULL;
#else
    const char *path = argc ? vel_str(argv[0]) : "/";
    struct statvfs vfs;

    if (statvfs(path, &vfs) != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "df: %s: %s", path, strerror(errno));
        vel_error_set(vel, msg);
        return NULL;
    }

    unsigned long long bsize  = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
    unsigned long long total  = vfs.f_blocks * bsize;
    unsigned long long free_b = vfs.f_bfree  * bsize;
    unsigned long long used   = total - free_b;
    double             pct    = total ? (double)used / total * 100.0 : 0.0;

    char tbuf[16], ubuf[16], fbuf[16];
    fmt_size(tbuf, sizeof(tbuf), (long long)total);
    fmt_size(ubuf, sizeof(ubuf), (long long)used);
    fmt_size(fbuf, sizeof(fbuf), (long long)free_b);

    char header[] = "Filesystem       Size   Used   Free  Use%\n";
    char line[256];
    snprintf(line, sizeof(line), "%-16s %5s  %5s  %5s  %3.0f%%\n",
             path, tbuf, ubuf, fbuf, pct);

    char out[512];
    snprintf(out, sizeof(out), "%s%s", header, line);
    return vel_val_str(out);
#endif
}


/* ============================================================
 * printf - format string lalu tulis ke stdout
 *   printf {Hello %s!} world     -> tulis langsung, return NULL
 *
 * Berbeda dengan format: printf menulis ke stdout dan tidak return value.
 * Mirip printf di bash/shell.
 * ========================================================= */

/*
 * FIX: tidak lagi menduplikasi logika cmd_format dari vel_sys.c.
 * Cukup panggil "format" via vel_call, lalu write hasilnya ke output.
 *   printf {Hello %s, you are %d years old} Alice 30
 */
static VELCB vel_val_t cmd_printf_extra(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_val_t r;
    if (!argc) return NULL;
    r = vel_call(vel, "format", argc, argv);
    if (r) {
        vel_write(vel, vel_str(r));
        vel_val_free(r);
    }
    return NULL;
}

/* ============================================================
 * input - baca satu baris dari stdin
 *   input                   -> baca, return string
 *   input varname           -> baca, simpan ke var, return string
 *   input -p "prompt" var   -> tampilkan prompt, baca, simpan
 *
 * Berguna untuk script interaktif, mirip bash 'read' command.
 * ========================================================= */

static VELCB vel_val_t cmd_input(vel_t vel, size_t argc, vel_val_t *argv)
{
    char        buf[65536];
    const char *prompt  = NULL;
    const char *varname = NULL;
    size_t      i       = 0;
    size_t      len;

    while (i < argc) {
        const char *s = vel_str(argv[i]);
        if (!strcmp(s, "-p") && i + 1 < argc) {
            prompt = vel_str(argv[++i]);
        } else {
            varname = s;
        }
        i++;
    }

    if (prompt) {
        vel_write(vel, prompt);
        fflush(stdout);
    }

    if (feof(stdin)) {
        vel_error_set(vel, "input: end of file");
        return NULL;
    }

    if (!fgets(buf, sizeof(buf), stdin)) {
        if (ferror(stdin)) vel_error_set(vel, "input: read error");
        return NULL;
    }

    /* strip trailing CR/LF */
    len = strlen(buf);
    while (len && (buf[len-1] == '\n' || buf[len-1] == '\r'))
        buf[--len] = '\0';

    vel_val_t result = vel_val_str(buf);

    if (varname && varname[0])
        vel_var_set(vel, varname, result, VEL_VAR_LOCAL);

    return result;
}

/* ============================================================
 * Registration
 * ========================================================= */

void register_extra_builtins(vel_t vel)
{
    /* File/dir */
    vel_register(vel, "ls",       cmd_ls);
    vel_register(vel, "tree",     cmd_tree);
    vel_register(vel, "find",     cmd_find);
    vel_register(vel, "touch",    cmd_touch);
    vel_register(vel, "stat",     cmd_stat);
    /* Text */
    vel_register(vel, "cat",      cmd_cat);
    vel_register(vel, "head",     cmd_head);
    vel_register(vel, "tail",     cmd_tail);
    vel_register(vel, "wc",       cmd_wc);
    vel_register(vel, "grep",     cmd_grep);
    /* System */
    vel_register(vel, "uptime",   cmd_uptime);
    vel_register(vel, "whoami",   cmd_whoami);
    vel_register(vel, "hostname", cmd_hostname);
    vel_register(vel, "uname",    cmd_uname);
    vel_register(vel, "which",    cmd_which);
    vel_register(vel, "env",      cmd_env);
    /* Misc */
    vel_register(vel, "clear",    cmd_clear);
    vel_register(vel, "echo",     cmd_echo);
    vel_register(vel, "seq",      cmd_seq);
    vel_register(vel, "yes",      cmd_yes);
    vel_register(vel, "tee",      cmd_tee);
    vel_register(vel, "df",       cmd_df);
    /* BARU */
    vel_register(vel, "printf",   cmd_printf_extra);
    vel_register(vel, "input",    cmd_input);
}
