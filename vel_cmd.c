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

#include "vel_priv.h"

#ifndef WIN32
#  include <unistd.h>
#endif
#include <time.h>
#include <errno.h>

/* forward declaration from vel_sys.c */
void register_sys_builtins(vel_t vel);
void register_job_builtins(vel_t vel);
void register_extra_builtins(vel_t vel);

/* ============================================================
 * Reflection / meta
 * ========================================================= */

static VELCB vel_val_t cmd_reflect(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_fn_t     fn;
    const char  *what;
    vel_val_t    r;
    size_t       i;

    if (!argc) return NULL;
    what = vel_str(argv[0]);

    if (!strcmp(what, "version"))
        return vel_val_str(VEL_VERSION);

    if (!strcmp(what, "args")) {
        if (argc < 2) return NULL;
        fn = mem_find_fn(vel, vel_str(argv[1]));
        if (!fn || !fn->params) return NULL;
        return vel_list_pack(fn->params, 1);
    }
    if (!strcmp(what, "body")) {
        if (argc < 2) return NULL;
        fn = mem_find_fn(vel, vel_str(argv[1]));
        if (!fn || fn->native) return NULL;
        return vel_val_clone(fn->body);
    }
    if (!strcmp(what, "func-count"))
        return vel_val_int((vel_int_t)vel->fn_count);

    if (!strcmp(what, "funcs")) {
        vel_list_t list = vel_list_new();
        for (i = 0; i < vel->fn_count; i++)
            vel_list_push(list, vel_val_str(vel->fn[i]->name));
        r = vel_list_pack(list, 1);
        vel_list_free(list);
        return r;
    }
    if (!strcmp(what, "vars")) {
        vel_list_t list = vel_list_new();
        vel_env_t  env  = vel->env;
        while (env) {
            for (i = 0; i < env->var_count; i++)
                vel_list_push(list, vel_val_str(env->vars[i]->name));
            env = env->parent;
        }
        r = vel_list_pack(list, 1);
        vel_list_free(list);
        return r;
    }
    if (!strcmp(what, "globals")) {
        vel_list_t list = vel_list_new();
        for (i = 0; i < vel->root_env->var_count; i++)
            vel_list_push(list, vel_val_str(vel->root_env->vars[i]->name));
        r = vel_list_pack(list, 1);
        vel_list_free(list);
        return r;
    }
    if (!strcmp(what, "has-func")) {
        if (argc < 2) return NULL;
        return vmap_has(&vel->fnmap, vel_str(argv[1])) ? vel_val_str("1") : NULL;
    }
    if (!strcmp(what, "has-var")) {
        vel_env_t env = vel->env;
        if (argc < 2) return NULL;
        while (env) {
            if (vmap_has(&env->varmap, vel_str(argv[1]))) return vel_val_str("1");
            env = env->parent;
        }
        return NULL;
    }
    if (!strcmp(what, "has-global")) {
        if (argc < 2) return NULL;
        return vmap_has(&vel->root_env->varmap, vel_str(argv[1])) ? vel_val_str("1") : NULL;
    }
    if (!strcmp(what, "error"))
        return vel->err_msg ? vel_val_str(vel->err_msg) : NULL;

    if (!strcmp(what, "dollar-prefix")) {
        if (argc == 1) return vel_val_str(vel->dollar_prefix);
        r = vel_val_str(vel->dollar_prefix);
        free(vel->dollar_prefix);
        vel->dollar_prefix = vel_strdup(vel_str(argv[1]));
        return r;
    }
    if (!strcmp(what, "this")) {
        vel_env_t env = vel->env;
        while (env != vel->root_env && !env->catcher_name && !env->func)
            env = env->parent;
        if (env->catcher_name) return vel_val_str(vel->catcher);
        if (env == vel->root_env) return vel_val_str(vel->root_src);
        return env->func ? vel_val_clone(env->func->body) : NULL;
    }
    if (!strcmp(what, "name")) {
        vel_env_t env = vel->env;
        while (env != vel->root_env && !env->catcher_name && !env->func)
            env = env->parent;
        if (env->catcher_name) return env->catcher_name;
        if (env == vel->root_env) return NULL;
        return env->func ? vel_val_str(env->func->name) : NULL;
    }
    return NULL;
}

/* ============================================================
 * Function definition
 * ========================================================= */

static VELCB vel_val_t cmd_func(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_val_t  name;
    vel_fn_t   fn;
    vel_list_t params;

    if (!argc) return NULL;

    if (argc >= 3) {
        name   = vel_val_clone(argv[0]);
        params = vel_subst_list(vel, argv[1]);
        fn     = mem_add_fn(vel, vel_str(argv[0]));
        if (!fn) {
            vel_list_free(params);
            vel_val_free(name);
            vel_error_set(vel, "func: failed to register function");
            return NULL;
        }
        fn->params = params;
        fn->body   = vel_val_clone(argv[2]);
    } else {
        name = vel_unused_name(vel, "anon");
        if (!name) return NULL;
        if (argc < 2) {
            vel_val_t tmp = vel_val_str("args");
            params = vel_subst_list(vel, tmp);
            vel_val_free(tmp);
            fn = mem_add_fn(vel, vel_str(name));
            if (!fn) {
                vel_list_free(params);
                vel_val_free(name);
                vel_error_set(vel, "func: failed to register function");
                return NULL;
            }
            fn->params = params;
            fn->body   = vel_val_clone(argv[0]);
        } else {
            params = vel_subst_list(vel, argv[0]);
            fn     = mem_add_fn(vel, vel_str(name));
            if (!fn) {
                vel_list_free(params);
                vel_val_free(name);
                vel_error_set(vel, "func: failed to register function");
                return NULL;
            }
            fn->params = params;
            fn->body   = vel_val_clone(argv[1]);
        }
    }
    return name;
}

static VELCB vel_val_t cmd_rename(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_val_t   r;
    vel_fn_t    fn;
    const char *from, *to;

    if (argc < 2) return NULL;
    from = vel_str(argv[0]);
    to   = vel_str(argv[1]);
    fn   = mem_find_fn(vel, from);

    if (!fn) {
        char *msg = malloc(32 + strlen(from));
        sprintf(msg, "unknown function '%s'", from);
        vel_error_set_at(vel, vel->pos, msg);
        free(msg);
        return NULL;
    }

    r = vel_val_str(fn->name);
    if (to[0]) {
        vmap_set(&vel->fnmap, from, NULL);
        vmap_set(&vel->fnmap, to, fn);
        free(fn->name);
        fn->name = vel_strdup(to);
    } else {
        mem_del_fn(vel, fn);
    }
    return r;
}

static VELCB vel_val_t cmd_unusedname(vel_t vel, size_t argc, vel_val_t *argv)
{
    return vel_unused_name(vel, argc > 0 ? vel_str(argv[0]) : "unusedname");
}

/* ============================================================
 * Variables
 * ========================================================= */

static VELCB vel_val_t cmd_quote(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_val_t r;
    size_t i;
    (void)vel;
    if (!argc) return NULL;
    r = val_make(NULL);
    for (i = 0; i < argc; i++) {
        if (i) vel_val_cat_ch(r, ' ');
        vel_val_cat(r, argv[i]);
    }
    return r;
}

static VELCB vel_val_t cmd_set(vel_t vel, size_t argc, vel_val_t *argv)
{
    size_t    i      = 0;
    vel_var_t var    = NULL;
    int       mode   = VEL_VAR_LOCAL;

    if (!argc) return NULL;
    if (!strcmp(vel_str(argv[0]), "global")) { i = 1; mode = VEL_VAR_GLOBAL; }

    while (i < argc) {
        if (argc == i + 1)
            return vel_val_clone(vel_var_get(vel, vel_str(argv[i])));
        var = vel_var_set(vel, vel_str(argv[i]), argv[i + 1], mode);
        i += 2;
    }
    return var ? vel_val_clone(var->value) : NULL;
}

static VELCB vel_val_t cmd_local(vel_t vel, size_t argc, vel_val_t *argv)
{
    size_t i;
    for (i = 0; i < argc; i++) {
        const char *name = vel_str(argv[i]);
        if (!mem_find_local(vel, vel->env, name))
            vel_var_set(vel, name, vel->empty, VEL_VAR_LOCAL_NEW);
    }
    return NULL;
}

/* ============================================================
 * Output
 * ========================================================= */

static VELCB vel_val_t cmd_write(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_val_t msg = val_make(NULL);
    size_t i;
    for (i = 0; i < argc; i++) {
        if (i) vel_val_cat_ch(msg, ' ');
        vel_val_cat(msg, argv[i]);
    }
    vel_write(vel, vel_str(msg));
    vel_val_free(msg);
    return NULL;
}

static VELCB vel_val_t cmd_print(vel_t vel, size_t argc, vel_val_t *argv)
{
    cmd_write(vel, argc, argv);
    vel_write(vel, "\n");
    return NULL;
}

/* ============================================================
 * Eval variants
 * ========================================================= */

static vel_val_t do_eval(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_val_t joined, r;
    size_t    i;
    int       saved_stop;

    if (!argc) return NULL;

    saved_stop     = vel->env->stop;
    vel->env->stop = 0;

    if (argc == 1) {
        r = vel_parse_val(vel, argv[0], 0);
    } else {
        joined = val_make(NULL);
        for (i = 0; i < argc; i++) {
            if (i) vel_val_cat_ch(joined, ' ');
            vel_val_cat(joined, argv[i]);
        }
        r = vel_parse_val(vel, joined, 0);
        vel_val_free(joined);
    }

    if (vel->env->retval_set) {
        vel_val_free(r);
        r                    = vel->env->retval;
        vel->env->retval     = NULL;
        vel->env->retval_set = 0;
    }

    vel->env->stop = saved_stop;
    return r;
}

static VELCB vel_val_t cmd_eval(vel_t vel, size_t argc, vel_val_t *argv)
{
    return do_eval(vel, argc, argv);
}

static VELCB vel_val_t cmd_topeval(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_env_t cur = vel->env;
    vel_val_t r;
    vel->env = vel->root_env;
    r = do_eval(vel, argc, argv);
    vel->env = cur;
    return r;
}

static VELCB vel_val_t cmd_upeval(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_env_t cur = vel->env;
    vel_val_t r;
    if (cur == vel->root_env) return do_eval(vel, argc, argv);
    vel->env = cur->parent;
    r = do_eval(vel, argc, argv);
    vel->env = cur;
    return r;
}

static VELCB vel_val_t cmd_enveval(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_list_t   invars = NULL, outvars = NULL;
    vel_val_t   *saved  = NULL;
    int          codeidx;
    size_t       i;
    vel_val_t    r;

    if (!argc) return NULL;
    if (argc == 1) { codeidx = 0; }
    else {
        invars  = vel_subst_list(vel, argv[0]);
        saved   = malloc(sizeof(vel_val_t) * vel_list_len(invars));
        for (i = 0; i < vel_list_len(invars); i++)
            saved[i] = vel_val_clone(vel_var_get(vel, vel_str(vel_list_get(invars, i))));
        if (argc > 2) { codeidx = 2; outvars = vel_subst_list(vel, argv[1]); }
        else          { codeidx = 1; }
    }

    vel_env_push(vel);
    if (invars) {
        for (i = 0; i < vel_list_len(invars); i++) {
            vel_var_set(vel, vel_str(vel_list_get(invars, i)),
                        saved[i], VEL_VAR_LOCAL_NEW);
            vel_val_free(saved[i]);
        }
    }
    r = vel_parse_val(vel, argv[codeidx], 0);

    if (invars) {
        vel_list_t  target = outvars ? outvars : invars;
        saved = realloc(saved, sizeof(vel_val_t) * vel_list_len(target));
        for (i = 0; i < vel_list_len(target); i++)
            saved[i] = vel_val_clone(vel_var_get(vel, vel_str(vel_list_get(target, i))));
    }
    vel_env_pop(vel);

    if (invars) {
        vel_list_t target = outvars ? outvars : invars;
        for (i = 0; i < vel_list_len(target); i++) {
            vel_var_set(vel, vel_str(vel_list_get(target, i)), saved[i], VEL_VAR_LOCAL);
            vel_val_free(saved[i]);
        }
        vel_list_free(invars);
        if (outvars) vel_list_free(outvars);
        free(saved);
    }
    return r;
}

static VELCB vel_val_t cmd_jaileval(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_t     sub;
    vel_val_t r;
    size_t    base = 0, i;

    if (!argc) return NULL;
    if (!strcmp(vel_str(argv[0]), "clean")) { base = 1; if (argc == 1) return NULL; }

    sub = vel_new();
    if (!base) {
        for (i = vel->fn_sys; i < vel->fn_count; i++) {
            vel_fn_t fn = vel->fn[i];
            if (!fn->native) continue;
            vel_register(sub, fn->name, fn->native);
        }
    }
    r = vel_parse_val(sub, argv[base], 1);
    vel_free(sub);
    return r;
}

/* ============================================================
 * Control flow
 * ========================================================= */

static VELCB vel_val_t cmd_return(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel->env->stop       = 1;
    vel_val_free(vel->env->retval);
    vel->env->retval     = argc ? vel_val_clone(argv[0]) : NULL;
    vel->env->retval_set = 1;
    return vel_val_clone(vel->env->retval);
}

static VELCB vel_val_t cmd_result(vel_t vel, size_t argc, vel_val_t *argv)
{
    if (argc > 0) {
        vel_val_free(vel->env->retval);
        vel->env->retval     = vel_val_clone(argv[0]);
        vel->env->retval_set = 1;
    }
    return vel->env->retval_set ? vel_val_clone(vel->env->retval) : NULL;
}

/* break — stop loop iteration, do NOT propagate as return */
static VELCB vel_val_t cmd_break(vel_t vel, size_t argc, vel_val_t *argv)
{
    (void)argc; (void)argv;
    vel->env->stop = 1;
    /* Ensure retval_set is 0 so loops know this is a break, not a return */
    return NULL;
}

/* continue — skip rest of body, next iteration
 * Implemented by stopping current body and letting the loop re-evaluate cond.
 * We mark env with a special continue flag via the stop bit but clear retval_set.
 */
static VELCB vel_val_t cmd_continue(vel_t vel, size_t argc, vel_val_t *argv)
{
    (void)argc; (void)argv;
    vel->env->stop = 2;   /* 2 = continue signal */
    return NULL;
}

static VELCB vel_val_t cmd_if(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_val_t cond;
    int base = 0, inv = 0, truth;

    if (!argc) return NULL;
    if (!strcmp(vel_str(argv[0]), "not")) { base = inv = 1; }
    if (argc < (size_t)base + 2) return NULL;

    cond = vel_eval_expr(vel, argv[base]);
    if (!cond || vel->err_code) return NULL;
    truth = vel_bool(cond);
    if (inv) truth = !truth;
    vel_val_free(cond);

    if (truth)                        return vel_parse_val(vel, argv[base + 1], 0);
    if (argc > (size_t)base + 2)      return vel_parse_val(vel, argv[base + 2], 0);
    return NULL;
}

/* not — standalone boolean negation
 *   not $x        -> 1 if $x is falsy
 *   not [expr]    -> inverted
 */
static VELCB vel_val_t cmd_not(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_val_t v;
    int truth;
    if (!argc) return vel_val_int(1);
    /* try numeric expression first; swallow only expression-parse failures,
     * not genuine interpreter errors (div-by-zero, etc.) */
    v = vel_eval_expr(vel, argv[0]);
    if (vel->err_code) {
        /* Only suppress a non-numeric / non-parseable expression — treat
         * the argument as a plain string truth value instead.  Real errors
         * (e.g. division by zero) are left intact so the caller sees them. */
        if (v != NULL) {
            /* vel_eval_expr returned a value despite err_code — shouldn't
             * happen, but free it to be safe */
            vel_val_free(v);
        }
        vel->err_code = VEL_ERR_NONE;
        free(vel->err_msg);
        vel->err_msg = NULL;
        v = vel_val_clone(argv[0]);
    }
    truth = vel_bool(v);
    vel_val_free(v);
    return vel_val_int(!truth);
}

static VELCB vel_val_t cmd_while(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_val_t cond, r = NULL;
    int base = 0, inv = 0;

    if (!argc) return NULL;
    if (!strcmp(vel_str(argv[0]), "not")) { base = inv = 1; }
    if (argc < (size_t)base + 2) return NULL;

    while (!vel->err_code && !vel->env->stop) {
        cond = vel_eval_expr(vel, argv[base]);
        if (!cond || vel->err_code) return NULL;
        int truth = vel_bool(cond);
        if (inv) truth = !truth;
        vel_val_free(cond);
        if (!truth) break;
        vel_val_free(r);
        r = vel_parse_val(vel, argv[base + 1], 0);
        /* handle continue (stop==2): reset stop, continue loop */
        if (vel->env->stop == 2 && !vel->env->retval_set)
            vel->env->stop = 0;
    }

    /* consume break signal (stop==1, no retval) */
    if (vel->env->stop && !vel->env->retval_set)
        vel->env->stop = 0;

    return r;
}

static VELCB vel_val_t cmd_for(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_val_t cond, r = NULL;
    if (argc < 4) return NULL;

    vel_val_free(vel_parse_val(vel, argv[0], 0));
    while (!vel->err_code && !vel->env->stop) {
        cond = vel_eval_expr(vel, argv[1]);
        if (!cond || vel->err_code) return NULL;
        if (!vel_bool(cond)) { vel_val_free(cond); break; }
        vel_val_free(cond);
        vel_val_free(r);
        r = vel_parse_val(vel, argv[3], 0);
        /* handle continue */
        if (vel->env->stop == 2 && !vel->env->retval_set)
            vel->env->stop = 0;
        if (!vel->env->stop)
            vel_val_free(vel_parse_val(vel, argv[2], 0));
    }

    if (vel->env->stop && !vel->env->retval_set)
        vel->env->stop = 0;

    return r;
}

/* ============================================================
 * Lists
 * ========================================================= */

static VELCB vel_val_t cmd_count(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_list_t list;
    vel_val_t  r;
    if (!argc) return vel_val_int(0);
    list = vel_subst_list(vel, argv[0]);
    r    = vel_val_int((vel_int_t)vel_list_len(list));
    vel_list_free(list);
    return r;
}

static VELCB vel_val_t cmd_index(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_list_t list;
    vel_val_t  r;
    size_t     idx;
    if (argc < 2) return NULL;
    list = vel_subst_list(vel, argv[0]);
    idx  = (size_t)vel_int(argv[1]);
    r    = idx < vel_list_len(list) ? vel_val_clone(vel_list_get(list, idx)) : NULL;
    vel_list_free(list);
    return r;
}

static VELCB vel_val_t cmd_indexof(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_list_t list;
    vel_val_t  r = NULL;
    size_t     i;
    if (argc < 2) return NULL;
    list = vel_subst_list(vel, argv[0]);
    for (i = 0; i < vel_list_len(list); i++) {
        if (!strcmp(vel_str(vel_list_get(list, i)), vel_str(argv[1]))) {
            r = vel_val_int((vel_int_t)i);
            break;
        }
    }
    vel_list_free(list);
    return r;
}

static VELCB vel_val_t cmd_append(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_list_t  list;
    vel_val_t   r;
    size_t      i, base = 1;
    int         mode = VEL_VAR_LOCAL;
    const char *vname;

    if (argc < 2) return NULL;
    vname = vel_str(argv[0]);
    if (!strcmp(vname, "global")) {
        if (argc < 3) return NULL;
        vname = vel_str(argv[1]);
        base  = 2;
        mode  = VEL_VAR_GLOBAL;
    }

    list = vel_subst_list(vel, vel_var_get(vel, vname));
    for (i = base; i < argc; i++)
        vel_list_push(list, vel_val_clone(argv[i]));
    r = vel_list_pack(list, 1);
    vel_list_free(list);
    vel_var_set(vel, vname, r, mode);
    return r;
}

static VELCB vel_val_t cmd_slice(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_list_t list, out;
    vel_val_t  r;
    vel_int_t  from, to;
    size_t     i;

    if (!argc) return NULL;
    if (argc < 2) return vel_val_clone(argv[0]);

    from = vel_int(argv[1]);
    if (from < 0) from = 0;

    list = vel_subst_list(vel, argv[0]);
    to   = argc > 2 ? vel_int(argv[2]) : (vel_int_t)vel_list_len(list);
    if (to > (vel_int_t)vel_list_len(list)) to = (vel_int_t)vel_list_len(list);
    if (to < from) to = from;

    out = vel_list_new();
    for (i = (size_t)from; i < (size_t)to; i++)
        vel_list_push(out, vel_val_clone(vel_list_get(list, i)));
    vel_list_free(list);
    r = vel_list_pack(out, 1);
    vel_list_free(out);
    return r;
}

static VELCB vel_val_t cmd_filter(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_list_t  list, out;
    vel_val_t   r;
    size_t      i, base = 0;
    const char *vname = "x";

    if (argc < 2) return argc ? vel_val_clone(argv[0]) : NULL;
    if (argc > 2) { base = 1; vname = vel_str(argv[0]); }

    list = vel_subst_list(vel, argv[base]);
    out  = vel_list_new();

    /* FIX: use env push/pop to properly scope the iterator variable */
    vel_env_push(vel);
    for (i = 0; i < vel_list_len(list) && !vel->env->stop; i++) {
        vel_var_set(vel, vname, vel_list_get(list, i), VEL_VAR_LOCAL_NEW);
        r = vel_eval_expr(vel, argv[base + 1]);
        if (vel_bool(r)) vel_list_push(out, vel_val_clone(vel_list_get(list, i)));
        vel_val_free(r);
    }
    vel_env_pop(vel);

    vel_list_free(list);
    r = vel_list_pack(out, 1);
    vel_list_free(out);
    return r;
}

static VELCB vel_val_t cmd_list(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_list_t list = vel_list_new();
    vel_val_t  r;
    size_t     i;
    (void)vel;
    for (i = 0; i < argc; i++)
        vel_list_push(list, vel_val_clone(argv[i]));
    r = vel_list_pack(list, 1);
    vel_list_free(list);
    return r;
}

static VELCB vel_val_t cmd_subst(vel_t vel, size_t argc, vel_val_t *argv)
{
    if (!argc) return NULL;
    return vel_subst_val(vel, argv[0]);
}

static VELCB vel_val_t cmd_concat(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_val_t r = val_make(NULL);
    size_t    i;
    for (i = 0; i < argc; i++) {
        vel_list_t list = vel_subst_list(vel, argv[i]);
        vel_val_t  tmp  = vel_list_pack(list, 1);
        vel_list_free(list);
        if (i && r->len && tmp->len) vel_val_cat_ch(r, ' ');
        vel_val_cat(r, tmp);
        vel_val_free(tmp);
    }
    return r;
}

static VELCB vel_val_t cmd_foreach(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_list_t  list, rlist;
    vel_val_t   r;
    size_t      i, listidx = 0, codeidx = 1;
    const char *vname = "i";

    if (argc < 2) return NULL;
    if (argc >= 3) { vname = vel_str(argv[0]); listidx = 1; codeidx = 2; }

    list  = vel_subst_list(vel, argv[listidx]);
    rlist = vel_list_new();

    /* FIX: push a scope so the iterator variable doesn't leak to caller */
    vel_env_push(vel);

    for (i = 0; i < vel_list_len(list); i++) {
        vel_val_t rv;
        vel_var_set(vel, vname, vel_list_get(list, i), VEL_VAR_LOCAL_NEW);
        rv = vel_parse_val(vel, argv[codeidx], 0);
        /* handle continue */
        if (vel->env->stop == 2 && !vel->env->retval_set) {
            vel->env->stop = 0;
            vel_val_free(rv);
            continue;
        }
        if (rv->len) vel_list_push(rlist, rv);
        else         vel_val_free(rv);
        if (vel->env->stop || vel->err_code) break;
    }

    /* propagate break/return before popping */
    {
        int   stop_sig  = vel->env->stop;
        int   ret_set   = vel->env->retval_set;
        vel_val_t retv  = ret_set ? vel_val_clone(vel->env->retval) : NULL;
        vel_env_pop(vel);
        if (stop_sig && !ret_set)
            vel->env->stop = 0;   /* consume break */
        else if (ret_set) {
            vel_val_free(vel->env->retval);
            vel->env->retval     = retv;
            vel->env->retval_set = 1;
            vel->env->stop       = stop_sig;
        } else {
            vel_val_free(retv);
        }
    }

    r = vel_list_pack(rlist, 1);
    vel_list_free(list);
    vel_list_free(rlist);
    return r;
}

static VELCB vel_val_t cmd_lmap(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_list_t list;
    size_t     i;
    if (argc < 2) return NULL;
    list = vel_subst_list(vel, argv[0]);
    for (i = 1; i < argc; i++)
        vel_var_set(vel, vel_str(argv[i]), vel_list_get(list, i - 1), VEL_VAR_LOCAL);
    vel_list_free(list);
    return NULL;
}

/* join — join list elements with a separator
 *   join $list           -> elements joined with space
 *   join $list ,         -> elements joined with ","
 *   join $list {, }      -> elements joined with ", "
 */
static VELCB vel_val_t cmd_join(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_list_t  list;
    vel_val_t   r;
    const char *sep = " ";
    size_t      i;

    if (!argc) return NULL;
    if (argc > 1) sep = vel_str(argv[1]);

    list = vel_subst_list(vel, argv[0]);
    r    = val_make(NULL);

    for (i = 0; i < vel_list_len(list); i++) {
        if (i && sep[0]) vel_val_cat_str(r, sep);
        vel_val_cat(r, vel_list_get(list, i));
    }
    vel_list_free(list);
    return r;
}

/* ============================================================
 * Math / expressions
 * ========================================================= */

static VELCB vel_val_t cmd_expr(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_val_t joined, r;
    size_t    i;
    if (!argc) return NULL;
    if (argc == 1) return vel_eval_expr(vel, argv[0]);
    joined = val_make(NULL);
    for (i = 0; i < argc; i++) {
        if (i) vel_val_cat_ch(joined, ' ');
        vel_val_cat(joined, argv[i]);
    }
    r = vel_eval_expr(vel, joined);
    vel_val_free(joined);
    return r;
}

static vel_val_t do_inc(vel_t vel, const char *vname, double delta)
{
    double    dv  = vel_dbl(vel_var_get(vel, vname)) + delta;
    vel_val_t pv  = fmod(dv, 1.0) ? vel_val_dbl(dv) : vel_val_int((vel_int_t)dv);
    vel_var_set(vel, vname, pv, VEL_VAR_LOCAL);
    return pv;
}

static VELCB vel_val_t cmd_inc(vel_t vel, size_t argc, vel_val_t *argv)
{
    if (!argc) return NULL;
    return do_inc(vel, vel_str(argv[0]), argc > 1 ? vel_dbl(argv[1]) : 1.0);
}

static VELCB vel_val_t cmd_dec(vel_t vel, size_t argc, vel_val_t *argv)
{
    if (!argc) return NULL;
    return do_inc(vel, vel_str(argv[0]), -(argc > 1 ? vel_dbl(argv[1]) : 1.0));
}

/* FIX: rand is now seeded in vel_new() via srand(time^ptr) */
static VELCB vel_val_t cmd_rand(vel_t vel, size_t argc, vel_val_t *argv)
{
    (void)vel;
    if (argc >= 2) {
        /* rand min max -> integer in [min, max] */
        vel_int_t lo = vel_int(argv[0]);
        vel_int_t hi = vel_int(argv[1]);
        if (hi <= lo) return vel_val_int(lo);
        return vel_val_int(lo + (vel_int_t)(rand() % (int)(hi - lo + 1)));
    }
    if (argc == 1) {
        /* rand N -> integer in [0, N-1] */
        vel_int_t n = vel_int(argv[0]);
        if (n <= 0) return vel_val_int(0);
        return vel_val_int((vel_int_t)(rand() % (int)n));
    }
    /* rand -> float [0.0, 1.0) */
    return vel_val_dbl(rand() / ((double)RAND_MAX + 1.0));
}

/* sleep — pause execution
 *   sleep 1      -> sleep 1 second (integer)
 *   sleep 0.5    -> sleep 500ms (float, unix only)
 */
static VELCB vel_val_t cmd_sleep(vel_t vel, size_t argc, vel_val_t *argv)
{
    (void)vel;
    if (!argc) return NULL;
#ifdef WIN32
    {
        double ms = vel_dbl(argv[0]) * 1000.0;
        Sleep((DWORD)(ms < 0 ? 0 : ms));
    }
#else
    {
        double secs = vel_dbl(argv[0]);
        if (secs <= 0) return NULL;
        if (secs >= 1.0) {
            unsigned int s = (unsigned int)secs;
            sleep(s);
            secs -= s;
        }
        if (secs > 0) {
            struct timespec ts;
            ts.tv_sec  = 0;
            ts.tv_nsec = (long)(secs * 1e9);
            nanosleep(&ts, NULL);
        }
    }
#endif
    return NULL;
}

/* ============================================================
 * String operations
 * ========================================================= */

static VELCB vel_val_t cmd_char(vel_t vel, size_t argc, vel_val_t *argv)
{
    char s[2];
    (void)vel;
    if (!argc) return NULL;
    s[0] = (char)vel_int(argv[0]);
    s[1] = '\0';
    return vel_val_str(s);
}

static VELCB vel_val_t cmd_charat(vel_t vel, size_t argc, vel_val_t *argv)
{
    const char *s;
    size_t      idx;
    char        buf[2];
    (void)vel;
    if (argc < 2) return NULL;
    s   = vel_str(argv[0]);
    idx = (size_t)vel_int(argv[1]);
    if (idx >= strlen(s)) return NULL;
    buf[0] = s[idx]; buf[1] = '\0';
    return vel_val_str(buf);
}

static VELCB vel_val_t cmd_codeat(vel_t vel, size_t argc, vel_val_t *argv)
{
    const char *s;
    size_t      idx;
    (void)vel;
    if (argc < 2) return NULL;
    s   = vel_str(argv[0]);
    idx = (size_t)vel_int(argv[1]);
    if (idx >= strlen(s)) return NULL;
    return vel_val_int((vel_int_t)(unsigned char)s[idx]);
}

static VELCB vel_val_t cmd_substr(vel_t vel, size_t argc, vel_val_t *argv)
{
    const char *s;
    size_t      slen, start, end;
    (void)vel;
    if (argc < 2) return NULL;
    s     = vel_str(argv[0]);
    slen  = strlen(s);
    start = (size_t)atoll(vel_str(argv[1]));
    end   = argc > 2 ? (size_t)atoll(vel_str(argv[2])) : slen;
    if (end > slen) end = slen;
    if (start >= end) return NULL;
    return val_make_len(s + start, end - start);
}

static VELCB vel_val_t cmd_strpos(vel_t vel, size_t argc, vel_val_t *argv)
{
    const char *hay, *needle, *found;
    size_t      offset = 0;
    (void)vel;
    if (argc < 2) return vel_val_int(-1);
    hay    = vel_str(argv[0]);
    needle = vel_str(argv[1]);
    if (argc > 2) {
        offset = (size_t)atoll(vel_str(argv[2]));
        if (offset >= strlen(hay)) return vel_val_int(-1);
    }
    found = strstr(hay + offset, needle);
    return found ? vel_val_int((vel_int_t)(found - hay)) : vel_val_int(-1);
}

static VELCB vel_val_t cmd_length(vel_t vel, size_t argc, vel_val_t *argv)
{
    size_t total = 0, i;
    (void)vel;
    for (i = 0; i < argc; i++) {
        if (i) total++;
        total += strlen(vel_str(argv[i]));
    }
    return vel_val_int((vel_int_t)total);
}

static vel_val_t do_trim(const char *s, const char *chars, int left, int right)
{
    int    base = 0;
    size_t len;
    char  *buf;
    vel_val_t r;

    if (left)
        while (s[base] && strchr(chars, s[base])) base++;

    buf = vel_strdup(s + base);
    len = strlen(buf);

    if (right)
        while (len && strchr(chars, buf[len - 1])) len--;
    buf[len] = '\0';

    r = vel_val_str(buf);
    free(buf);
    return r;
}

static VELCB vel_val_t cmd_trim(vel_t vel, size_t argc, vel_val_t *argv)
{
    (void)vel;
    if (!argc) return NULL;
    return do_trim(vel_str(argv[0]), argc < 2 ? " \f\n\r\t\v" : vel_str(argv[1]), 1, 1);
}

static VELCB vel_val_t cmd_ltrim(vel_t vel, size_t argc, vel_val_t *argv)
{
    (void)vel;
    if (!argc) return NULL;
    return do_trim(vel_str(argv[0]), argc < 2 ? " \f\n\r\t\v" : vel_str(argv[1]), 1, 0);
}

static VELCB vel_val_t cmd_rtrim(vel_t vel, size_t argc, vel_val_t *argv)
{
    (void)vel;
    if (!argc) return NULL;
    return do_trim(vel_str(argv[0]), argc < 2 ? " \f\n\r\t\v" : vel_str(argv[1]), 0, 1);
}

static VELCB vel_val_t cmd_strcmp(vel_t vel, size_t argc, vel_val_t *argv)
{
    (void)vel;
    if (argc < 2) return NULL;
    return vel_val_int(strcmp(vel_str(argv[0]), vel_str(argv[1])));
}

static VELCB vel_val_t cmd_streq(vel_t vel, size_t argc, vel_val_t *argv)
{
    (void)vel;
    if (argc < 2) return NULL;
    return vel_val_int(strcmp(vel_str(argv[0]), vel_str(argv[1])) ? 0 : 1);
}

static VELCB vel_val_t cmd_repstr(vel_t vel, size_t argc, vel_val_t *argv)
{
    const char *from, *to, *sub;
    char       *buf;
    size_t      buflen, fromlen, tolen, idx;
    vel_val_t   r;
    (void)vel;

    if (argc < 3) return argc ? vel_val_clone(argv[0]) : NULL;
    from = vel_str(argv[1]);
    to   = vel_str(argv[2]);
    if (!from[0]) return NULL;

    buf     = vel_strdup(vel_str(argv[0]));
    buflen  = strlen(buf);
    fromlen = strlen(from);
    tolen   = strlen(to);

    while ((sub = strstr(buf, from))) {
        char  *nw = malloc(buflen - fromlen + tolen + 1);
        idx = (size_t)(sub - buf);
        if (idx) memcpy(nw, buf, idx);
        memcpy(nw + idx, to, tolen);
        memcpy(nw + idx + tolen, buf + idx + fromlen, buflen - idx - fromlen);
        buflen = buflen - fromlen + tolen;
        free(buf);
        buf = nw;
        buf[buflen] = '\0';
    }

    r = vel_val_str(buf);
    free(buf);
    return r;
}

static VELCB vel_val_t cmd_split(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_list_t  list;
    vel_val_t   cur, r;
    const char *s, *sep = " ";
    size_t      i;
    (void)vel;

    if (!argc) return NULL;
    if (argc > 1) {
        sep = vel_str(argv[1]);
        if (!sep[0]) return vel_val_clone(argv[0]);
    }

    list = vel_list_new();
    cur  = val_make(NULL);
    s    = vel_str(argv[0]);

    for (i = 0; s[i]; i++) {
        if (strchr(sep, s[i])) {
            vel_list_push(list, cur);
            cur = val_make(NULL);
        } else {
            vel_val_cat_ch(cur, s[i]);
        }
    }
    vel_list_push(list, cur);
    r = vel_list_pack(list, 1);
    vel_list_free(list);
    return r;
}

static VELCB vel_val_t cmd_toupper(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_val_t r;
    const char *s;
    size_t i;
    (void)vel;
    if (!argc) return NULL;
    s = vel_str(argv[0]);
    r = val_make(NULL);
    for (i = 0; s[i]; i++)
        vel_val_cat_ch(r, (char)toupper((unsigned char)s[i]));
    return r;
}

static VELCB vel_val_t cmd_tolower(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_val_t r;
    const char *s;
    size_t i;
    (void)vel;
    if (!argc) return NULL;
    s = vel_str(argv[0]);
    r = val_make(NULL);
    for (i = 0; s[i]; i++)
        vel_val_cat_ch(r, (char)tolower((unsigned char)s[i]));
    return r;
}

/* startswith — check if string starts with prefix
 *   startswith $str prefix    -> 1 or ""
 */
static VELCB vel_val_t cmd_startswith(vel_t vel, size_t argc, vel_val_t *argv)
{
    const char *s, *prefix;
    size_t plen;
    (void)vel;
    if (argc < 2) return NULL;
    s      = vel_str(argv[0]);
    prefix = vel_str(argv[1]);
    plen   = strlen(prefix);
    return (strncmp(s, prefix, plen) == 0) ? vel_val_int(1) : NULL;
}

/* endswith — check if string ends with suffix
 *   endswith $str suffix    -> 1 or ""
 */
static VELCB vel_val_t cmd_endswith(vel_t vel, size_t argc, vel_val_t *argv)
{
    const char *s, *suffix;
    size_t slen, suflen;
    (void)vel;
    if (argc < 2) return NULL;
    s      = vel_str(argv[0]);
    suffix = vel_str(argv[1]);
    slen   = strlen(s);
    suflen = strlen(suffix);
    if (suflen > slen) return NULL;
    return (strcmp(s + slen - suflen, suffix) == 0) ? vel_val_int(1) : NULL;
}

/* ============================================================
 * I/O
 * ========================================================= */

static char *read_file(const char *path)
{
    FILE  *f = fopen(path, "rb");
    char  *buf;
    long   sz_raw;
    size_t sz;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    sz_raw = ftell(f);
    if (sz_raw < 0)                  { fclose(f); return NULL; }
    sz = (size_t)sz_raw;
    fseek(f, 0, SEEK_SET);
    buf = malloc(sz + 1);
    if (buf) {
        size_t n = fread(buf, 1, sz, f);
        buf[n] = '\0';
    }
    fclose(f);
    return buf;
}

static VELCB vel_val_t cmd_read(vel_t vel, size_t argc, vel_val_t *argv)
{
    char     *buf;
    vel_val_t r;
    if (!argc) return NULL;

    if (vel->cb[VEL_CB_READ]) {
        vel_read_cb_t proc = (vel_read_cb_t)vel->cb[VEL_CB_READ];
        buf = proc(vel, vel_str(argv[0]));
    } else {
        buf = read_file(vel_str(argv[0]));
    }
    if (!buf) return NULL;
    r = vel_val_str(buf);
    free(buf);
    return r;
}

static VELCB vel_val_t cmd_store(vel_t vel, size_t argc, vel_val_t *argv)
{
    const char *data;
    FILE       *f;
    if (argc < 2) return NULL;
    data = vel_str(argv[1]);

    if (vel->cb[VEL_CB_STORE]) {
        vel_store_cb_t proc = (vel_store_cb_t)vel->cb[VEL_CB_STORE];
        proc(vel, vel_str(argv[0]), data);
    } else {
        size_t dlen = strlen(data);
        f = fopen(vel_str(argv[0]), "wb");
        if (!f) {
            char msg[256];
            snprintf(msg, sizeof(msg), "store: cannot open '%s': %s",
                     vel_str(argv[0]), strerror(errno));
            vel_error_set(vel, msg);
            return NULL;
        }
        /* FIX: check fwrite return value */
        if (dlen && fwrite(data, 1, dlen, f) != dlen) {
            char msg[256];
            snprintf(msg, sizeof(msg), "store: write error on '%s': %s",
                     vel_str(argv[0]), strerror(errno));
            fclose(f);
            vel_error_set(vel, msg);
            return NULL;
        }
        fclose(f);
    }
    return vel_val_clone(argv[1]);
}

static VELCB vel_val_t cmd_source(vel_t vel, size_t argc, vel_val_t *argv)
{
    char     *buf;
    vel_val_t r;
    if (!argc) return NULL;

    if (vel->cb[VEL_CB_SOURCE]) {
        buf = ((vel_source_cb_t)vel->cb[VEL_CB_SOURCE])(vel, vel_str(argv[0]));
    } else if (vel->cb[VEL_CB_READ]) {
        buf = ((vel_read_cb_t)vel->cb[VEL_CB_READ])(vel, vel_str(argv[0]));
    } else {
        buf = read_file(vel_str(argv[0]));
    }
    if (!buf) return NULL;
    r = vel_parse(vel, buf, 0, 0);
    free(buf);
    return r;
}

/* ============================================================
 * Miscellaneous
 * ========================================================= */

static VELCB vel_val_t cmd_try(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_val_t r;
    int       saved_stop;
    if (!argc || vel->err_code) return NULL;

    saved_stop     = vel->env->stop;
    vel->env->stop = 0;

    r = vel_parse_val(vel, argv[0], 0);

    if (vel->env->retval_set) {
        vel_val_free(r);
        r                    = vel->env->retval;
        vel->env->retval     = NULL;
        vel->env->retval_set = 0;
    }

    if (vel->err_code) {
        /* clear error so execution can continue after try */
        vel->err_code = VEL_ERR_NONE;
        free(vel->err_msg);
        vel->err_msg  = NULL;
        vel->err_pos  = 0;
        /* FIX: restore saved_stop before running the catch block */
        vel->env->stop = saved_stop;
        vel_val_free(r);
        r = argc > 1 ? vel_parse_val(vel, argv[1], 0) : NULL;
    } else {
        vel->env->stop = saved_stop;
    }
    return r;
}

static VELCB vel_val_t cmd_error(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_error_set(vel, argc ? vel_str(argv[0]) : NULL);
    return NULL;
}

static VELCB vel_val_t cmd_exit(vel_t vel, size_t argc, vel_val_t *argv)
{
    if (vel->cb[VEL_CB_EXIT]) {
        vel_exit_cb_t proc = (vel_exit_cb_t)vel->cb[VEL_CB_EXIT];
        proc(vel, argc ? argv[0] : NULL);
    }
    return NULL;
}

static VELCB vel_val_t cmd_catcher(vel_t vel, size_t argc, vel_val_t *argv)
{
    if (!argc) return vel_val_str(vel->catcher ? vel->catcher : "");
    {
        const char *s = vel_str(argv[0]);
        free(vel->catcher);
        vel->catcher = s[0] ? vel_strdup(s) : NULL;
    }
    return NULL;
}

static VELCB vel_val_t cmd_watch(vel_t vel, size_t argc, vel_val_t *argv)
{
    const char *wcode;
    size_t      i;
    if (argc < 2) return NULL;
    wcode = vel_str(argv[argc - 1]);
    for (i = 0; i + 1 < argc; i++) {
        const char *vname = vel_str(argv[i]);
        vel_var_t   var;
        if (!vname[0]) continue;
        var = mem_find_var(vel, vel->env, vname);
        if (!var) var = vel_var_set(vel, vname, NULL, VEL_VAR_LOCAL_NEW);
        free(var->watch);
        var->watch = wcode[0] ? vel_strdup(wcode) : NULL;
    }
    return NULL;
}

/* ============================================================
 * Registration
 * ========================================================= */

void register_builtins(vel_t vel)
{
    /* Reflection */
    vel_register(vel, "reflect",    cmd_reflect);
    /* Functions */
    vel_register(vel, "func",       cmd_func);
    vel_register(vel, "rename",     cmd_rename);
    vel_register(vel, "unusedname", cmd_unusedname);
    /* Variables */
    vel_register(vel, "quote",      cmd_quote);
    vel_register(vel, "set",        cmd_set);
    vel_register(vel, "local",      cmd_local);
    /* Output */
    vel_register(vel, "write",      cmd_write);
    vel_register(vel, "print",      cmd_print);
    /* Eval */
    vel_register(vel, "eval",       cmd_eval);
    vel_register(vel, "topeval",    cmd_topeval);
    vel_register(vel, "upeval",     cmd_upeval);
    vel_register(vel, "enveval",    cmd_enveval);
    vel_register(vel, "jaileval",   cmd_jaileval);
    /* Control flow */
    vel_register(vel, "return",     cmd_return);
    vel_register(vel, "result",     cmd_result);
    vel_register(vel, "break",      cmd_break);
    vel_register(vel, "continue",   cmd_continue);
    vel_register(vel, "not",        cmd_not);
    vel_register(vel, "if",         cmd_if);
    vel_register(vel, "while",      cmd_while);
    vel_register(vel, "for",        cmd_for);
    /* Lists */
    vel_register(vel, "count",      cmd_count);
    vel_register(vel, "index",      cmd_index);
    vel_register(vel, "indexof",    cmd_indexof);
    vel_register(vel, "append",     cmd_append);
    vel_register(vel, "slice",      cmd_slice);
    vel_register(vel, "filter",     cmd_filter);
    vel_register(vel, "list",       cmd_list);
    vel_register(vel, "subst",      cmd_subst);
    vel_register(vel, "concat",     cmd_concat);
    vel_register(vel, "foreach",    cmd_foreach);
    vel_register(vel, "lmap",       cmd_lmap);
    vel_register(vel, "join",       cmd_join);
    /* Math */
    vel_register(vel, "expr",       cmd_expr);
    vel_register(vel, "inc",        cmd_inc);
    vel_register(vel, "dec",        cmd_dec);
    vel_register(vel, "rand",       cmd_rand);
    vel_register(vel, "sleep",      cmd_sleep);
    /* Strings */
    vel_register(vel, "char",       cmd_char);
    vel_register(vel, "charat",     cmd_charat);
    vel_register(vel, "codeat",     cmd_codeat);
    vel_register(vel, "substr",     cmd_substr);
    vel_register(vel, "strpos",     cmd_strpos);
    vel_register(vel, "length",     cmd_length);
    vel_register(vel, "trim",       cmd_trim);
    vel_register(vel, "ltrim",      cmd_ltrim);
    vel_register(vel, "rtrim",      cmd_rtrim);
    vel_register(vel, "strcmp",     cmd_strcmp);
    vel_register(vel, "streq",      cmd_streq);
    vel_register(vel, "repstr",     cmd_repstr);
    vel_register(vel, "split",      cmd_split);
    vel_register(vel, "toupper",    cmd_toupper);
    vel_register(vel, "tolower",    cmd_tolower);
    vel_register(vel, "startswith", cmd_startswith);
    vel_register(vel, "endswith",   cmd_endswith);
    /* I/O */
    vel_register(vel, "read",       cmd_read);
    vel_register(vel, "store",      cmd_store);
    vel_register(vel, "source",     cmd_source);
    /* Misc */
    vel_register(vel, "try",        cmd_try);
    vel_register(vel, "error",      cmd_error);
    vel_register(vel, "exit",       cmd_exit);
    vel_register(vel, "catcher",    cmd_catcher);
    vel_register(vel, "watch",      cmd_watch);

    vel->fn_sys = vel->fn_count;

    /* system/shell extensions (vel_sys.c) */
    register_sys_builtins(vel);

    /* job control dan pipeline (vel_jobs.c) */
    register_job_builtins(vel);

    /* extra shell commands (vel_extra.c) */
    register_extra_builtins(vel);
}
