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
 * List utilities : lreverse  lsort  luniq  lassign
 * Math helpers   : abs  max  min  math (sin/cos/sqrt/pow/floor/ceil/round/...)
 * String extras  : string (repeat|reverse)
 * Clock          : clock (s|ms|us|ns)
 */

#include "vel_priv.h"
#include <math.h>
#include <time.h>

/* helper: return int if result has no fractional part, else float */
static vel_val_t smart_num(double d)
{
    return (fmod(d, 1.0) == 0.0) ? vel_val_int((vel_int_t)d) : vel_val_dbl(d);
}

/* ============================================================
 * List utilities
 * ========================================================= */

/*
 * lreverse list
 * Returns a new list with elements in reverse order.
 * Example:  lreverse {a b c}  ->  c b a
 */
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

/* qsort comparator — ascending lexicographic order */
static int cmp_val_asc(const void *a, const void *b)
{
    return strcmp(vel_str(*(const vel_val_t *)a),
                  vel_str(*(const vel_val_t *)b));
}

/*
 * lsort list
 * Returns a new list sorted in ascending lexicographic order.
 * Example:  lsort {banana apple cherry}  ->  apple banana cherry
 */
static VELCB vel_val_t cmd_lsort(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_list_t list;
    vel_val_t  r;

    if (!argc) return NULL;
    list = vel_subst_list(vel, argv[0]);

    /* sort in-place using the internal items array (vel_priv.h exposes it) */
    if (list->count > 1)
        qsort(list->items, list->count, sizeof(vel_val_t), cmp_val_asc);

    r = vel_list_pack(list, 1);
    vel_list_free(list);
    return r;
}

/*
 * luniq list
 * Removes consecutive duplicate elements (like Unix uniq).
 * Sort first with lsort to remove ALL duplicates.
 * Example:  luniq {a a b b c a}  ->  a b c a
 */
static VELCB vel_val_t cmd_luniq(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_list_t  in, out;
    vel_val_t   r;
    size_t      i;
    const char *prev = NULL;
    char       *prev_copy = NULL; /* owned copy of previous string */

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
 * lassign list var1 var2 ...
 * Unpacks list elements into named variables (Tcl-style).
 * Extra variables beyond list length are set to "".
 * Extra list elements beyond variable count are silently ignored.
 * Example:
 *   lassign {alice 30 engineer} name age job
 *   # $name=alice  $age=30  $job=engineer
 */
static VELCB vel_val_t cmd_lassign(vel_t vel, size_t argc, vel_val_t *argv)
{
    vel_list_t list;
    size_t     i, n;

    if (argc < 2) return NULL;
    list = vel_subst_list(vel, argv[0]);
    n    = vel_list_len(list);

    for (i = 1; i < argc; i++) {
        vel_val_t v = (i - 1 < n)
                      ? vel_val_clone(vel_list_get(list, i - 1))
                      : val_make(NULL);   /* empty string for unmatched vars */
        vel_var_set(vel, vel_str(argv[i]), v, VEL_VAR_LOCAL);
        vel_val_free(v);
    }

    vel_list_free(list);
    return NULL;
}

/* ============================================================
 * Math helpers
 * ========================================================= */

/*
 * abs n
 * Returns absolute value. Preserves int/float type.
 * Example:  abs -7  ->  7     abs -2.5  ->  2.5
 */
static VELCB vel_val_t cmd_abs(vel_t vel, size_t argc, vel_val_t *argv)
{
    double d;
    (void)vel;
    if (!argc) return NULL;
    d = vel_dbl(argv[0]);
    return smart_num(d < 0.0 ? -d : d);
}

/*
 * max a b
 * Returns the larger of two numbers.
 * Example:  max 3 7  ->  7
 */
static VELCB vel_val_t cmd_max(vel_t vel, size_t argc, vel_val_t *argv)
{
    double a, b;
    (void)vel;
    if (argc < 2) return argc ? vel_val_clone(argv[0]) : NULL;
    a = vel_dbl(argv[0]);
    b = vel_dbl(argv[1]);
    return smart_num(a > b ? a : b);
}

/*
 * min a b
 * Returns the smaller of two numbers.
 * Example:  min 3 7  ->  3
 */
static VELCB vel_val_t cmd_min(vel_t vel, size_t argc, vel_val_t *argv)
{
    double a, b;
    (void)vel;
    if (argc < 2) return argc ? vel_val_clone(argv[0]) : NULL;
    a = vel_dbl(argv[0]);
    b = vel_dbl(argv[1]);
    return smart_num(a < b ? a : b);
}

/*
 * math subcommand arg [arg2]
 *
 * Subcommands (all from <math.h>):
 *   sin cos tan asin acos atan   -- trig (radians)
 *   sqrt log log2 log10 abs      -- one-arg math
 *   floor ceil round             -- rounding
 *   pow base exp                 -- exponentiation
 *   atan2 y x                    -- two-arg arctangent
 *   pi                           -- constant M_PI (no arg needed)
 *   e                            -- constant M_E  (no arg needed)
 *
 * Examples:
 *   math sin 0          ->  0
 *   math sqrt 2         ->  1.4142135623730951
 *   math pow 2 10       ->  1024
 *   math floor 3.7      ->  3
 *   math pi             ->  3.141592653589793
 */
static VELCB vel_val_t cmd_math(vel_t vel, size_t argc, vel_val_t *argv)
{
    const char *op;
    double      a = 0.0, b = 0.0, res;

    if (!argc) { vel_error_set(vel, "math: subcommand required"); return NULL; }
    op = vel_str(argv[0]);

    /* constants — no numeric arg needed */
    if (!strcmp(op, "pi")) return vel_val_dbl(M_PI);
    if (!strcmp(op, "e"))  return vel_val_dbl(M_E);

    /* all other subcommands need at least one numeric arg */
    if (argc < 2) {
        char msg[80];
        snprintf(msg, sizeof(msg), "math %s: argument required", op);
        vel_error_set(vel, msg);
        return NULL;
    }
    a = vel_dbl(argv[1]);

    /* two-argument subcommands */
    if (!strcmp(op, "pow") || !strcmp(op, "atan2")) {
        if (argc < 3) {
            char msg[80];
            snprintf(msg, sizeof(msg), "math %s: two arguments required", op);
            vel_error_set(vel, msg);
            return NULL;
        }
        b = vel_dbl(argv[2]);
        res = (!strcmp(op, "pow")) ? pow(a, b) : atan2(a, b);
        return smart_num(res);
    }

    /* one-argument subcommands */
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
        char msg[80];
        snprintf(msg, sizeof(msg), "math: unknown subcommand '%s'", op);
        vel_error_set(vel, msg);
        return NULL;
    }

    return smart_num(res);
}

/* ============================================================
 * String extras
 * ========================================================= */

/*
 * string subcommand ...
 *
 * string repeat str n    -- repeat str n times
 *   Example:  string repeat ab 3  ->  ababab
 *
 * string reverse str     -- reverse characters of str
 *   Example:  string reverse hello  ->  olleh
 */
static VELCB vel_val_t cmd_string(vel_t vel, size_t argc, vel_val_t *argv)
{
    const char *op;

    if (!argc) { vel_error_set(vel, "string: subcommand required"); return NULL; }
    op = vel_str(argv[0]);

    /* ---- string repeat str n ---- */
    if (!strcmp(op, "repeat")) {
        const char *s;
        size_t      slen, n, i;
        vel_val_t   r;

        if (argc < 3) return val_make(NULL);
        s    = vel_str(argv[1]);
        slen = strlen(s);
        n    = (size_t)vel_int(argv[2]);
        if (!slen || !n) return val_make(NULL);

        r = val_make(NULL);
        for (i = 0; i < n; i++)
            vel_val_cat_str_len(r, s, slen);
        return r;
    }

    /* ---- string reverse str ---- */
    if (!strcmp(op, "reverse")) {
        const char *s;
        size_t      len, i;
        vel_val_t   r;

        if (argc < 2) return val_make(NULL);
        s   = vel_str(argv[1]);
        len = strlen(s);
        r   = val_make(NULL);
        for (i = len; i > 0; i--)
            vel_val_cat_ch(r, s[i - 1]);
        return r;
    }

    {
        char msg[80];
        snprintf(msg, sizeof(msg), "string: unknown subcommand '%s'", op);
        vel_error_set(vel, msg);
        return NULL;
    }
}

/* ============================================================
 * High-precision clock
 * ========================================================= */

/*
 * clock [unit]
 * Returns current wall-clock time as an integer (or float for "s").
 *
 * Units:
 *   s    -- seconds (float, default)
 *   ms   -- milliseconds (integer)
 *   us   -- microseconds (integer)
 *   ns   -- nanoseconds (integer)
 *
 * Examples:
 *   clock ms   -> 1711700000123
 *   clock us   -> 1711700000123456
 *   set t0 [clock ms]
 *   ... do work ...
 *   print [expr [clock ms] - $t0] ms elapsed
 */
static VELCB vel_val_t cmd_clock(vel_t vel, size_t argc, vel_val_t *argv)
{
    struct timespec ts;
    const char     *unit;

    (void)vel;
    clock_gettime(CLOCK_REALTIME, &ts);
    unit = (argc > 0) ? vel_str(argv[0]) : "s";

    if (!strcmp(unit, "ms"))
        return vel_val_int((vel_int_t)ts.tv_sec * 1000LL
                         + (vel_int_t)ts.tv_nsec / 1000000LL);
    if (!strcmp(unit, "us"))
        return vel_val_int((vel_int_t)ts.tv_sec * 1000000LL
                         + (vel_int_t)ts.tv_nsec / 1000LL);
    if (!strcmp(unit, "ns"))
        return vel_val_int((vel_int_t)ts.tv_sec * 1000000000LL
                         + (vel_int_t)ts.tv_nsec);
    /* default: seconds as float */
    return vel_val_dbl((double)ts.tv_sec + (double)ts.tv_nsec / 1.0e9);
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

    /* String extras */
    vel_register(vel, "string",   cmd_string);

    /* Clock */
    vel_register(vel, "clock",    cmd_clock);
}
