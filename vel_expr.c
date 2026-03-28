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
#include <stdint.h>  /* INT64_MAX, INT64_MIN */

#if defined(__GNUC__) || defined(__clang__)
#  define VEL_ADDOVF(a,b,res) __builtin_add_overflow((a),(b),(res))
#  define VEL_SUBOVF(a,b,res) __builtin_sub_overflow((a),(b),(res))
#  define VEL_MULOVF(a,b,res) __builtin_mul_overflow((a),(b),(res))
#else
/* Manual fallback — correct for two's-complement int64_t */
static int vel_add_ovf(vel_int_t a, vel_int_t b, vel_int_t *res)
{
    *res = (vel_int_t)((uint64_t)a + (uint64_t)b);
    if (b >= 0 && *res < a) return 1;
    if (b <  0 && *res > a) return 1;
    return 0;
}
static int vel_sub_ovf(vel_int_t a, vel_int_t b, vel_int_t *res)
{
    *res = (vel_int_t)((uint64_t)a - (uint64_t)b);
    if (b < 0 && *res < a) return 1;
    if (b > 0 && *res > a) return 1;
    return 0;
}
static int vel_mul_ovf(vel_int_t a, vel_int_t b, vel_int_t *res)
{
    if (a == 0 || b == 0) { *res = 0; return 0; }
    *res = (vel_int_t)((uint64_t)a * (uint64_t)b);
    if (a != 0 && *res / a != b) return 1;
    return 0;
}
#  define VEL_ADDOVF(a,b,res) vel_add_ovf((a),(b),(res))
#  define VEL_SUBOVF(a,b,res) vel_sub_ovf((a),(b),(res))
#  define VEL_MULOVF(a,b,res) vel_mul_ovf((a),(b),(res))
#endif

/* forward declaration */
static void expr_parse(expr_ctx_t *ctx);


static void expr_parse(expr_ctx_t *ctx);

/* ---- helpers ---- */

static void expr_skip_ws(expr_ctx_t *ctx)
{
    while (ctx->pos < ctx->len && isspace((unsigned char)ctx->src[ctx->pos]))
        ctx->pos++;
}

static int is_bad_punct(int c)
{
    return ispunct(c) && c != '!' && c != '~' &&
           c != '(' && c != ')' && c != '-' && c != '+';
}

/* ---- atom ---- */

static void expr_number(expr_ctx_t *ctx)
{
    vel_int_t frac = 0, frac_div = 1;
    ctx->type = EXPR_INT;
    ctx->ival = 0;
    ctx->dval = 0;

    while (ctx->pos < ctx->len) {
        char c = ctx->src[ctx->pos];
        if (c == '.') {
            if (ctx->type == EXPR_FLT) break;
            ctx->type = EXPR_FLT;
            ctx->pos++;
            continue;
        }
        if (!isdigit((unsigned char)c)) break;
        if (ctx->type == EXPR_INT)
            ctx->ival = ctx->ival * 10 + (c - '0');
        else {
            frac = frac * 10 + (c - '0');
            frac_div *= 10;
        }
        ctx->pos++;
    }

    if (ctx->type == EXPR_FLT)
        ctx->dval = (double)ctx->ival + (double)frac / (double)frac_div;
}

static void expr_atom(expr_ctx_t *ctx)
{
    if (isdigit((unsigned char)ctx->src[ctx->pos])) {
        expr_number(ctx);
        return;
    }
    /* non-numeric: treat as truthy, signal soft-stop */
    ctx->type = EXPR_INT;
    ctx->ival = 1;
    ctx->err  = EXERR_STOP;
}

static void expr_paren(expr_ctx_t *ctx)
{
    expr_skip_ws(ctx);
    if (ctx->src[ctx->pos] == '(') {
        ctx->pos++;
        expr_parse(ctx);
        expr_skip_ws(ctx);
        if (ctx->src[ctx->pos] == ')') ctx->pos++;
        else ctx->err = EXERR_SYNTAX;
    } else {
        expr_atom(ctx);
    }
}

/* ---- unary:  - + ~ ! ---- */

static void expr_unary(expr_ctx_t *ctx)
{
    char op;
    expr_skip_ws(ctx);
    if (ctx->pos >= ctx->len || ctx->err) { expr_paren(ctx); return; }

    op = ctx->src[ctx->pos];
    if (op != '-' && op != '+' && op != '~' && op != '!') { expr_paren(ctx); return; }
    ctx->pos++;
    expr_unary(ctx);
    if (ctx->err) return;

    switch (op) {
    case '-':
        if      (ctx->type == EXPR_FLT) ctx->dval = -ctx->dval;
        else                             ctx->ival = -ctx->ival;
        break;
    case '+': break;
    case '~':
        ctx->ival = ~((ctx->type == EXPR_FLT) ? (vel_int_t)ctx->dval : ctx->ival);
        ctx->type = EXPR_INT;
        break;
    case '!':
        if (ctx->type == EXPR_FLT) ctx->dval = !ctx->dval;
        else                        ctx->ival = !ctx->ival;
        break;
    }
}

/* ---- macro to capture LHS before evaluating RHS ---- */
#define SAVE_LHS(rhs_fn) \
    do { \
        double   lf = ctx->dval; \
        vel_int_t li = ctx->ival; \
        int      lt = ctx->type; \
        (void)lf; (void)li; \
        rhs_fn(ctx); \
        if (ctx->err) return

#define END_SAVE } while(0)

/* ---- mul/div:  * / \ % ---- */
static void expr_muldiv(expr_ctx_t *ctx)
{
    expr_unary(ctx);
    expr_skip_ws(ctx);

    while (!ctx->err && ctx->pos < ctx->len) {
        char c  = ctx->src[ctx->pos];
        char nc = ctx->src[ctx->pos + 1];
        if ((c != '*' && c != '/' && c != '\\' && c != '%') || is_bad_punct(nc))
            break;

        ctx->pos++;
        SAVE_LHS(expr_unary);

        if (c == '*') {
            if      (lt == EXPR_FLT && ctx->type == EXPR_FLT) ctx->dval = lf * ctx->dval;
            else if (lt == EXPR_FLT)                           { ctx->dval = lf * ctx->ival; ctx->type = EXPR_FLT; }
            else if (ctx->type == EXPR_FLT)                    { ctx->dval = li * ctx->dval; }
            else {
                /* BUG 6 FIX: detect int64 overflow before storing result */
                vel_int_t res;
                if (VEL_MULOVF(li, ctx->ival, &res))
                    ctx->err = EXERR_OVERFLOW;
                else
                    ctx->ival = res;
            }
        } else if (c == '%') {
            if (ctx->type == EXPR_FLT) {
                if (ctx->dval == 0.0) ctx->err = EXERR_DIVZERO;
                else { ctx->dval = fmod(lt == EXPR_FLT ? lf : (double)li, ctx->dval); ctx->type = EXPR_FLT; }
            } else {
                if (ctx->ival == 0) ctx->err = EXERR_DIVZERO;
                else if (lt == EXPR_FLT) { ctx->dval = fmod(lf, (double)ctx->ival); ctx->type = EXPR_FLT; }
                else ctx->ival = li % ctx->ival;
            }
        } else if (c == '/') {
            double lhf = (lt == EXPR_FLT) ? lf : (double)li;
            double rhf = (ctx->type == EXPR_FLT) ? ctx->dval : (double)ctx->ival;
            if (rhf == 0.0) ctx->err = EXERR_DIVZERO;
            else { ctx->dval = lhf / rhf; ctx->type = EXPR_FLT; }
        } else { /* '\' integer division */
            double lhf = (lt == EXPR_FLT) ? lf : (double)li;
            double rhf = (ctx->type == EXPR_FLT) ? ctx->dval : (double)ctx->ival;
            if (rhf == 0.0) ctx->err = EXERR_DIVZERO;
            else { ctx->ival = (vel_int_t)(lhf / rhf); ctx->type = EXPR_INT; }
        }
        END_SAVE;
        expr_skip_ws(ctx);
    }
}

/* ---- add/sub:  + - ---- */
static void expr_addsub(expr_ctx_t *ctx)
{
    expr_muldiv(ctx);
    expr_skip_ws(ctx);

    while (!ctx->err && ctx->pos < ctx->len) {
        char c  = ctx->src[ctx->pos];
        char nc = ctx->src[ctx->pos + 1];
        if ((c != '+' && c != '-') || is_bad_punct(nc)) break;
        ctx->pos++;
        SAVE_LHS(expr_muldiv);
        {
            int  rf = (ctx->type == EXPR_FLT), lf2 = (lt == EXPR_FLT);
            double r = rf  ? ctx->dval : (double)ctx->ival;
            double l = lf2 ? lf  : (double)li;
            ctx->dval = (c == '+') ? (l + r) : (l - r);
            ctx->type = EXPR_FLT;
            /* downgrade to int if both sides were integers */
            if (!rf && !lf2) {
                /* BUG 6 FIX: detect overflow before storing int result */
                vel_int_t res;
                int ovf = (c == '+') ? VEL_ADDOVF(li, ctx->ival, &res)
                                     : VEL_SUBOVF(li, ctx->ival, &res);
                if (ovf)
                    ctx->err = EXERR_OVERFLOW;
                else {
                    ctx->ival = res;
                    ctx->type = EXPR_INT;
                }
            }
        }
        END_SAVE;
        expr_skip_ws(ctx);
    }
}

/* ---- shift:  << >> ---- */
static void expr_shift(expr_ctx_t *ctx)
{
    expr_addsub(ctx);
    expr_skip_ws(ctx);

    while (!ctx->err && ctx->pos + 1 < ctx->len) {
        char c  = ctx->src[ctx->pos];
        char nc = ctx->src[ctx->pos + 1];
        if (!((c == '<' && nc == '<') || (c == '>' && nc == '>'))) break;
        ctx->pos += 2;
        SAVE_LHS(expr_addsub);
        {
            vel_int_t lv = (lt  == EXPR_FLT) ? (vel_int_t)lf : li;
            vel_int_t rv = (ctx->type == EXPR_FLT) ? (vel_int_t)ctx->dval : ctx->ival;
            ctx->ival = (c == '<') ? (lv << rv) : (lv >> rv);
            ctx->type = EXPR_INT;
        }
        END_SAVE;
        expr_skip_ws(ctx);
    }
}

/* ---- compare:  < > <= >= ---- */
static void expr_cmp(expr_ctx_t *ctx)
{
    expr_shift(ctx);
    expr_skip_ws(ctx);

    while (!ctx->err && ctx->pos < ctx->len) {
        char c  = ctx->src[ctx->pos];
        char nc = ctx->src[ctx->pos + 1];
        int  eq = 0;
        if (c != '<' && c != '>') break;
        if (nc != '=' && is_bad_punct(nc)) break;
        ctx->pos++;
        if (ctx->src[ctx->pos] == '=') { eq = 1; ctx->pos++; }
        SAVE_LHS(expr_shift);
        {
            double l = (lt  == EXPR_FLT) ? lf : (double)li;
            double r = (ctx->type == EXPR_FLT) ? ctx->dval : (double)ctx->ival;
            if      (c == '<' &&  eq) ctx->ival = (l <= r);
            else if (c == '<' && !eq) ctx->ival = (l <  r);
            else if (c == '>' &&  eq) ctx->ival = (l >= r);
            else                       ctx->ival = (l >  r);
            ctx->type = EXPR_INT;
        }
        END_SAVE;
        expr_skip_ws(ctx);
    }
}

/* ---- equality:  == != ---- */
static void expr_eq(expr_ctx_t *ctx)
{
    expr_cmp(ctx);
    expr_skip_ws(ctx);

    while (!ctx->err && ctx->pos + 1 < ctx->len) {
        char c = ctx->src[ctx->pos], nc = ctx->src[ctx->pos + 1];
        if (!((c == '=' && nc == '=') || (c == '!' && nc == '='))) break;
        ctx->pos += 2;
        SAVE_LHS(expr_cmp);
        {
            double l = (lt == EXPR_FLT) ? lf : (double)li;
            double r = (ctx->type == EXPR_FLT) ? ctx->dval : (double)ctx->ival;
            ctx->ival = (c == '=') ? (l == r) : (l != r);
            ctx->type = EXPR_INT;
        }
        END_SAVE;
        expr_skip_ws(ctx);
    }
}

/* ---- bitwise AND: & ---- */
static void expr_band(expr_ctx_t *ctx)
{
    expr_eq(ctx);
    expr_skip_ws(ctx);
    while (!ctx->err && ctx->pos < ctx->len
           && ctx->src[ctx->pos] == '&' && !is_bad_punct(ctx->src[ctx->pos + 1])) {
        ctx->pos++;
        SAVE_LHS(expr_eq);
        ctx->ival = ((lt == EXPR_FLT ? (vel_int_t)lf : li)
                   & (ctx->type == EXPR_FLT ? (vel_int_t)ctx->dval : ctx->ival));
        ctx->type = EXPR_INT;
        END_SAVE;
        expr_skip_ws(ctx);
    }
}

/* ---- bitwise OR: | ---- */
static void expr_bor(expr_ctx_t *ctx)
{
    expr_band(ctx);
    expr_skip_ws(ctx);
    while (!ctx->err && ctx->pos < ctx->len
           && ctx->src[ctx->pos] == '|' && !is_bad_punct(ctx->src[ctx->pos + 1])) {
        ctx->pos++;
        SAVE_LHS(expr_band);
        ctx->ival = ((lt == EXPR_FLT ? (vel_int_t)lf : li)
                   | (ctx->type == EXPR_FLT ? (vel_int_t)ctx->dval : ctx->ival));
        ctx->type = EXPR_INT;
        END_SAVE;
        expr_skip_ws(ctx);
    }
}

/* ---- logical AND: && ---- */
static void expr_land(expr_ctx_t *ctx)
{
    expr_bor(ctx);
    expr_skip_ws(ctx);
    while (!ctx->err && ctx->pos + 1 < ctx->len
           && ctx->src[ctx->pos] == '&' && ctx->src[ctx->pos + 1] == '&') {
        ctx->pos += 2;
        SAVE_LHS(expr_bor);
        {
            int l = (lt == EXPR_FLT) ? (lf != 0.0) : (li != 0);
            int r = (ctx->type == EXPR_FLT) ? (ctx->dval != 0.0) : (ctx->ival != 0);
            ctx->ival = l && r;
            ctx->type = EXPR_INT;
        }
        END_SAVE;
        expr_skip_ws(ctx);
    }
}

/* ---- logical OR: || ---- */
static void expr_lor(expr_ctx_t *ctx)
{
    expr_land(ctx);
    expr_skip_ws(ctx);
    while (!ctx->err && ctx->pos + 1 < ctx->len
           && ctx->src[ctx->pos] == '|' && ctx->src[ctx->pos + 1] == '|') {
        ctx->pos += 2;
        SAVE_LHS(expr_land);
        {
            int l = (lt == EXPR_FLT) ? (lf != 0.0) : (li != 0);
            int r = (ctx->type == EXPR_FLT) ? (ctx->dval != 0.0) : (ctx->ival != 0);
            ctx->ival = l || r;
            ctx->type = EXPR_INT;
        }
        END_SAVE;
        expr_skip_ws(ctx);
    }
}

static void expr_parse(expr_ctx_t *ctx)
{
    expr_lor(ctx);
    /* EXERR_STOP is a soft signal, not a real error */
    if (ctx->err == EXERR_STOP) {
        ctx->err  = EXERR_NONE;
        ctx->ival = 1;
    }
}

/* ============================================================
 * Public API
 * ========================================================= */

vel_val_t vel_eval_expr(vel_t vel, vel_val_t code)
{
    expr_ctx_t ctx;

    code = vel_subst_val(vel, code);
    if (vel->err_code) return NULL;

    ctx.src = vel_str(code);
    if (!ctx.src[0]) {
        vel_val_free(code);
        return vel_val_int(0);
    }

    ctx.pos  = 0;
    ctx.len  = code->len;
    ctx.ival = 0;
    ctx.dval = 0.0;
    ctx.type = EXPR_INT;
    ctx.err  = EXERR_NONE;

    expr_parse(&ctx);
    vel_val_free(code);

    if (ctx.err) {
        switch (ctx.err) {
        case EXERR_DIVZERO:   vel_error_set(vel, "division by zero");          break;
        case EXERR_TYPE:      vel_error_set(vel, "invalid type in expression"); break;
        case EXERR_SYNTAX:    vel_error_set(vel, "expression syntax error");    break;
        case EXERR_OVERFLOW:  vel_error_set(vel, "integer overflow");           break;
        default:              vel_error_set(vel, "expression error");            break;
        }
        return NULL;
    }

    return (ctx.type == EXPR_INT) ? vel_val_int(ctx.ival) : vel_val_dbl(ctx.dval);
}
