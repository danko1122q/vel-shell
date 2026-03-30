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

#ifndef VEL_H
#define VEL_H

/*
 * vel.h  --  Vel Interpreted Language, public API
 */

#define VEL_VERSION "0.3.1"

/* ---------- integer type ---------- */
#include <stdint.h>
#include <inttypes.h>
typedef int64_t vel_int_t;
#define VEL_INT_FMT "%" PRIi64

/* ---------- platform / export ---------- */
#if defined(VELDLL) && (defined(WIN32) || defined(_WIN32))
  #ifdef VEL_BUILD
    #define VELAPI  __declspec(dllexport) __stdcall
  #else
    #define VELAPI  __declspec(dllimport) __stdcall
  #endif
  #define VELCB __stdcall
#else
  #define VELAPI
  #define VELCB
#endif

/* ---------- opaque handles ---------- */
typedef struct vel_s        *vel_t;
typedef struct vel_value_s  *vel_val_t;
typedef struct vel_func_s   *vel_fn_t;
typedef struct vel_var_s    *vel_var_t;
typedef struct vel_env_s    *vel_env_t;
typedef struct vel_list_s   *vel_list_t;

/* ---------- callback signatures ---------- */
typedef VELCB vel_val_t (*vel_fn_proc_t)  (vel_t, size_t argc, vel_val_t *argv);
typedef VELCB void      (*vel_cb_t)       (void);
typedef VELCB void      (*vel_exit_cb_t)  (vel_t, vel_val_t);
typedef VELCB void      (*vel_write_cb_t) (vel_t, const char *);
typedef VELCB char     *(*vel_read_cb_t)  (vel_t, const char *);
typedef VELCB char     *(*vel_source_cb_t)(vel_t, const char *);
typedef VELCB void      (*vel_store_cb_t) (vel_t, const char *name, const char *data);
typedef VELCB void      (*vel_error_cb_t) (vel_t, size_t pos, const char *msg);
typedef VELCB int       (*vel_setvar_cb_t)(vel_t, const char *name, vel_val_t *val);
typedef VELCB int       (*vel_getvar_cb_t)(vel_t, const char *name, vel_val_t *val);
typedef VELCB const char*(*vel_filter_cb_t)(vel_t, const char *);

/* ---------- callback slot IDs ---------- */
#define VEL_CB_EXIT     0
#define VEL_CB_WRITE    1
#define VEL_CB_READ     2
#define VEL_CB_STORE    3
#define VEL_CB_SOURCE   4
#define VEL_CB_ERROR    5
#define VEL_CB_SETVAR   6
#define VEL_CB_GETVAR   7
#define VEL_CB_FILTER   8
#define VEL_CB_COUNT    9

/* ---------- vel_set_var modes ---------- */
#define VEL_VAR_GLOBAL      0   /* write to root env */
#define VEL_VAR_LOCAL       1   /* write to current env, walk up if exists */
#define VEL_VAR_LOCAL_NEW   2   /* always create in current env;
                                   NOTE: VEL_CB_SETVAR is NOT invoked for this
                                   mode — it is used for function parameters and
                                   loop variables that are always local and
                                   should not trigger external watchers. */
#define VEL_VAR_LOCAL_ONLY  3   /* current env only, skip root */

/* ---------- template flags ---------- */
#define VEL_TMPL_NONE   0x0000

/* ============================================================
 * Interpreter lifecycle
 * ========================================================= */
VELAPI vel_t      vel_new   (void);
VELAPI void       vel_free  (vel_t vel);

/* ============================================================
 * Registration
 * ========================================================= */
VELAPI int  vel_register(vel_t vel, const char *name, vel_fn_proc_t proc);

/* ============================================================
 * Execution
 * ========================================================= */
VELAPI vel_val_t  vel_parse      (vel_t vel, const char *code, size_t len, int fnlevel);
VELAPI vel_val_t  vel_parse_val  (vel_t vel, vel_val_t code, int fnlevel);
VELAPI vel_val_t  vel_call       (vel_t vel, const char *name, size_t argc, vel_val_t *argv);
VELAPI int        vel_break_run  (vel_t vel, int do_break);

/* ============================================================
 * Callbacks
 * ========================================================= */
VELAPI void vel_set_callback(vel_t vel, int slot, vel_cb_t proc);

/* ============================================================
 * Errors
 * ========================================================= */
VELAPI void vel_error_set   (vel_t vel, const char *msg);
VELAPI void vel_error_set_at(vel_t vel, size_t pos, const char *msg);
VELAPI int  vel_error_get   (vel_t vel, const char **msg, size_t *pos);

/* ============================================================
 * Value conversion
 * ========================================================= */
VELAPI const char *vel_str  (vel_val_t val);
VELAPI double      vel_dbl  (vel_val_t val);
VELAPI vel_int_t   vel_int  (vel_val_t val);
VELAPI int         vel_bool (vel_val_t val);

/* ============================================================
 * Value allocation / mutation
 * ========================================================= */
VELAPI vel_val_t vel_val_str  (const char *s);
VELAPI vel_val_t vel_val_dbl  (double n);
VELAPI vel_val_t vel_val_int  (vel_int_t n);
VELAPI vel_val_t vel_val_clone(vel_val_t src);
VELAPI void      vel_val_free (vel_val_t val);

VELAPI int  vel_val_cat_ch      (vel_val_t val, char ch);
VELAPI int  vel_val_cat_str     (vel_val_t val, const char *s);
VELAPI int  vel_val_cat_str_len (vel_val_t val, const char *s, size_t len);
VELAPI int  vel_val_cat         (vel_val_t val, vel_val_t other);

/* ============================================================
 * Lists
 * ========================================================= */
VELAPI vel_list_t vel_list_new    (void);
VELAPI void       vel_list_free   (vel_list_t list);
VELAPI void       vel_list_push   (vel_list_t list, vel_val_t val);
VELAPI size_t     vel_list_len    (vel_list_t list);
VELAPI vel_val_t  vel_list_get    (vel_list_t list, size_t idx);
VELAPI vel_val_t  vel_list_pack   (vel_list_t list, int escape);

/* ============================================================
 * Substitution
 * ========================================================= */
VELAPI vel_list_t vel_subst_list (vel_t vel, vel_val_t code);
VELAPI vel_val_t  vel_subst_val  (vel_t vel, vel_val_t code);

/* ============================================================
 * Environments / variables
 * ========================================================= */
VELAPI vel_env_t  vel_env_new  (vel_env_t parent);
VELAPI void       vel_env_free (vel_env_t env);
VELAPI vel_env_t  vel_env_push (vel_t vel);
VELAPI void       vel_env_pop  (vel_t vel);

VELAPI vel_var_t  vel_var_set    (vel_t vel, const char *name, vel_val_t val, int mode);
VELAPI vel_val_t  vel_var_get    (vel_t vel, const char *name);
VELAPI vel_val_t  vel_var_get_or (vel_t vel, const char *name, vel_val_t def);

/* ============================================================
 * Expressions
 * ========================================================= */
VELAPI vel_val_t vel_eval_expr(vel_t vel, vel_val_t code);

/* ============================================================
 * Miscellaneous
 * ========================================================= */
VELAPI vel_val_t  vel_unused_name(vel_t vel, const char *hint);
VELAPI vel_val_t  vel_arg        (vel_val_t *argv, size_t idx);
VELAPI void       vel_set_data   (vel_t vel, void *data);
VELAPI void      *vel_get_data   (vel_t vel);
VELAPI void       vel_write      (vel_t vel, const char *msg);
VELAPI void       vel_freemem    (void *ptr);

/* ============================================================
 * Native command authoring helpers
 *
 * These macros make it harder to accidentally leak memory in native
 * command implementations.  Pattern:
 *
 *   static VELCB vel_val_t cmd_foo(vel_t vel, size_t argc, vel_val_t *argv)
 *   {
 *       char *tmp = malloc(...);
 *       ...
 *       vel_val_t r = vel_val_str(tmp);
 *       VEL_CMD_RETURN_FREE(r, tmp);   // frees tmp, returns r
 *   }
 *
 * VEL_CMD_RETURN(v)         -- return vel_val_t v, no extra cleanup
 * VEL_CMD_RETURN_FREE(v, p) -- free(p) then return v (one heap pointer)
 * VEL_CMD_RETURN_NULL       -- return NULL (empty result)
 * VEL_CMD_ERROR(msg)        -- set error and return NULL
 * ========================================================= */
#define VEL_CMD_RETURN(v)          do { return (v); } while(0)
#define VEL_CMD_RETURN_FREE(v, p)  do { vel_val_t _r = (v); free(p); return _r; } while(0)
#define VEL_CMD_RETURN_NULL        return NULL
#define VEL_CMD_ERROR(vel_, msg_)  do { vel_error_set((vel_), (msg_)); return NULL; } while(0)


VELAPI char *vel_template(vel_t vel, const char *src, unsigned int flags);

/* ============================================================
 * Diagnostics
 * ========================================================= */
/* vel_stack_trace: fills buf with a human-readable call stack.
 * Returns immediately (empty string) when no scripted frames are active.
 * Safe to call from any context after a vel_error_get(). */
VELAPI void vel_stack_trace(vel_t vel, char *buf, size_t bufsz);

#endif /* VEL_H */
