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

#ifndef VEL_PRIV_H
#define VEL_PRIV_H

/*
 * vel_priv.h  --  internal types and declarations (not part of public API)
 */

#define VEL_BUILD

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "vel.h"

#if defined(_MSC_VER) && !defined(__POCC__)
  #define atoll _atoi64
  #pragma warning(disable: 4996 4244)
#endif

/* ---- error state ---- */
#define VEL_ERR_NONE     0   /* no error */
#define VEL_ERR_SET      1   /* error with known position */
#define VEL_ERR_PENDING  2   /* error, position not yet known */

/* ---- runtime limits ---- */
#define VEL_MAX_CATCH_DEPTH 16384
#define VEL_MAX_DEPTH       1000    /* max recursion depth before controlled error */
#define VEL_UNUSED_NAME_MAX 10000   /* max iterations for vel_unused_name */
#define VEL_CB_SLOTS        VEL_CB_COUNT

/* Call stack — maximum frames tracked for error stack traces.
 * 256 > VEL_MAX_DEPTH/4 gives useful deep traces while keeping the
 * fixed-size array safely below the 1000-frame recursion limit. */
#define VEL_STACK_MAX  256

/* ---- map constants ---- */
/* FIX 1: 1024 buckets reduces average chain length by 4x vs the old 256.
 * MAP_MASK must always be (MAP_BUCKETS - 1) and MAP_BUCKETS must be a
 * power of two so the bitwise AND works correctly. */
#define MAP_BUCKETS  1024
#define MAP_MASK     0x3FF

/* ==============================================================
 * Internal structures
 * ============================================================ */

/* hash map */
typedef struct map_entry_s {
    char *key;
    void *val;
} map_entry_t;

typedef struct map_bucket_s {
    map_entry_t *entries;
    size_t       count;
} map_bucket_t;

typedef struct vel_map_s {
    map_bucket_t bucket[MAP_BUCKETS];
} vel_map_t;

/* value */
struct vel_value_s {
    size_t len;
    char  *data;
};

/* variable */
struct vel_var_s {
    char       *name;
    char       *watch;      /* code to run on assignment */
    vel_env_t   env;
    vel_val_t   value;
};

/* environment (scope) */
struct vel_env_s {
    vel_env_t   parent;
    vel_fn_t    func;
    vel_val_t   catcher_name;
    vel_var_t  *vars;
    size_t      var_count;
    vel_map_t   varmap;
    vel_val_t   retval;
    int         retval_set;
    int         stop;       /* break / return signal */
};

/* list */
struct vel_list_s {
    vel_val_t *items;
    size_t     count;
    size_t     cap;
};

/* function */
struct vel_func_s {
    char         *name;
    vel_val_t     body;
    vel_list_t    params;
    vel_fn_proc_t native;   /* NULL for scripted functions */
};

/* interpreter instance */
struct vel_s {
    /* lexer state (saved/restored during nested parse) */
    const char *src;
    const char *root_src;
    char       *root_src_buf;   /* owned copy of root_src (freed in vel_free) */
    size_t      src_len;
    size_t      pos;
    int         skip_eol;

    /* function table */
    vel_fn_t   *fn;
    size_t      fn_count;
    size_t      fn_sys;     /* number of built-in functions */
    vel_map_t   fnmap;

    /* catcher */
    char       *catcher;
    int         catcher_depth;

    /* dollar-prefix (normally "set ") */
    char       *dollar_prefix;

    /* environments */
    vel_env_t   env;        /* current */
    vel_env_t   root_env;

    /* empty value (shared, never freed) */
    vel_val_t   empty;

    /* error state */
    int         err_code;
    size_t      err_pos;
    char       *err_msg;

    /* callbacks */
    vel_cb_t    cb[VEL_CB_SLOTS];

    /* recursion depth */
    size_t      depth;

    /* FIX 5: call stack for stack traces on error.
     * Each frame stores the function name pointer (owned by vel_fn_t — no
     * copy needed) and the source position of the call site.
     * Frames 0..stack_depth-1 are valid; frame 0 = outermost call. */
    const char *stack_names[VEL_STACK_MAX];
    size_t      stack_pos  [VEL_STACK_MAX];
    size_t      stack_depth;

    /* user data */
    void       *data;

    /* template output buffer */
    char       *tmpl_buf;
    size_t      tmpl_len;
};

/* ==============================================================
 * Expression evaluator state
 * ============================================================ */
#define EXPR_INT  0
#define EXPR_FLT  1

#define EXERR_NONE    0
#define EXERR_SYNTAX  1
#define EXERR_TYPE    2
#define EXERR_DIVZERO 3
#define EXERR_STOP    4   /* soft stop — not a real error */
#define EXERR_OVERFLOW 5  /* BUG 6 FIX: integer overflow */

typedef struct expr_ctx_s {
    const char *src;
    size_t      len;
    size_t      pos;
    vel_int_t   ival;
    double      dval;
    int         type;
    int         err;
} expr_ctx_t;

/* ==============================================================
 * Shared global: last external command exit code
 * Defined in vel_sys.c, used in vel_run.c, vel_jobs.c, main.c
 * ============================================================ */
extern int g_last_exit;

/* ==============================================================
 * Internal function declarations
 * ============================================================ */

/* vel_map.c */
void  vmap_init   (vel_map_t *m);
void  vmap_free   (vel_map_t *m);
void  vmap_set    (vel_map_t *m, const char *key, void *val);
void *vmap_get    (vel_map_t *m, const char *key);
int   vmap_has    (vel_map_t *m, const char *key);

/* vel_mem.c */
char     *vel_strdup      (const char *s);
vel_val_t val_make        (const char *s);
vel_val_t val_make_len    (const char *s, size_t len);

vel_var_t mem_find_var    (vel_t vel, vel_env_t env, const char *name);
vel_var_t mem_find_local  (vel_t vel, vel_env_t env, const char *name);
vel_fn_t  mem_find_fn     (vel_t vel, const char *name);
vel_fn_t  mem_add_fn      (vel_t vel, const char *name);
void      mem_del_fn      (vel_t vel, vel_fn_t fn);

/* vel_lex.c */
vel_val_t lex_next_token  (vel_t vel);

/* vel_cmd.c */
void register_builtins(vel_t vel);

/* vel_run.c (stack trace helper — used by error printing in main.c) */
void vel_stack_trace(vel_t vel, char *buf, size_t bufsz);

#endif /* VEL_PRIV_H */
