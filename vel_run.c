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
 * vel_run.c  --  interpreter lifecycle, execution loop, and error/callback API
 *
 * g_last_exit is declared in vel_priv.h (extern) and defined in vel_sys.c.
 */

#include "vel_priv.h"
#include <time.h>

#ifndef WIN32
#  include <unistd.h>
#  include <sys/wait.h>
#  include <errno.h>
#endif

/* forward declarations from vel_lex.c */
void      lex_skip_ws  (vel_t vel);
vel_list_t lex_tokenize(vel_t vel);

/* ============================================================
 * Execution loop (core parse/run)
 * ========================================================= */

vel_val_t vel_parse(vel_t vel, const char *code, size_t len, int fnlevel)
{
    const char *saved_src = vel->src;
    size_t      saved_len = vel->src_len;
    size_t      saved_pos = vel->pos;
    vel_val_t   result    = NULL;
    vel_list_t  words     = NULL;

    if (!saved_src) {
        free(vel->root_src_buf);
        vel->root_src_buf = vel_strdup(code);
        vel->root_src = vel->root_src_buf;
    }

    vel->src     = code;
    vel->src_len = len ? len : strlen(code);
    vel->pos     = 0;
    vel->depth++;

    lex_skip_ws(vel);

#ifdef VEL_MAX_DEPTH
    if (vel->depth > VEL_MAX_DEPTH) {
        vel_error_set(vel, "recursion limit exceeded");
        goto done;
    }
#endif

    if (vel->depth == 1) vel->err_code = VEL_ERR_NONE;
    if (fnlevel)         vel->env->stop = 0;

    while (vel->pos < vel->src_len && !vel->err_code) {

        if (words)  vel_list_free(words);
        if (result) vel_val_free(result);
        result = NULL;

        /* BUG 5 FIX: capture statement start position BEFORE tokenising.
         * The old code captured vel->pos AFTER lex_tokenize() had already
         * advanced past the entire statement, so error positions always
         * pointed to the character AFTER the command rather than to the
         * command name itself. */
        size_t stmt_mark = vel->pos;

        words = lex_tokenize(vel);
        if (!words || vel->err_code) goto done;

        if (words->count) {
            const char *cmd_name = vel_str(words->items[0]);
            vel_fn_t    fn       = mem_find_fn(vel, cmd_name);

            if (!fn) {
                if (words->items[0]->len == 0) goto next_stmt;

                /* ---- auto-exec: coba jalankan sebagai external command ----
                 * Jika nama perintah mengandung '/' atau ditemukan di PATH,
                 * langsung fork+exec dengan stdin/stdout/stderr diwariskan.
                 * Ini memungkinkan: make, cmake, cargo, zig, ./build.sh, dll.
                 */
#ifndef WIN32
                {
                    const char *cmd0 = vel_str(words->items[0]);
                    int try_external = 0;

                    /* Selalu coba jika ada '/' (path eksplisit: ./build.sh, /usr/bin/make) */
                    if (strchr(cmd0, '/')) {
                        try_external = 1;
                    } else {
                        /* Cek di PATH pakai access() pada setiap dir di PATH */
                        const char *PATH = getenv("PATH");
                        if (PATH) {
                            char *pathcopy = vel_strdup(PATH);
                            char *dir, *saveptr = NULL, *p = pathcopy;
                            while ((dir = strtok_r(p, ":", &saveptr)) != NULL) {
                                char fullpath[4096];
                                snprintf(fullpath, sizeof(fullpath), "%s/%s", dir, cmd0);
                                if (access(fullpath, X_OK) == 0) {
                                    try_external = 1;
                                    break;
                                }
                                p = NULL;
                            }
                            free(pathcopy);
                        }
                    }

                    if (try_external) {
                        /* Bangun argv dari words */
                        size_t wc = words->count;
                        const char **av = malloc(sizeof(char *) * (wc + 1));
                        size_t wi;
                        pid_t  pid;
                        int    status = 0;

                        for (wi = 0; wi < wc; wi++)
                            av[wi] = vel_str(words->items[wi]);
                        av[wc] = NULL;

                        /* fork dan exec langsung — stdin/stdout/stderr diwariskan */
                        pid = fork();
                        if (pid == 0) {
                            execvp(av[0], (char *const *)av);
                            fprintf(stderr, "vel: %s: %s\n", av[0], strerror(errno));
                            _exit(127);
                        }
                        free(av);

                        if (pid > 0) {
                            waitpid(pid, &status, 0);
                            g_last_exit = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                            {
                                vel_val_t ev = vel_val_int((vel_int_t)g_last_exit);
                                vel_var_set(vel, "?", ev, VEL_VAR_GLOBAL);
                                vel_val_free(ev);
                            }
                        }
                        goto next_stmt;
                    }
                }
#endif /* !WIN32 */

                /* try catcher */
                if (vel->catcher) {
                    if (vel->catcher_depth < VEL_MAX_CATCH_DEPTH) {
                        vel_val_t packed;
                        vel->catcher_depth++;
                        vel_env_push(vel);
                        vel->env->catcher_name = words->items[0];
                        packed = vel_list_pack(words, 1);
                        vel_var_set(vel, "args", packed, VEL_VAR_LOCAL_NEW);
                        vel_val_free(packed);
                        result = vel_parse(vel, vel->catcher, 0, 1);
                        vel_env_pop(vel);
                        vel->catcher_depth--;
                    } else {
                        char *msg = malloc(words->items[0]->len + 64);
                        sprintf(msg, "catcher limit reached calling unknown function %s",
                                words->items[0]->data);
                        vel_error_set_at(vel, vel->pos, msg);
                        free(msg);
                        goto done;
                    }
                } else {
                    char *msg = malloc(words->items[0]->len + 24);
                    sprintf(msg, "unknown function: %s", words->items[0]->data);
                    vel_error_set_at(vel, vel->pos, msg);
                    free(msg);
                    goto done;
                }

            } else if (fn->native) {
                result = fn->native(vel, words->count - 1, words->items + 1);
                if (vel->err_code == VEL_ERR_PENDING) {
                    vel->err_code = VEL_ERR_SET;
                    vel->err_pos  = stmt_mark;   /* BUG 5 FIX: position before stmt */
                }
            } else {
                /* scripted function — push call stack frame */
                vel_env_push(vel);
                vel->env->func = fn;

                /* FIX 5: record call frame for stack traces */
                if (vel->stack_depth < VEL_STACK_MAX) {
                    vel->stack_names[vel->stack_depth] = fn->name;
                    vel->stack_pos  [vel->stack_depth] = vel->pos;
                    vel->stack_depth++;
                }

                if (!fn->params || !fn->body) {
                    vel_error_set_at(vel, vel->pos, "scripted function not fully defined");
                    vel_env_pop(vel);
                    /* FIX: pop the frame we just pushed so the stack trace
                     * for subsequent calls is not polluted by this failed frame */
                    if (vel->stack_depth) vel->stack_depth--;
                    goto done;
                }

                if (fn->params->count == 1 &&
                    !strcmp(vel_str(fn->params->items[0]), "args")) {
                    vel_val_t packed = vel_list_pack(words, 1);
                    vel_var_set(vel, "args", packed, VEL_VAR_LOCAL_NEW);
                    vel_val_free(packed);
                } else {
                    size_t i;
                    for (i = 0; i < fn->params->count; i++) {
                        vel_val_t arg = (i < words->count - 1)
                                        ? words->items[i + 1] : vel->empty;
                        vel_var_set(vel, vel_str(fn->params->items[i]),
                                    arg, VEL_VAR_LOCAL_NEW);
                    }
                }
                result = vel_parse_val(vel, fn->body, 1);
                vel_env_pop(vel);
                /* FIX 5: pop call frame */
                if (vel->stack_depth) vel->stack_depth--;
            }
        }

next_stmt:
        if (vel->env->stop) goto done;
        lex_skip_ws(vel);

        /* consume end-of-statement markers */
        while (vel->pos < vel->src_len
               && (vel->src[vel->pos] == '\n' || vel->src[vel->pos] == '\r'
                   || vel->src[vel->pos] == ';'))
            vel->pos++;

        lex_skip_ws(vel);
    }

done:
    if (vel->err_code && vel->cb[VEL_CB_ERROR] && vel->depth == 1) {
        vel_error_cb_t proc = (vel_error_cb_t)vel->cb[VEL_CB_ERROR];
        proc(vel, vel->err_pos, vel->err_msg);
    }

    if (words)  vel_list_free(words);
    vel->src     = saved_src;
    vel->src_len = saved_len;
    vel->pos     = saved_pos;

    if (fnlevel && vel->env->retval_set) {
        vel_val_free(result);
        result             = vel->env->retval;
        vel->env->retval   = NULL;
        vel->env->retval_set = 0;
        vel->env->stop     = 0;
    }

    vel->depth--;
    return result ? result : val_make(NULL);
}

vel_val_t vel_parse_val(vel_t vel, vel_val_t code, int fnlevel)
{
    if (!code || !code->data || !code->len) return val_make(NULL);
    return vel_parse(vel, code->data, code->len, fnlevel);
}

vel_val_t vel_call(vel_t vel, const char *name, size_t argc, vel_val_t *argv)
{
    vel_fn_t  fn = mem_find_fn(vel, name);
    vel_val_t r  = NULL;

    if (!fn) return NULL;

    if (fn->native) {
        r = fn->native(vel, argc, argv);
    } else {
        size_t i;

        if (!fn->params || !fn->body) {
            vel_error_set(vel, "scripted function not fully defined");
            return NULL;
        }

        vel_env_push(vel);
        vel->env->func = fn;

        if (fn->params->count == 1 &&
            !strcmp(vel_str(fn->params->items[0]), "args")) {
            vel_list_t  tmp = vel_list_new();
            vel_val_t   packed;
            for (i = 0; i < argc; i++)
                vel_list_push(tmp, vel_val_clone(argv[i]));
            packed = vel_list_pack(tmp, 0);
            vel_var_set(vel, "args", packed, VEL_VAR_LOCAL_NEW);
            vel_val_free(packed);
            vel_list_free(tmp);
        } else {
            for (i = 0; i < fn->params->count; i++)
                vel_var_set(vel, vel_str(fn->params->items[i]),
                            i < argc ? argv[i] : NULL, VEL_VAR_LOCAL_NEW);
        }
        r = vel_parse_val(vel, fn->body, 1);
        vel_env_pop(vel);
    }
    return r;
}

int vel_break_run(vel_t vel, int do_break)
{
    if (do_break) vel->env->stop = 1;
    return vel->env->stop;
}

/* ============================================================
 * Callback API
 * ========================================================= */

void vel_set_callback(vel_t vel, int slot, vel_cb_t proc)
{
    if (slot >= 0 && slot < VEL_CB_SLOTS)
        vel->cb[slot] = proc;
}

/* ============================================================
 * Error API
 * ========================================================= */

void vel_error_set(vel_t vel, const char *msg)
{
    if (vel->err_code) return;
    free(vel->err_msg);
    vel->err_code = VEL_ERR_PENDING;
    vel->err_pos  = 0;
    vel->err_msg  = vel_strdup(msg ? msg : "");
}

void vel_error_set_at(vel_t vel, size_t pos, const char *msg)
{
    if (vel->err_code) return;
    free(vel->err_msg);
    vel->err_code = VEL_ERR_SET;
    vel->err_pos  = pos;
    vel->err_msg  = vel_strdup(msg ? msg : "");
}

int vel_error_get(vel_t vel, const char **msg, size_t *pos)
{
    if (!vel->err_code) return 0;
    *msg           = vel->err_msg;
    *pos           = vel->err_pos;
    vel->err_code  = VEL_ERR_NONE;
    return 1;
}

/* ============================================================
 * Write helper
 * ========================================================= */

void vel_write(vel_t vel, const char *msg)
{
    if (vel->cb[VEL_CB_WRITE]) {
        vel_write_cb_t proc = (vel_write_cb_t)vel->cb[VEL_CB_WRITE];
        proc(vel, msg);
    } else {
        printf("%s", msg);
    }
}

/* ============================================================
 * FIX 5: Stack trace helper
 *
 * Builds a human-readable call stack string into buf (NUL-terminated).
 * Frame 0 = outermost scripted function call, last frame = innermost.
 * Example output:
 *   call stack:
 *     [0] assert (pos 142)
 *     [1] assertnum (pos 580)
 * ========================================================= */

void vel_stack_trace(vel_t vel, char *buf, size_t bufsz)
{
    size_t i, off = 0;
    int    n;

    if (!vel->stack_depth) {
        if (bufsz) buf[0] = '\0';
        return;
    }

    n = snprintf(buf + off, bufsz - off, "call stack:\n");
    if (n > 0) off += (size_t)n;

    for (i = 0; i < vel->stack_depth && off < bufsz - 1; i++) {
        n = snprintf(buf + off, bufsz - off,
                     "  [%u] %s (pos %u)\n",
                     (unsigned)i,
                     vel->stack_names[i] ? vel->stack_names[i] : "?",
                     (unsigned)vel->stack_pos[i]);
        if (n > 0) off += (size_t)n;
    }

    /* Warn if the call stack overflowed VEL_STACK_MAX during execution.
     * The actual depth can exceed stack_depth if recursion hit the cap. */
    if (vel->stack_depth >= VEL_STACK_MAX && off < bufsz - 1) {
        n = snprintf(buf + off, bufsz - off,
                     "  ... (stack trace truncated at %d frames)\n",
                     VEL_STACK_MAX);
        if (n > 0) off += (size_t)n;
    }
}

/* ============================================================
 * Unused-name helper
 * ========================================================= */

vel_val_t vel_unused_name(vel_t vel, const char *hint)
{
    char     *name = malloc(strlen(hint) + 64);
    vel_val_t val;
    size_t    i;

    for (i = 0; i < VEL_UNUSED_NAME_MAX; i++) {
        sprintf(name, "!!un!%s!%09u!nu!!", hint, (unsigned int)i);
        if (!mem_find_fn(vel, name) && !mem_find_var(vel, vel->env, name)) {
            val = vel_val_str(name);
            free(name);
            return val;
        }
    }
    free(name);
    vel_error_set(vel, "could not generate unused name");
    return NULL;
}

/* ============================================================
 * Interpreter lifecycle
 * ========================================================= */

vel_t vel_new(void)
{
    vel_t vel = calloc(1, sizeof(struct vel_s));
    if (!vel) return NULL;

    vel->root_env     = vel_env_new(NULL);
    vel->env          = vel->root_env;
    vel->empty        = val_make(NULL);
    vel->dollar_prefix = vel_strdup("set ");

    /* FIX: seed rand so cmd_rand doesn't always return the same sequence */
    srand((unsigned int)time(NULL) ^ (unsigned int)(size_t)vel);

    vmap_init(&vel->fnmap);
    register_builtins(vel);
    return vel;
}

void vel_free(vel_t vel)
{
    size_t i;
    if (!vel) return;

    free(vel->err_msg);
    free(vel->root_src_buf);
    vel_val_free(vel->empty);

    while (vel->env) {
        vel_env_t up = vel->env->parent;
        vel_env_free(vel->env);
        vel->env = up;
    }

    for (i = 0; i < vel->fn_count; i++) {
        if (vel->fn[i]->params) vel_list_free(vel->fn[i]->params);
        vel_val_free(vel->fn[i]->body);
        free(vel->fn[i]->name);
        free(vel->fn[i]);
    }

    vmap_free(&vel->fnmap);
    free(vel->fn);
    free(vel->dollar_prefix);
    free(vel->catcher);
    free(vel);
}
