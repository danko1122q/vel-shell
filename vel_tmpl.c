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
 * vel_tmpl.c  --  embedded template engine
 *
 * Processes <?vel ... ?> tags inside plain text.
 * Everything outside tags is written verbatim; code inside tags is executed.
 *
 * Literal { and } in text sections are encoded as the escape sequences
 * \o and \c inside a quoted string so they survive the write{} wrapping.
 * The approach: flush accumulated literal as:
 *   write "<accumulated text with \\o and \\c for braces>"
 * This avoids the open/close brace nesting problems of the original approach.
 */

#include "vel_priv.h"

/* write callback used during template rendering */
static VELCB void tmpl_write(vel_t vel, const char *msg)
{
    size_t len;

    if (vel->cb[VEL_CB_FILTER]) {
        vel_filter_cb_t proc = (vel_filter_cb_t)vel->cb[VEL_CB_FILTER];
        msg = proc(vel, msg);
        if (!msg) return;
    }

    len = strlen(msg);
    vel->tmpl_buf = realloc(vel->tmpl_buf, vel->tmpl_len + len + 1);
    memcpy(vel->tmpl_buf + vel->tmpl_len, msg, len + 1);
    vel->tmpl_len += len;
}

/*
 * Flush accumulated literal text as:   write "<text with \\o/\\c escapes>"
 * Using a double-quoted string means { and } are safe as \o and \c.
 */
static void flush_literal(char **code, size_t *codelen,
                           const char *lit, size_t litlen)
{
    size_t i;
    /* worst case: every char becomes \x (2 chars) + quotes + "write " + "\n" */
    size_t maxadd = litlen * 2 + 16;
    *code = realloc(*code, *codelen + maxadd);

    (*code)[(*codelen)++] = '\n';
    (*code)[(*codelen)++] = 'w';
    (*code)[(*codelen)++] = 'r';
    (*code)[(*codelen)++] = 'i';
    (*code)[(*codelen)++] = 't';
    (*code)[(*codelen)++] = 'e';
    (*code)[(*codelen)++] = ' ';
    (*code)[(*codelen)++] = '"';

    for (i = 0; i < litlen; i++) {
        char c = lit[i];
        if (c == '{') {
            (*code)[(*codelen)++] = '\\';
            (*code)[(*codelen)++] = 'o';
        } else if (c == '}') {
            (*code)[(*codelen)++] = '\\';
            (*code)[(*codelen)++] = 'c';
        } else if (c == '"') {
            (*code)[(*codelen)++] = '\\';
            (*code)[(*codelen)++] = '"';
        } else if (c == '\\') {
            (*code)[(*codelen)++] = '\\';
            (*code)[(*codelen)++] = '\\';
        } else {
            (*code)[(*codelen)++] = c;
        }
    }

    (*code)[(*codelen)++] = '"';
    (*code)[(*codelen)++] = '\n';
}

char *vel_template(vel_t vel, const char *src, unsigned int flags)
{
    char  *prev_buf      = vel->tmpl_buf;
    size_t prev_len      = vel->tmpl_len;
    vel_cb_t prev_write  = vel->cb[VEL_CB_WRITE];

    char  *code    = NULL;
    size_t codelen = 0;
    char  *lit     = NULL;   /* accumulated literal text */
    size_t litlen  = 0;
    size_t i       = 0;
    size_t srclen  = strlen(src);
    char  *result;

    (void)flags;

    vel->cb[VEL_CB_WRITE] = (vel_cb_t)tmpl_write;
    vel->tmpl_buf = NULL;
    vel->tmpl_len = 0;

    while (i < srclen) {
        /* check for <?vel opening tag */
        if (i + 4 < srclen &&
            src[i]     == '<' && src[i+1] == '?' &&
            src[i+2]   == 'v' && src[i+3] == 'e' && src[i+4] == 'l') {

            i += 5;

            /* flush pending literal */
            if (litlen) {
                flush_literal(&code, &codelen, lit, litlen);
                free(lit);
                lit    = NULL;
                litlen = 0;
            }

            /* copy code until ?> closing tag */
            while (i < srclen) {
                if (i + 1 < srclen && src[i] == '?' && src[i+1] == '>') {
                    i += 2;
                    break;
                }
                code = realloc(code, codelen + 1);
                code[codelen++] = src[i++];
            }
            code = realloc(code, codelen + 1);
            code[codelen++] = '\n';

        } else {
            /* accumulate literal character */
            lit = realloc(lit, litlen + 1);
            lit[litlen++] = src[i++];
        }
    }

    /* flush remaining literal */
    if (litlen) {
        flush_literal(&code, &codelen, lit, litlen);
        free(lit);
    }

    code = realloc(code, codelen + 1);
    code[codelen] = '\0';

    vel_val_free(vel_parse(vel, code, 0, 1));
    free(code);

    result = vel->tmpl_buf ? vel->tmpl_buf : vel_strdup("");

    vel->tmpl_buf        = prev_buf;
    vel->tmpl_len        = prev_len;
    vel->cb[VEL_CB_WRITE] = prev_write;

    return result;
}
