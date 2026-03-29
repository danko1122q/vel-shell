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
 * vel_mem.c  --  allocation and lifecycle for all vel objects:
 *               values, lists, environments, variables, and functions
 *
 * GC: Reference-counted string data (hidden-prefix approach).
 *
 *   Each string data buffer has a hidden size_t refcount stored
 *   immediately BEFORE the char* pointer stored in v->data:
 *
 *     heap layout:  [ size_t refcount | char data[len+1] ]
 *                                      ^
 *                                      v->data points here
 *
 *   This means ALL existing code that reads v->data is unchanged.
 *   Only allocation / free / clone / mutation functions change.
 *
 *   vel_val_clone(src) is now O(1): it shares the data buffer and
 *   increments the refcount instead of doing a full memcpy.
 *
 *   vel_val_cat_* perform Copy-on-Write: if the data buffer is shared
 *   (refcount > 1) they make a private copy before mutating.
 *
 *   vel_val_free decrements the refcount and frees the buffer only
 *   when it reaches zero.
 */

#include "vel_priv.h"

/* ============================================================
 * GC: hidden-prefix refcount helpers
 * ========================================================= */

/* Pointer to the refcount word hidden before a data pointer.
 * data MUST NOT be NULL when this macro is used. */
#define GC_RC(data)  (*(size_t *)((char *)(data) - sizeof(size_t)))

/* Allocate a fresh data buffer of `len+1` bytes with refcount=1.
 * Returns a pointer to the string area (refcount is hidden before it).
 * Returns NULL on OOM. */
static char *gc_data_alloc(size_t len)
{
    char *base = malloc(sizeof(size_t) + len + 1);
    if (!base) return NULL;
    *(size_t *)base = 1;               /* refcount = 1 */
    return base + sizeof(size_t);     /* caller sees only the string area */
}

/* Realloc a data buffer to hold new_len chars (+ NUL).
 * data may be NULL (equivalent to a fresh alloc with refcount=1).
 * The caller MUST have already called gc_cow() to ensure refcount==1
 * before calling this function.
 * Returns new string-area pointer, or NULL on OOM. */
static char *gc_data_realloc(char *data, size_t new_len)
{
    char *base    = data ? (char *)data - sizeof(size_t) : NULL;
    char *newbase = realloc(base, sizeof(size_t) + new_len + 1);
    if (!newbase) return NULL;
    if (!base) *(size_t *)newbase = 1; /* fresh alloc: init refcount */
    return newbase + sizeof(size_t);
}

/* Decrement refcount for a data buffer; free when it reaches zero.
 * Safe to call with data == NULL. */
static void gc_data_release(char *data)
{
    if (!data) return;
    if (--GC_RC(data) == 0)
        free((char *)data - sizeof(size_t));
}

/* Copy-on-Write: ensure v->data is exclusively owned (refcount == 1)
 * before a mutation.  If the data is shared, a private copy is made.
 * Returns 1 on success, 0 on OOM (v is left unmodified). */
static int gc_cow(vel_val_t v)
{
    char *newdata;
    if (!v->data || GC_RC(v->data) <= 1) return 1;

    newdata = gc_data_alloc(v->len);
    if (!newdata) return 0;
    memcpy(newdata, v->data, v->len + 1);
    gc_data_release(v->data);   /* release our share of the old buffer */
    v->data = newdata;
    return 1;
}

/* ============================================================
 * Utility
 * ========================================================= */

char *vel_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char  *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* ============================================================
 * Value
 * ========================================================= */

vel_val_t val_make_len(const char *s, size_t len)
{
    vel_val_t v = calloc(1, sizeof(struct vel_value_s));
    if (!v) return NULL;   /* true OOM: nothing we can do */
    if (s && len) {
        v->data = gc_data_alloc(len);
        if (!v->data) {
            /* BUG 4 FIX: gc_data_alloc failed but struct is valid.
             * Return an empty value (len=0, data=NULL) rather than NULL,
             * so downstream callers that don't check for NULL still
             * receive a usable vel_val_t instead of crashing. */
            return v;
        }
        memcpy(v->data, s, len);
        v->data[len] = '\0';
        v->len = len;
    }
    return v;
}

vel_val_t val_make(const char *s)
{
    return val_make_len(s, s ? strlen(s) : 0);
}

vel_val_t vel_val_str(const char *s)    { return val_make(s); }

vel_val_t vel_val_int(vel_int_t n)
{
    char buf[64];
    sprintf(buf, VEL_INT_FMT, n);
    return val_make(buf);
}

vel_val_t vel_val_dbl(double n)
{
    char buf[64];
    sprintf(buf, "%f", n);
    return val_make(buf);
}

/*
 * vel_val_clone: O(1) shallow clone — shares the string data buffer.
 * The data buffer's refcount is incremented; both the original and the
 * clone must be freed independently with vel_val_free.
 */
vel_val_t vel_val_clone(vel_val_t src)
{
    vel_val_t v;
    if (!src) return NULL;
    v = calloc(1, sizeof(struct vel_value_s));
    if (!v) return NULL;
    v->data = src->data;
    v->len  = src->len;
    if (v->data) GC_RC(v->data)++;   /* share the buffer */
    return v;
}

/*
 * vel_val_free: decrement data refcount; free struct + buffer when RC hits 0.
 */
void vel_val_free(vel_val_t v)
{
    if (!v) return;
    gc_data_release(v->data);
    free(v);
}

int vel_val_cat_ch(vel_val_t v, char ch)
{
    char *nw;
    if (!gc_cow(v)) return 0;
    nw = gc_data_realloc(v->data, v->len + 1);
    if (!nw) return 0;
    nw[v->len++] = ch;
    nw[v->len]   = '\0';
    v->data = nw;
    return 1;
}

int vel_val_cat_str_len(vel_val_t v, const char *s, size_t len)
{
    char *nw;
    if (!s || !len) return 1;
    if (!gc_cow(v)) return 0;
    nw = gc_data_realloc(v->data, v->len + len);
    if (!nw) return 0;
    memcpy(nw + v->len, s, len);   /* GC BUG 1 FIX: copy exactly len bytes.
                                    * Old code: memcpy(..., len + 1) assumed
                                    * s[len] == '\0', which is FALSE for raw
                                    * substring inputs (e.g. heredoc lines from
                                    * vel->src).  Explicitly set the NUL here. */
    nw[v->len + len] = '\0';
    v->len += len;
    v->data = nw;
    return 1;
}

int vel_val_cat_str(vel_val_t v, const char *s)
{
    return vel_val_cat_str_len(v, s, strlen(s));
}

int vel_val_cat(vel_val_t v, vel_val_t other)
{
    if (!other || !other->len) return 1;
    return vel_val_cat_str_len(v, other->data, other->len);
}

const char *vel_str(vel_val_t v)
{
    return (v && v->len) ? v->data : "";
}

double    vel_dbl(vel_val_t v)  { return atof(vel_str(v)); }
vel_int_t vel_int(vel_val_t v)  { return (vel_int_t)atoll(vel_str(v)); }

int vel_bool(vel_val_t v)
{
    const char *s = vel_str(v);
    size_t i;
    int has_dot = 0;
    if (!s[0]) return 0;
    for (i = 0; s[i]; i++) {
        if (s[i] == '.') {
            if (has_dot) return 1;
            has_dot = 1;
        } else if (s[i] != '0') {
            return 1;
        }
    }
    return 0;
}

/* ============================================================
 * List
 * ========================================================= */

vel_list_t vel_list_new(void)
{
    return calloc(1, sizeof(struct vel_list_s));
}

void vel_list_free(vel_list_t list)
{
    size_t i;
    if (!list) return;
    for (i = 0; i < list->count; i++)
        vel_val_free(list->items[i]);
    free(list->items);
    free(list);
}

void vel_list_push(vel_list_t list, vel_val_t val)
{
    if (list->count == list->cap) {
        size_t    cap  = list->cap ? list->cap + list->cap / 2 : 16;
        vel_val_t *nw  = realloc(list->items, sizeof(vel_val_t) * cap);
        if (!nw) return;
        list->items = nw;
        list->cap   = cap;
    }
    list->items[list->count++] = val;
}

size_t    vel_list_len(vel_list_t list)           { return list->count; }
vel_val_t vel_list_get(vel_list_t list, size_t i) { return i < list->count ? list->items[i] : NULL; }

vel_val_t vel_list_pack(vel_list_t list, int escape)
{
    vel_val_t out = val_make(NULL);
    size_t i, j;

    if (!out) return NULL;

    for (i = 0; i < list->count; i++) {
        const char *s   = vel_str(list->items[i]);
        size_t      len = list->items[i] ? list->items[i]->len : 0;
        int has_brace   = 0;
        int needs_quote = 0;

        if (escape) {
            for (j = 0; j < len; j++) {
                char c = s[j];
                if (c == '{' || c == '}') { has_brace = 1; needs_quote = 1; break; }
                if (ispunct((unsigned char)c) || isspace((unsigned char)c))
                    needs_quote = 1;
            }
        }

        if (i) vel_val_cat_ch(out, ' ');

        if (!escape || !needs_quote) {
            vel_val_cat(out, list->items[i]);
        } else if (has_brace) {
            vel_val_cat_ch(out, '"');
            for (j = 0; j < len; j++) {
                char c = s[j];
                if (c == '"' || c == '\\') vel_val_cat_ch(out, '\\');
                vel_val_cat_ch(out, c);
            }
            vel_val_cat_ch(out, '"');
        } else {
            vel_val_cat_ch(out, '{');
            vel_val_cat(out, list->items[i]);
            vel_val_cat_ch(out, '}');
        }
    }
    return out;
}

/* ============================================================
 * Environment
 * ========================================================= */

vel_env_t vel_env_new(vel_env_t parent)
{
    vel_env_t env = calloc(1, sizeof(struct vel_env_s));
    if (env) env->parent = parent;
    return env;
}

void vel_env_free(vel_env_t env)
{
    size_t i;
    if (!env) return;
    vel_val_free(env->retval);
    vmap_free(&env->varmap);
    for (i = 0; i < env->var_count; i++) {
        free(env->vars[i]->name);
        free(env->vars[i]->watch);
        vel_val_free(env->vars[i]->value);
        free(env->vars[i]);
    }
    free(env->vars);
    free(env);
}

vel_env_t vel_env_push(vel_t vel)
{
    vel_env_t env = vel_env_new(vel->env);
    vel->env = env;
    return env;
}

void vel_env_pop(vel_t vel)
{
    if (vel->env->parent) {
        vel_env_t up = vel->env->parent;
        vel_env_free(vel->env);
        vel->env = up;
    } else {
#ifdef NDEBUG
        fprintf(stderr,
                "vel: internal warning: vel_env_pop() at root env (unbalanced push/pop)\n");
#else
        fprintf(stderr,
                "vel: FATAL: vel_env_pop() at root env (unbalanced push/pop) -- aborting\n");
        abort();
#endif
    }
}

/* ============================================================
 * Variable lookup and assignment
 * ========================================================= */

vel_var_t mem_find_local(vel_t vel, vel_env_t env, const char *name)
{
    (void)vel;
    return vmap_get(&env->varmap, name);
}

vel_var_t mem_find_var(vel_t vel, vel_env_t env, const char *name)
{
    vel_env_t cur = env;
    (void)vel;
    while (cur) {
        vel_var_t v = mem_find_local(vel, cur, name);
        if (v) return v;
        cur = cur->parent;
    }
    return NULL;
}

vel_var_t vel_var_set(vel_t vel, const char *name, vel_val_t val, int mode)
{
    vel_env_t   env     = (mode == VEL_VAR_GLOBAL) ? vel->root_env : vel->env;
    vel_var_t  *slot;
    int         freeval = 0;

    if (!name || !name[0]) return NULL;

    if (mode != VEL_VAR_LOCAL_NEW) {
        vel_var_t var = mem_find_var(vel, env, name);

        if (mode == VEL_VAR_LOCAL_ONLY && var
                && var->env == vel->root_env && var->env != env)
            var = NULL;

        if (((!var && env == vel->root_env) || (var && var->env == vel->root_env))
                && vel->cb[VEL_CB_SETVAR]) {
            vel_setvar_cb_t proc = (vel_setvar_cb_t)vel->cb[VEL_CB_SETVAR];
            vel_val_t newval = val;
            int r = proc(vel, name, &newval);
            if (r < 0) return NULL;
            if (r) { val = newval; freeval = 1; }
        }

        if (var) {
            vel_val_free(var->value);
            var->value = freeval ? val : vel_val_clone(val);
            if (var->watch) {
                vel_env_t saved = vel->env;
                vel->env = var->env;
                vel_val_free(vel_parse(vel, var->watch, 0, 1));
                vel->env = saved;
            }
            return var;
        }
    }

    slot = realloc(env->vars, sizeof(vel_var_t) * (env->var_count + 1));
    if (!slot) return NULL;
    env->vars = slot;

    slot[env->var_count] = calloc(1, sizeof(struct vel_var_s));
    if (!slot[env->var_count]) return NULL;
    slot[env->var_count]->name  = vel_strdup(name);
    slot[env->var_count]->watch = NULL;
    slot[env->var_count]->env   = env;
    slot[env->var_count]->value = freeval ? val : vel_val_clone(val);

    vmap_set(&env->varmap, name, slot[env->var_count]);
    return slot[env->var_count++];
}

vel_val_t vel_var_get(vel_t vel, const char *name)
{
    /* GC BUG 2 FIX: always return an owned clone, never a borrowed pointer.
     *
     * Old code: return vel_var_get_or(vel, name, vel->empty)
     *   This returned vel->empty directly when the variable was not found.
     *   Callers that call vel_val_free() on the result would free the
     *   vel->empty struct itself, leaving vel->empty as a dangling pointer.
     *   A second lookup of a missing variable (or vel_free()) would then
     *   trigger a heap-use-after-free (confirmed with ASan).
     *
     * Fix: clone the result before returning.  With refcount GC this is O(1):
     *   - existing var:  RC goes 1 → 2; caller free() brings it back to 1
     *   - vel->empty (data=NULL): allocates a fresh empty struct, no RC bump
     * The caller now owns the returned value and MUST vel_val_free() it. */
    vel_val_t v = vel_var_get_or(vel, name, vel->empty);
    return vel_val_clone(v);
}

vel_val_t vel_var_get_or(vel_t vel, const char *name, vel_val_t def)
{
    vel_var_t  var    = mem_find_var(vel, vel->env, name);
    vel_val_t  result = var ? var->value : def;

    if (vel->cb[VEL_CB_GETVAR] && (!var || var->env == vel->root_env)) {
        vel_getvar_cb_t proc = (vel_getvar_cb_t)vel->cb[VEL_CB_GETVAR];
        vel_val_t       got  = result;
        if (proc(vel, name, &got))
            result = got;
    }
    return result;
}

/* ============================================================
 * Function registry
 * ========================================================= */

vel_fn_t mem_find_fn(vel_t vel, const char *name)
{
    return vmap_get(&vel->fnmap, name);
}

vel_fn_t mem_add_fn(vel_t vel, const char *name)
{
    vel_fn_t  fn  = mem_find_fn(vel, name);
    vel_fn_t *arr;

    if (fn) {
        if (fn->params) vel_list_free(fn->params);
        vel_val_free(fn->body);
        fn->params = NULL;
        fn->body   = NULL;
        fn->native = NULL;
        return fn;
    }

    fn  = calloc(1, sizeof(struct vel_func_s));
    if (!fn) return NULL;
    fn->name = vel_strdup(name);

    arr = realloc(vel->fn, sizeof(vel_fn_t) * (vel->fn_count + 1));
    if (!arr) { free(fn->name); free(fn); return NULL; }
    vel->fn = arr;
    arr[vel->fn_count++] = fn;

    vmap_set(&vel->fnmap, name, fn);
    return fn;
}

void mem_del_fn(vel_t vel, vel_fn_t fn)
{
    size_t i, idx = vel->fn_count;

    for (i = 0; i < vel->fn_count; i++)
        if (vel->fn[i] == fn) { idx = i; break; }
    if (idx == vel->fn_count) return;

    vmap_set(&vel->fnmap, fn->name, NULL);
    if (fn->params) vel_list_free(fn->params);
    vel_val_free(fn->body);
    free(fn->name);
    free(fn);

    vel->fn_count--;
    for (i = idx; i < vel->fn_count; i++)
        vel->fn[i] = vel->fn[i + 1];
}

int vel_register(vel_t vel, const char *name, vel_fn_proc_t proc)
{
    vel_fn_t fn = mem_add_fn(vel, name);
    if (!fn) return 0;
    fn->native = proc;
    return 1;
}

/* ============================================================
 * Misc public helpers
 * ========================================================= */

vel_val_t vel_arg(vel_val_t *argv, size_t idx) { return argv ? argv[idx] : NULL; }
void      vel_set_data(vel_t vel, void *data)  { vel->data = data; }
void     *vel_get_data(vel_t vel)              { return vel->data; }
void      vel_freemem(void *ptr)               { free(ptr); }
