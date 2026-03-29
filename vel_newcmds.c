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
 * vel_newcmds.c  --  additional built-in commands
 *
 * List utilities  : lreverse  lsort  luniq  lassign (+ splat *rest)
 * Math helpers    : abs  max  min  math (sin/cos/sqrt/pow/floor/ceil/round...)
 * String extras   : string (repeat|reverse|is|map)
 * Clock           : clock (s|ms|us|ns)
 * Dict (flat list): dict set/get/exists/unset/keys/values/size/for
 * Base64          : base64 encode/decode
 * upvar           : upvar localname parentname
 * Reflection ext  : reflect level / reflect depth
 * try...finally   : try body ?err catch? ?finally fin?
 */

#include "vel_priv.h"
#include <math.h>
#include <time.h>
#include <errno.h>
#include <stdint.h>

/* helper: return int if result has no fractional part, else float */
static vel_val_t smart_num(double d)
{
    return (fmod(d, 1.0) == 0.0) ? vel_val_int((vel_int_t)d) : vel_val_dbl(d);
}

/* ============================================================
 * List utilities
 * ========================================================= */

static VELCB vel_val_t cmd_lreverse(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_list_t in, out;
    vel_val_t  r;
    size_t     i, n;

    if (!argc) return NULL;
    in  = vel_subst_list(vel, argv[0]);
    out = vel_list_new();
    n   = vel_list_len(in);
    for (i = n; i > 0; i--)
        vel_list_push(out, vel_val_clone(vel_list_get(in, i - 1)));
    vel_list_free(in);
    r = vel_list_pack(out, 1);
    vel_list_free(out);
    return r;
}

static int cmp_val_asc(const void *a, const void *b)
{
    return strcmp(vel_str(*(const vel_val_t *)a),
                  vel_str(*(const vel_val_t *)b));
}

static VELCB vel_val_t cmd_lsort(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_list_t list;
    vel_val_t  r;

    if (!argc) return NULL;
    list = vel_subst_list(vel, argv[0]);
    if (list->count > 1)
        qsort(list->items, list->count, sizeof(vel_val_t), cmp_val_asc);
    r = vel_list_pack(list, 1);
    vel_list_free(list);
    return r;
}

static VELCB vel_val_t cmd_luniq(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_list_t  in, out;
    vel_val_t   r;
    size_t      i;
    char       *prev_copy = NULL;
    const char *prev      = NULL;

    if (!argc) return NULL;
    in  = vel_subst_list(vel, argv[0]);
    out = vel_list_new();
    for (i = 0; i < vel_list_len(in); i++) {
        const char *s = vel_str(vel_list_get(in, i));
        if (!prev || strcmp(s, prev) != 0) {
            vel_list_push(out, vel_val_clone(vel_list_get(in, i)));
            free(prev_copy);
            prev_copy = vel_strdup(s);
            prev      = prev_copy;
        }
    }
    free(prev_copy);
    vel_list_free(in);
    r = vel_list_pack(out, 1);
    vel_list_free(out);
    return r;
}

/*
 * lassign list var1 var2 ... ?*rest?
 * Splat: if last var starts with '*', it receives all remaining elements.
 * Example:
 *   lassign {a b c d} x y *rest   -> x=a  y=b  rest={c d}
 */
static VELCB vel_val_t cmd_lassign(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_list_t list;
    size_t     i, n, nvar;

    if (argc < 2) return NULL;
    list = vel_subst_list(vel, argv[0]);
    n    = vel_list_len(list);
    nvar = argc - 1;

    for (i = 0; i < nvar; i++) {
        const char *vname = vel_str(argv[i + 1]);
        vel_val_t   v;

        /* splat: *rest collects remaining elements */
        if (vname[0] == '*' && i == nvar - 1) {
            vel_list_t rest = vel_list_new();
            size_t     j;
            for (j = i; j < n; j++)
                vel_list_push(rest, vel_val_clone(vel_list_get(list, j)));
            v = vel_list_pack(rest, 1);
            vel_list_free(rest);
            vel_var_set(vel, vname + 1, v, VEL_VAR_LOCAL);
            vel_val_free(v);
            break;
        }

        v = (i < n) ? vel_val_clone(vel_list_get(list, i)) : val_make(NULL);
        vel_var_set(vel, vname, v, VEL_VAR_LOCAL);
        vel_val_free(v);
    }

    vel_list_free(list);
    return NULL;
}

/* ============================================================
 * Math helpers
 * ========================================================= */

static VELCB vel_val_t cmd_abs(vel_t vel, size_t argc, vel_val_t *argv)
{
    double d;
    (void)vel;
    if (!argc) return NULL;
    d = vel_dbl(argv[0]);
    return smart_num(d < 0.0 ? -d : d);
}

static VELCB vel_val_t cmd_max(vel_t vel, size_t argc, vel_val_t *argv)
{
    double a, b;
    (void)vel;
    if (argc < 2) return argc ? vel_val_clone(argv[0]) : NULL;
    a = vel_dbl(argv[0]); b = vel_dbl(argv[1]);
    return smart_num(a > b ? a : b);
}

static VELCB vel_val_t cmd_min(vel_t vel, size_t argc, vel_val_t *argv)
{
    double a, b;
    (void)vel;
    if (argc < 2) return argc ? vel_val_clone(argv[0]) : NULL;
    a = vel_dbl(argv[0]); b = vel_dbl(argv[1]);
    return smart_num(a < b ? a : b);
}

static VELCB vel_val_t cmd_math(vel_t vel, size_t argc, vel_val_t *argv)
{
    const char *op;
    double      a = 0.0, b = 0.0, res;

    if (!argc) { vel_error_set(vel, "math: subcommand required"); return NULL; }
    op = vel_str(argv[0]);

    if (!strcmp(op, "pi")) return vel_val_dbl(M_PI);
    if (!strcmp(op, "e"))  return vel_val_dbl(M_E);

    if (argc < 2) {
        char msg[80]; snprintf(msg, sizeof(msg), "math %s: argument required", op);
        vel_error_set(vel, msg); return NULL;
    }
    a = vel_dbl(argv[1]);

    if (!strcmp(op, "pow") || !strcmp(op, "atan2")) {
        if (argc < 3) {
            char msg[80]; snprintf(msg, sizeof(msg), "math %s: two arguments required", op);
            vel_error_set(vel, msg); return NULL;
        }
        b = vel_dbl(argv[2]);
        return smart_num(!strcmp(op, "pow") ? pow(a, b) : atan2(a, b));
    }

    if      (!strcmp(op, "sin"))   res = sin(a);
    else if (!strcmp(op, "cos"))   res = cos(a);
    else if (!strcmp(op, "tan"))   res = tan(a);
    else if (!strcmp(op, "asin"))  res = asin(a);
    else if (!strcmp(op, "acos"))  res = acos(a);
    else if (!strcmp(op, "atan"))  res = atan(a);
    else if (!strcmp(op, "sqrt"))  res = sqrt(a);
    else if (!strcmp(op, "log"))   res = log(a);
    else if (!strcmp(op, "log2"))  res = log2(a);
    else if (!strcmp(op, "log10")) res = log10(a);
    else if (!strcmp(op, "abs"))   res = fabs(a);
    else if (!strcmp(op, "floor")) res = floor(a);
    else if (!strcmp(op, "ceil"))  res = ceil(a);
    else if (!strcmp(op, "round")) res = round(a);
    else {
        char msg[80]; snprintf(msg, sizeof(msg), "math: unknown subcommand '%s'", op);
        vel_error_set(vel, msg); return NULL;
    }
    return smart_num(res);
}

/* ============================================================
 * String extras
 * ========================================================= */

static int str_is_integer(const char *s)
{
    char *end;
    if (!s || !s[0]) return 0;
    errno = 0;
    strtoll(s, &end, 10);
    return errno == 0 && *end == '\0';
}

static int str_is_double(const char *s)
{
    char *end;
    if (!s || !s[0]) return 0;
    errno = 0;
    strtod(s, &end);
    return errno == 0 && *end == '\0';
}

/*
 * string subcommand ...
 *
 * string repeat   str n      -- repeat str n times
 * string reverse  str        -- reverse characters
 * string is       type str   -- type test: integer double alpha alnum space upper lower print ascii
 * string map      pairs str  -- multi-pair substitution; pairs = {old1 new1 old2 new2 ...}
 */
static VELCB vel_val_t cmd_string(vel_t vel, size_t argc, vel_val_t *argv)
{
    const char *op;
    if (!argc) { vel_error_set(vel, "string: subcommand required"); return NULL; }
    op = vel_str(argv[0]);

    /* ---- repeat ---- */
    if (!strcmp(op, "repeat")) {
        const char *s; size_t slen, n, i; vel_val_t r;
        if (argc < 3) return val_make(NULL);
        s = vel_str(argv[1]); slen = strlen(s); n = (size_t)vel_int(argv[2]);
        if (!slen || !n) return val_make(NULL);
        r = val_make(NULL);
        for (i = 0; i < n; i++) vel_val_cat_str_len(r, s, slen);
        return r;
    }

    /* ---- reverse ---- */
    if (!strcmp(op, "reverse")) {
        const char *s; size_t len, i; vel_val_t r;
        if (argc < 2) return val_make(NULL);
        s = vel_str(argv[1]); len = strlen(s); r = val_make(NULL);
        for (i = len; i > 0; i--) vel_val_cat_ch(r, s[i - 1]);
        return r;
    }

    /* ---- is ---- */
    if (!strcmp(op, "is")) {
        const char *type, *s; size_t i, len; int ok = 1;
        if (argc < 3) return NULL;
        type = vel_str(argv[1]); s = vel_str(argv[2]); len = strlen(s);
        if (!len) return NULL;

        if      (!strcmp(type, "integer")) ok = str_is_integer(s);
        else if (!strcmp(type, "double"))  ok = str_is_double(s);
        else if (!strcmp(type, "alpha"))  { for (i=0;i<len&&ok;i++) ok=isalpha((unsigned char)s[i])!=0; }
        else if (!strcmp(type, "alnum"))  { for (i=0;i<len&&ok;i++) ok=isalnum((unsigned char)s[i])!=0; }
        else if (!strcmp(type, "space"))  { for (i=0;i<len&&ok;i++) ok=isspace((unsigned char)s[i])!=0; }
        else if (!strcmp(type, "upper"))  { for (i=0;i<len&&ok;i++) ok=isupper((unsigned char)s[i])!=0; }
        else if (!strcmp(type, "lower"))  { for (i=0;i<len&&ok;i++) ok=islower((unsigned char)s[i])!=0; }
        else if (!strcmp(type, "print"))  { for (i=0;i<len&&ok;i++) ok=isprint((unsigned char)s[i])!=0; }
        else if (!strcmp(type, "ascii"))  { for (i=0;i<len&&ok;i++) ok=((unsigned char)s[i]<128);       }
        else {
            char msg[80]; snprintf(msg, sizeof(msg), "string is: unknown type '%s'", type);
            vel_error_set(vel, msg); return NULL;
        }
        return ok ? vel_val_int(1) : NULL;
    }

    /* ---- map ---- */
    if (!strcmp(op, "map")) {
        vel_list_t pairs; vel_val_t r; const char *src; size_t np, pi, srclen, i;
        if (argc < 3) return argc > 2 ? vel_val_clone(argv[2]) : val_make(NULL);
        pairs  = vel_subst_list(vel, argv[1]);
        np     = vel_list_len(pairs);
        src    = vel_str(argv[2]);
        srclen = strlen(src);

        if (np % 2 != 0) {
            vel_list_free(pairs);
            vel_error_set(vel, "string map: pairs list must have even number of elements");
            return NULL;
        }

        r = val_make(NULL); i = 0;
        while (i < srclen) {
            int matched = 0;
            for (pi = 0; pi + 1 < np; pi += 2) {
                const char *from = vel_str(vel_list_get(pairs, pi));
                size_t      flen = strlen(from);
                if (!flen) continue;
                if (flen <= srclen - i && memcmp(src + i, from, flen) == 0) {
                    vel_val_cat_str(r, vel_str(vel_list_get(pairs, pi + 1)));
                    i += flen; matched = 1; break;
                }
            }
            if (!matched) vel_val_cat_ch(r, src[i++]);
        }
        vel_list_free(pairs);
        return r;
    }

    {
        char msg[80]; snprintf(msg, sizeof(msg), "string: unknown subcommand '%s'", op);
        vel_error_set(vel, msg); return NULL;
    }
}

/* ============================================================
 * High-precision clock
 * ========================================================= */

static VELCB vel_val_t cmd_clock(vel_t vel, size_t argc, vel_val_t *argv)
{
    struct timespec ts;
    const char     *unit;
    (void)vel;
    clock_gettime(CLOCK_REALTIME, &ts);
    unit = (argc > 0) ? vel_str(argv[0]) : "s";
    if (!strcmp(unit, "ms"))
        return vel_val_int((vel_int_t)ts.tv_sec * 1000LL   + (vel_int_t)ts.tv_nsec / 1000000LL);
    if (!strcmp(unit, "us"))
        return vel_val_int((vel_int_t)ts.tv_sec * 1000000LL + (vel_int_t)ts.tv_nsec / 1000LL);
    if (!strcmp(unit, "ns"))
        return vel_val_int((vel_int_t)ts.tv_sec * 1000000000LL + (vel_int_t)ts.tv_nsec);
    return vel_val_dbl((double)ts.tv_sec + (double)ts.tv_nsec / 1.0e9);
}

/* ============================================================
 * Dict — flat key-value list  {k1 v1 k2 v2 ...}
 *
 * dict set    varname key value
 * dict get    dict    key
 * dict exists dict    key
 * dict unset  varname key
 * dict keys   dict
 * dict values dict
 * dict size   dict
 * dict for    {kvar vvar} dict body
 * ========================================================= */

static size_t dict_find(vel_list_t list, const char *key)
{
    size_t i;
    for (i = 0; i + 1 < vel_list_len(list); i += 2)
        if (!strcmp(key, vel_str(vel_list_get(list, i))))
            return i;
    return (size_t)-1;
}

static VELCB vel_val_t cmd_dict(vel_t vel, size_t argc, vel_val_t *argv)
{
    const char *op;
    if (!argc) { vel_error_set(vel, "dict: subcommand required"); return NULL; }
    op = vel_str(argv[0]);

    /* set */
    if (!strcmp(op, "set")) {
        vel_list_t list; vel_val_t r; size_t idx; const char *vname, *key;
        if (argc < 4) { vel_error_set(vel, "dict set: varname key value"); return NULL; }
        vname = vel_str(argv[1]); key = vel_str(argv[2]);
        list  = vel_subst_list(vel, vel_var_get(vel, vname));
        idx   = dict_find(list, key);
        if (idx != (size_t)-1) {
            vel_val_free(list->items[idx + 1]);
            list->items[idx + 1] = vel_val_clone(argv[3]);
        } else {
            vel_list_push(list, vel_val_clone(argv[2]));
            vel_list_push(list, vel_val_clone(argv[3]));
        }
        r = vel_list_pack(list, 1);
        vel_list_free(list);
        vel_var_set(vel, vname, r, VEL_VAR_LOCAL);
        return r;
    }

    /* get */
    if (!strcmp(op, "get")) {
        vel_list_t list; vel_val_t r = NULL; size_t idx;
        if (argc < 3) return NULL;
        list = vel_subst_list(vel, argv[1]);
        idx  = dict_find(list, vel_str(argv[2]));
        if (idx != (size_t)-1) r = vel_val_clone(vel_list_get(list, idx + 1));
        vel_list_free(list);
        return r;
    }

    /* exists */
    if (!strcmp(op, "exists")) {
        vel_list_t list; size_t idx; int found;
        if (argc < 3) return NULL;
        list  = vel_subst_list(vel, argv[1]);
        idx   = dict_find(list, vel_str(argv[2]));
        found = (idx != (size_t)-1);
        vel_list_free(list);
        return found ? vel_val_int(1) : NULL;
    }

    /* unset */
    if (!strcmp(op, "unset")) {
        vel_list_t list, out; vel_val_t r; size_t idx, i; const char *vname, *key;
        if (argc < 3) { vel_error_set(vel, "dict unset: varname key"); return NULL; }
        vname = vel_str(argv[1]); key = vel_str(argv[2]);
        list  = vel_subst_list(vel, vel_var_get(vel, vname));
        idx   = dict_find(list, key);
        if (idx == (size_t)-1) { r = vel_list_pack(list, 1); vel_list_free(list); return r; }
        out = vel_list_new();
        for (i = 0; i < vel_list_len(list); i++) {
            if (i == idx || i == idx + 1) continue;
            vel_list_push(out, vel_val_clone(vel_list_get(list, i)));
        }
        vel_list_free(list);
        r = vel_list_pack(out, 1);
        vel_list_free(out);
        vel_var_set(vel, vname, r, VEL_VAR_LOCAL);
        return r;
    }

    /* keys */
    if (!strcmp(op, "keys")) {
        vel_list_t list, out; vel_val_t r; size_t i;
        if (argc < 2) return NULL;
        list = vel_subst_list(vel, argv[1]); out = vel_list_new();
        for (i = 0; i + 1 < vel_list_len(list); i += 2)
            vel_list_push(out, vel_val_clone(vel_list_get(list, i)));
        vel_list_free(list); r = vel_list_pack(out, 1); vel_list_free(out); return r;
    }

    /* values */
    if (!strcmp(op, "values")) {
        vel_list_t list, out; vel_val_t r; size_t i;
        if (argc < 2) return NULL;
        list = vel_subst_list(vel, argv[1]); out = vel_list_new();
        for (i = 1; i < vel_list_len(list); i += 2)
            vel_list_push(out, vel_val_clone(vel_list_get(list, i)));
        vel_list_free(list); r = vel_list_pack(out, 1); vel_list_free(out); return r;
    }

    /* size */
    if (!strcmp(op, "size")) {
        vel_list_t list; size_t n;
        if (argc < 2) return vel_val_int(0);
        list = vel_subst_list(vel, argv[1]); n = vel_list_len(list) / 2;
        vel_list_free(list); return vel_val_int((vel_int_t)n);
    }

    /* for */
    if (!strcmp(op, "for")) {
        vel_list_t vars, list; const char *kvar, *vvar; vel_val_t r = NULL; size_t i;
        if (argc < 4) { vel_error_set(vel, "dict for: {kvar vvar} dict body"); return NULL; }
        vars = vel_subst_list(vel, argv[1]);
        if (vel_list_len(vars) < 2) { vel_list_free(vars); vel_error_set(vel, "dict for: need {keyvar valvar}"); return NULL; }
        kvar = vel_str(vel_list_get(vars, 0));
        vvar = vel_str(vel_list_get(vars, 1));
        list = vel_subst_list(vel, argv[2]);

        vel_env_push(vel);
        for (i = 0; i + 1 < vel_list_len(list) && !vel->env->stop && !vel->err_code; i += 2) {
            vel_var_set(vel, kvar, vel_list_get(list, i),     VEL_VAR_LOCAL_NEW);
            vel_var_set(vel, vvar, vel_list_get(list, i + 1), VEL_VAR_LOCAL_NEW);
            vel_val_free(r);
            r = vel_parse_val(vel, argv[3], 0);
            if (vel->env->stop == 2 && !vel->env->retval_set) vel->env->stop = 0;
        }
        {
            int stop_sig = vel->env->stop, ret_set = vel->env->retval_set;
            vel_val_t retv = ret_set ? vel_val_clone(vel->env->retval) : NULL;
            vel_env_pop(vel);
            if (stop_sig && !ret_set) vel->env->stop = 0;
            else if (ret_set) {
                vel_val_free(vel->env->retval);
                vel->env->retval = retv; vel->env->retval_set = 1; vel->env->stop = stop_sig;
            } else { vel_val_free(retv); }
        }
        vel_list_free(vars); vel_list_free(list);
        return r;
    }

    {
        char msg[80]; snprintf(msg, sizeof(msg), "dict: unknown subcommand '%s'", op);
        vel_error_set(vel, msg); return NULL;
    }
}

/* ============================================================
 * Base64 encode / decode  (pure C, no external library)
 * ========================================================= */

static const char b64_tbl[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_dec(char c)
{
    if (c>='A'&&c<='Z') return c-'A';
    if (c>='a'&&c<='z') return c-'a'+26;
    if (c>='0'&&c<='9') return c-'0'+52;
    if (c=='+') return 62;
    if (c=='/') return 63;
    return -1;
}

/*
 * base64 encode str   -- returns base64 string
 * base64 decode str   -- returns decoded string
 */
static VELCB vel_val_t cmd_base64(vel_t vel, size_t argc, vel_val_t *argv)
{
    const char *op;
    if (!argc) { vel_error_set(vel, "base64: subcommand required"); return NULL; }
    op = vel_str(argv[0]);

    if (!strcmp(op, "encode")) {
        const unsigned char *src; size_t srclen, i; vel_val_t r;
        if (argc < 2) return val_make(NULL);
        src = (const unsigned char *)vel_str(argv[1]); srclen = strlen((const char *)src);
        r   = val_make(NULL);
        for (i = 0; i < srclen; i += 3) {
            int b0 = src[i];
            int b1 = (i+1<srclen) ? src[i+1] : 0;
            int b2 = (i+2<srclen) ? src[i+2] : 0;
            vel_val_cat_ch(r, b64_tbl[b0>>2]);
            vel_val_cat_ch(r, b64_tbl[((b0&3)<<4)|(b1>>4)]);
            vel_val_cat_ch(r, (i+1<srclen) ? b64_tbl[((b1&0xF)<<2)|(b2>>6)] : '=');
            vel_val_cat_ch(r, (i+2<srclen) ? b64_tbl[b2&0x3F]               : '=');
        }
        return r;
    }

    if (!strcmp(op, "decode")) {
        const char *src; size_t srclen, i; vel_val_t r;
        if (argc < 2) return val_make(NULL);
        src = vel_str(argv[1]); srclen = strlen(src); r = val_make(NULL);
        for (i = 0; i + 3 < srclen; i += 4) {
            int v0 = b64_dec(src[i]), v1 = b64_dec(src[i+1]);
            int v2 = (src[i+2]=='=') ? 0 : b64_dec(src[i+2]);
            int v3 = (src[i+3]=='=') ? 0 : b64_dec(src[i+3]);
            if (v0<0||v1<0) { vel_error_set(vel,"base64 decode: invalid input"); vel_val_free(r); return NULL; }
            vel_val_cat_ch(r, (char)((v0<<2)|(v1>>4)));
            if (src[i+2]!='=') vel_val_cat_ch(r, (char)(((v1&0xF)<<4)|(v2>>2)));
            if (src[i+3]!='=') vel_val_cat_ch(r, (char)(((v2&3)<<6)|v3));
        }
        return r;
    }

    {
        char msg[80]; snprintf(msg, sizeof(msg), "base64: unknown subcommand '%s'", op);
        vel_error_set(vel, msg); return NULL;
    }
}

/* ============================================================
 * upvar localname parentname
 *
 * Creates a local alias for a variable in the parent scope.
 * Reads the parent's current value into a new local variable, then
 * installs a watch that writes the new value back to the parent
 * env directly (bypassing upeval which can't see the right scope).
 *
 * The watch stores the parent variable name and uses vel->env->parent
 * to write directly — so it works regardless of nesting depth.
 *
 * Usage:
 *   func bump {} {
 *       upvar n counter   ;# n mirrors caller's counter
 *       inc n             ;# counter in caller is updated
 *   }
 * ========================================================= */

static VELCB vel_val_t cmd_upvar(vel_t vel, size_t argc, vel_val_t *argv)
{
    const char *localname, *parentname;
    vel_env_t   parent;
    vel_var_t   pvar, lvar;
    vel_val_t   initval;
    char       *wcode;

    if (argc < 2) { vel_error_set(vel, "upvar: localname parentname"); return NULL; }
    localname  = vel_str(argv[0]);
    parentname = vel_str(argv[1]);

    parent = vel->env->parent ? vel->env->parent : vel->root_env;
    pvar   = mem_find_var(vel, parent, parentname);

    /* seed local with parent's current value (or empty) */
    initval = pvar ? vel_val_clone(pvar->value) : val_make(NULL);
    vel_var_set(vel, localname, initval, VEL_VAR_LOCAL_NEW);
    vel_val_free(initval);

    /*
     * Watch: when the local var changes, write back to parent.
     * Watch code runs with vel->env = local var's env (func scope).
     * 
     * If parentname lives in root_env: use "set global" (always reaches root).
     * If parentname lives in immediate parent frame: use "upeval { set ... }".
     * This covers the two most common upvar patterns.
     */
    lvar = mem_find_local(vel, vel->env, localname);
    if (lvar) {
        int is_global = (parent == vel->root_env);
        wcode = malloc(40 + strlen(localname) + strlen(parentname));
        if (is_global)
            sprintf(wcode, "set global %s $%s", parentname, localname);
        else
            sprintf(wcode, "upeval { set %s $%s }", parentname, localname);
        free(lvar->watch);
        lvar->watch = wcode;
    }
    return NULL;
}

/* ============================================================
 * reflect extension  (adds "level" and "depth" subcommands)
 *
 * Strategy: save the original reflect as "__reflect_orig",
 * then register a wrapper that handles the new subcommands and
 * delegates everything else to __reflect_orig.
 * ========================================================= */
static VELCB vel_val_t cmd_reflect_ext(vel_t vel, size_t argc, vel_val_t *argv)
{
    if (argc && !strcmp(vel_str(argv[0]), "level"))
        return vel_val_int((vel_int_t)vel->stack_depth);
    if (argc && !strcmp(vel_str(argv[0]), "depth"))
        return vel_val_int((vel_int_t)vel->depth);
    return vel_call(vel, "__reflect_orig", argc, argv);
}

/* ============================================================
 * try...finally
 *
 * Replaces the simpler cmd_try from vel_cmd.c.
 *
 * Syntax (all forms):
 *   try body
 *   try body errvar catch_body
 *   try body finally fin_body
 *   try body errvar catch_body finally fin_body
 *
 * Guarantee: fin_body ALWAYS runs — even when body/catch use
 * return, break, or raise an error.
 * ========================================================= */
static VELCB vel_val_t cmd_try_finally(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_val_t body, catch_body, fin_body, r;
    int       saved_stop, had_error;

    if (!argc || vel->err_code) return NULL;

    body       = argv[0];
    catch_body = NULL;
    fin_body   = NULL;

    /* parse optional catch and finally clauses */
    if (argc >= 3 && !strcmp(vel_str(argv[1]), "finally")) {
        fin_body = argv[2];
    } else if (argc >= 3) {
        catch_body = argv[2];
        if (argc >= 5 && !strcmp(vel_str(argv[3]), "finally"))
            fin_body = argv[4];
    }

    saved_stop     = vel->env->stop;
    vel->env->stop = 0;

    /* execute body */
    r = vel_parse_val(vel, body, 0);
    if (vel->env->retval_set) {
        vel_val_free(r);
        r                    = vel->env->retval;
        vel->env->retval     = NULL;
        vel->env->retval_set = 0;
    }

    had_error = vel->err_code;

    if (had_error && catch_body) {
        /* set errvar if provided (argv[1] is errvar name, not "finally") */
        if (argc >= 2 && strcmp(vel_str(argv[1]), "finally") != 0) {
            vel_val_t emsg = vel->err_msg ? vel_val_str(vel->err_msg) : val_make(NULL);
            vel_var_set(vel, vel_str(argv[1]), emsg, VEL_VAR_LOCAL);
            vel_val_free(emsg);
        }
        vel->err_code = VEL_ERR_NONE;
        free(vel->err_msg); vel->err_msg = NULL; vel->err_pos = 0;
        vel->env->stop = saved_stop;
        vel_val_free(r);
        r = vel_parse_val(vel, catch_body, 0);
    } else {
        vel->env->stop = saved_stop;
    }

    /* ALWAYS run finally */
    if (fin_body) {
        int       cur_stop   = vel->env->stop;
        int       cur_ret    = vel->env->retval_set;
        vel_val_t cur_retval = cur_ret ? vel_val_clone(vel->env->retval) : NULL;
        int       cur_err    = vel->err_code;
        char     *cur_errmsg = (cur_err && vel->err_msg) ? vel_strdup(vel->err_msg) : NULL;

        /* clear state so finally runs cleanly */
        vel->env->stop       = 0;
        vel->env->retval_set = 0;
        vel->err_code        = VEL_ERR_NONE;

        vel_val_free(vel_parse_val(vel, fin_body, 0));

        /* restore: original error / stop take precedence over finally's */
        if (cur_err) {
            vel->err_code = cur_err;
            free(vel->err_msg);
            vel->err_msg  = cur_errmsg;
        } else {
            free(cur_errmsg);
        }
        if (cur_ret) {
            vel_val_free(vel->env->retval);
            vel->env->retval     = cur_retval;
            vel->env->retval_set = 1;
            vel->env->stop       = cur_stop;
        } else {
            vel_val_free(cur_retval);
            if (!vel->env->stop) vel->env->stop = cur_stop;
        }
    }

    return r;
}

/* ============================================================
 * Registration
 * ========================================================= */

void register_new_builtins(vel_t vel)
{
    /* List utilities */
    vel_register(vel, "lreverse", cmd_lreverse);
    vel_register(vel, "lsort",    cmd_lsort);
    vel_register(vel, "luniq",    cmd_luniq);
    vel_register(vel, "lassign",  cmd_lassign);

    /* Math helpers */
    vel_register(vel, "abs",      cmd_abs);
    vel_register(vel, "max",      cmd_max);
    vel_register(vel, "min",      cmd_min);
    vel_register(vel, "math",     cmd_math);

    /* String extras (override any previous "string" command) */
    vel_register(vel, "string",   cmd_string);

    /* Clock */
    vel_register(vel, "clock",    cmd_clock);

    /* Dict */
    vel_register(vel, "dict",     cmd_dict);

    /* Base64 */
    vel_register(vel, "base64",   cmd_base64);

    /* upvar */
    vel_register(vel, "upvar",    cmd_upvar);

    /* try...finally replaces the simpler try from vel_cmd.c */
    vel_register(vel, "try",      cmd_try_finally);

    /* reflect extension: save original, install wrapper */
    {
        vel_fn_t orig = (vel_fn_t)vmap_get(&vel->fnmap, "reflect");
        if (orig && orig->native) {
            vel_register(vel, "__reflect_orig", orig->native);
            vel_register(vel, "reflect",        cmd_reflect_ext);
        }
    }
}
