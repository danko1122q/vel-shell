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
 * vel_lex.c  --  lexer: whitespace skipping, token reading, line tokenization
 *
 * Tokens are read left-to-right from vel->src[vel->pos].
 * Five forms:
 *   $name       variable expansion
 *   {text}      brace-quoted, no interpolation
 *   [code]      bracket-quoted, evaluated immediately
 *   "str"/'str' quoted string with interpolation and escapes
 *   word        bare word (stops at whitespace or special chars)
 */

#include "vel_priv.h"

static int is_special(char c)
{
    return c == '$' || c == '{' || c == '}' ||
           c == '[' || c == ']' ||
           c == '"' || c == '\'' || c == ';' ||
           c == '>' || c == '<' || c == '|';
}

static int is_eol(char c)
{
    return c == '\n' || c == '\r' || c == ';';
}

static int at_eol(vel_t vel)
{
    return !vel->skip_eol && is_eol(vel->src[vel->pos]);
}

void lex_skip_ws(vel_t vel)
{
    while (vel->pos < vel->src_len) {
        char c = vel->src[vel->pos];

        if (c == '#') {
            /* block comment: ## ... ## (but NOT ###) */
            /* FIX: guard pos+2 access — was UB when pos+2 >= src_len */
            if (vel->pos + 1 < vel->src_len &&
                vel->src[vel->pos + 1] == '#' &&
                (vel->pos + 2 >= vel->src_len || vel->src[vel->pos + 2] != '#')) {
                vel->pos += 2;
                while (vel->pos < vel->src_len) {
                    if (vel->src[vel->pos] == '#' &&
                        vel->pos + 1 < vel->src_len &&
                        vel->src[vel->pos + 1] == '#' &&
                        (vel->pos + 2 >= vel->src_len || vel->src[vel->pos + 2] != '#')) {
                        vel->pos += 2;
                        break;
                    }
                    vel->pos++;
                }
            } else {
                /* line comment */
                while (vel->pos < vel->src_len && !is_eol(vel->src[vel->pos]))
                    vel->pos++;
            }
        } else if (c == '\\' && vel->pos + 1 < vel->src_len
                   && is_eol(vel->src[vel->pos + 1])) {
            /* line continuation */
            vel->pos++;
            while (vel->pos < vel->src_len && is_eol(vel->src[vel->pos]))
                vel->pos++;
        } else if (is_eol(c)) {
            if (vel->skip_eol) vel->pos++;
            else               break;
        } else if (isspace((unsigned char)c)) {
            vel->pos++;
        } else {
            break;
        }
    }
}

/* ============================================================
 * read_heredoc  --  <<WORD ... WORD
 *
 * Called after lexer has seen "<<".  Reads the delimiter (no spaces
 * allowed), then accumulates lines until a line that is exactly the
 * delimiter (optionally preceded by horizontal whitespace).
 * Returns the accumulated text as a literal string value.
 * ========================================================= */
static vel_val_t read_heredoc(vel_t vel)
{
    /* collect delimiter word (stops at whitespace or EOL) */
    char delim[128];
    size_t dlen = 0;

    while (vel->pos < vel->src_len) {
        char c = vel->src[vel->pos];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') break;
        if (dlen < sizeof(delim) - 1)
            delim[dlen++] = c;
        vel->pos++;
    }
    delim[dlen] = '\0';

    /* skip to next line */
    while (vel->pos < vel->src_len &&
           (vel->src[vel->pos] == '\r' || vel->src[vel->pos] == '\n'))
        vel->pos++;

    if (!dlen) return val_make(NULL);

    vel_val_t out = val_make(NULL);

    while (vel->pos < vel->src_len) {
        /* find end-of-line */
        size_t line_start = vel->pos;
        while (vel->pos < vel->src_len &&
               vel->src[vel->pos] != '\n' && vel->src[vel->pos] != '\r')
            vel->pos++;
        size_t line_end = vel->pos;

        /* remember position of the EOL bytes before skipping them */
        size_t eol_start = vel->pos;

        /* skip over EOL bytes */
        while (vel->pos < vel->src_len &&
               (vel->src[vel->pos] == '\n' || vel->src[vel->pos] == '\r'))
            vel->pos++;

        /* strip leading horizontal whitespace from the line for delimiter check */
        size_t ls = line_start;
        while (ls < line_end &&
               (vel->src[ls] == ' ' || vel->src[ls] == '\t'))
            ls++;

        size_t content_len = line_end - ls;

        /* check if this line IS the delimiter */
        if (content_len == dlen &&
            memcmp(vel->src + ls, delim, dlen) == 0) {
            /*
             * FIX: rewind pos to the EOL that ended the delimiter line.
             * This ensures lex_tokenize's do-while condition sees is_eol()
             * immediately after read_heredoc returns, terminating the current
             * statement correctly so the NEXT statement (e.g. an assert call)
             * is not accidentally concatenated into the heredoc token.
             */
            vel->pos = eol_start;
            break;   /* done — do not include delimiter line */
        }

        /* append line + newline using GC-safe helpers */
        vel_val_cat_str_len(out, vel->src + line_start, line_end - line_start);
        vel_val_cat_ch(out, '\n');
    }

    return out;
}

/* read [code] — evaluates immediately */
static vel_val_t read_bracket(vel_t vel)
{
    int  saved_eol = vel->skip_eol;
    vel_val_t code = val_make(NULL);
    vel_val_t result;
    int depth = 1;

    vel->skip_eol = 0;
    vel->pos++;   /* skip '[' */

    while (vel->pos < vel->src_len) {
        char c = vel->src[vel->pos];
        if      (c == '[') { vel->pos++; depth++; vel_val_cat_ch(code, '['); }
        else if (c == ']') { vel->pos++; if (--depth == 0) break; vel_val_cat_ch(code, ']'); }
        else               { vel_val_cat_ch(code, vel->src[vel->pos++]); }
    }

    result = vel_parse_val(vel, code, 0);
    vel_val_free(code);
    vel->skip_eol = saved_eol;
    return result;
}

/* read $name — expands variable, or $((...)) for arithmetic */
static vel_val_t read_dollar(vel_t vel)
{
    vel_val_t name, key, result;
    vel->pos++;   /* skip '$' */

    /* $((expr)) — arithmetic expansion */
    if (vel->pos + 1 < vel->src_len &&
        vel->src[vel->pos] == '(' && vel->src[vel->pos + 1] == '(') {
        vel->pos += 2;   /* skip "((" */
        vel_val_t expr_val = val_make(NULL);
        /*
         * FIX (Bug 3): Track paren depth properly.
         * The opening "((" bumps depth to 2. Every subsequent '(' adds 1,
         * every ')' subtracts 1. When depth reaches 1 AND the next char is
         * also ')', we have found the closing "))".
         *
         * Old code peeked for src[pos+1]==')' at depth==1, which incorrectly
         * consumed the ')' of a sub-expression like $((a > (b+1))) — the ')'
         * closing (b+1) appeared at depth==1 and was mistaken for part of )).
         */
        int depth = 2;
        while (vel->pos < vel->src_len) {
            char c = vel->src[vel->pos];
            if (c == '(') {
                depth++;
                vel_val_cat_ch(expr_val, c);
                vel->pos++;
            } else if (c == ')') {
                depth--;
                if (depth == 1) {
                    /* next ')' closes the outer "((" */
                    if (vel->pos + 1 < vel->src_len &&
                        vel->src[vel->pos + 1] == ')') {
                        vel->pos += 2;   /* consume both ')' */
                        break;
                    }
                    /* no second ')': malformed — emit what we have */
                    vel->pos++;
                    break;
                }
                vel_val_cat_ch(expr_val, c);
                vel->pos++;
            } else {
                vel_val_cat_ch(expr_val, c);
                vel->pos++;
            }
        }
        result = vel_eval_expr(vel, expr_val);
        /* BUG 1 FIX: vel_eval_expr does NOT free its input — we must free expr_val here.
         * The old comment "vel_eval_expr frees expr_val internally" was wrong;
         * vel_eval_expr calls vel_subst_val which creates a NEW val and frees THAT,
         * leaving the original expr_val as a leak on every $((expr)) evaluation. */
        vel_val_free(expr_val);
        if (!result) result = vel_val_int(0);
        return result;
    }

    if (vel->pos < vel->src_len) {
        char c = vel->src[vel->pos];
        if (c == '{' || c == '[' || c == '$' || c == '"' || c == '\'') {
            name = lex_next_token(vel);
        } else if (c == '?' || c == '!' || c == '@') {
            /* $? $! $@ — special single-char shell variables */
            name = val_make_len(vel->src + vel->pos, 1);
            vel->pos++;
        } else {
            size_t start = vel->pos;
            while (vel->pos < vel->src_len) {
                char ic = vel->src[vel->pos];
                if (!isalnum((unsigned char)ic) && ic != '_') break;
                vel->pos++;
            }
            name = val_make_len(vel->src + start, vel->pos - start);
        }
    } else {
        name = val_make(NULL);
    }

    key    = val_make(vel->dollar_prefix);
    vel_val_cat(key, name);
    vel_val_free(name);
    result = vel_parse_val(vel, key, 0);
    vel_val_free(key);
    return result;
}

vel_val_t lex_next_token(vel_t vel)
{
    vel_val_t tok;
    size_t    start;

    lex_skip_ws(vel);

    if (vel->pos >= vel->src_len)
        return val_make(NULL);

    switch (vel->src[vel->pos]) {

    case '$':
        return read_dollar(vel);

    case '{': {
        int depth = 1;
        vel->pos++;
        tok = val_make(NULL);
        while (vel->pos < vel->src_len) {
            char c = vel->src[vel->pos];
            if      (c == '{') { vel->pos++; depth++; vel_val_cat_ch(tok, '{'); }
            else if (c == '}') { vel->pos++; if (--depth == 0) break; vel_val_cat_ch(tok, '}'); }
            else               { vel_val_cat_ch(tok, vel->src[vel->pos++]); }
        }
        return tok;
    }

    case '[':
        return read_bracket(vel);

    case '"':
    case '\'': {
        char quote = vel->src[vel->pos++];
        tok = val_make(NULL);
        while (vel->pos < vel->src_len) {
            char c = vel->src[vel->pos];
            if (c == '[' || c == '$') {
                vel_val_t part = (c == '$') ? read_dollar(vel) : read_bracket(vel);
                vel_val_cat(tok, part);
                vel_val_free(part);
                vel->pos--;   /* compensate for the ++ below */
            } else if (c == '\\') {
                vel->pos++;
                switch (vel->src[vel->pos]) {
                case 'b':  vel_val_cat_ch(tok, '\b'); break;
                case 't':  vel_val_cat_ch(tok, '\t'); break;
                case 'n':  vel_val_cat_ch(tok, '\n'); break;
                case 'v':  vel_val_cat_ch(tok, '\v'); break;
                case 'f':  vel_val_cat_ch(tok, '\f'); break;
                case 'r':  vel_val_cat_ch(tok, '\r'); break;
                case '0':  vel_val_cat_ch(tok,  '\0'); break;
                case 'a':  vel_val_cat_ch(tok, '\a'); break;
                case 'c':  vel_val_cat_ch(tok,  '}'); break;
                case 'o':  vel_val_cat_ch(tok,  '{'); break;
                default:   vel_val_cat_ch(tok, vel->src[vel->pos]); break;
                }
            } else if (c == quote) {
                vel->pos++;
                break;
            } else {
                vel_val_cat_ch(tok, c);
            }
            vel->pos++;
        }
        return tok;
    }

    case '>':
        /* >> or > */
        vel->pos++;
        if (vel->pos < vel->src_len && vel->src[vel->pos] == '>') {
            vel->pos++;
            return val_make(">>");
        }
        return val_make(">");

    case '<':
        vel->pos++;
        /* heredoc: << WORD */
        if (vel->pos < vel->src_len && vel->src[vel->pos] == '<') {
            vel->pos++;
            /* skip optional space between << and delimiter */
            while (vel->pos < vel->src_len &&
                   (vel->src[vel->pos] == ' ' || vel->src[vel->pos] == '\t'))
                vel->pos++;
            return read_heredoc(vel);
        }
        return val_make("<");

    case '|':
        vel->pos++;
        return val_make("|");

    default:
        /* 2> stderr redirect: bare token starting with '2' followed by '>' */
        if (vel->src[vel->pos] == '2' &&
            vel->pos + 1 < vel->src_len &&
            vel->src[vel->pos + 1] == '>') {
            vel->pos += 2;
            return val_make("2>");
        }
        start = vel->pos;
        while (vel->pos < vel->src_len
               && !isspace((unsigned char)vel->src[vel->pos])
               && !is_special(vel->src[vel->pos]))
            vel->pos++;
        return val_make_len(vel->src + start, vel->pos - start);
    }
}

/* tokenize one statement into a word list */
vel_list_t lex_tokenize(vel_t vel)
{
    vel_list_t words = vel_list_new();

    lex_skip_ws(vel);

    while (vel->pos < vel->src_len && !at_eol(vel) && !vel->err_code) {
        vel_val_t w = val_make(NULL);

        do {
            size_t    before = vel->pos;
            vel_val_t part   = lex_next_token(vel);

            if (before == vel->pos) {
                /* Lexer tidak bisa advance — karakter tidak dikenali.
                 * Set error agar caller tahu ada masalah, jangan diam. */
                vel_error_set_at(vel, vel->pos,
                    "lexer stuck: unexpected character in input");
                vel_val_free(w);
                vel_val_free(part);
                vel_list_free(words);
                return NULL;
            }
            vel_val_cat(w, part);
            vel_val_free(part);
        } while (vel->pos < vel->src_len
                 && !is_eol(vel->src[vel->pos])
                 && !isspace((unsigned char)vel->src[vel->pos])
                 && !vel->err_code);

        lex_skip_ws(vel);
        vel_list_push(words, w);
    }

    /* ----------------------------------------------------------------
     * Pipe sugar:  cmd1 | cmd2 | cmd3
     *
     * Jika ada token "|" di words, ubah seluruh word list menjadi:
     *   pipe {cmd1 args} {cmd2 args} {cmd3 args}
     *
     * Pipe sugar diproses SEBELUM redirect sugar agar redirect
     * di dalam pipe segment tetap bisa dideteksi.
     *
     * BUG FIX: skip_redir is set by vel_eval_expr so that '|', '>', '<'
     * are treated as plain tokens (comparison/logical operators in
     * arithmetic expressions), not as shell pipe/redirect operators.
     * ---------------------------------------------------------------- */
#ifndef WIN32
    if (!vel->skip_redir)
    {
        size_t i;
        int has_pipe = 0;
        for (i = 0; i < words->count; i++) {
            if (!strcmp(vel_str(words->items[i]), "|")) { has_pipe = 1; break; }
        }
        if (has_pipe) {
            vel_list_t new_words = vel_list_new();
            vel_list_push(new_words, vel_val_str("pipe"));

            vel_list_t seg = vel_list_new();
            for (i = 0; i < words->count; i++) {
                if (!strcmp(vel_str(words->items[i]), "|")) {
                    vel_val_t packed = vel_list_pack(seg, 1);
                    vel_list_free(seg);
                    vel_list_push(new_words, packed);
                    seg = vel_list_new();
                } else {
                    vel_list_push(seg, vel_val_clone(words->items[i]));
                }
            }
            /* last segment */
            vel_val_t packed = vel_list_pack(seg, 1);
            vel_list_free(seg);
            vel_list_push(new_words, packed);

            vel_list_free(words);
            words = new_words;
        }
    }
#endif

    /* ----------------------------------------------------------------
     * Redirect sugar:  cmd args > file
     *                  cmd args >> file
     *                  cmd args 2> file
     *                  cmd args > file 2> errfile    (both at once)
     *
     * BUG 2 FIX: old code did a linear scan and broke on the first
     * redirect operator, so a second operator like "2>" in
     *   cmd > out.txt 2> err.txt
     * was silently discarded.
     *
     * New algorithm: single pass, collect ALL redirect ops, build cmd
     * word list by skipping op+filename pairs, then emit:
     *   - one redirect only  ->  redirect {cmd} file [mode]   (unchanged)
     *   - both stdout+stderr ->  redirect2 {cmd} out mode err
     * ---------------------------------------------------------------- */
#ifndef WIN32
    if (!vel->skip_redir)
    {
        size_t i, k;
        size_t stdout_idx = (size_t)-1;
        size_t stderr_idx = (size_t)-1;
        size_t stdin_idx  = (size_t)-1;
        int    stdout_app = 0;

        for (i = 0; i < words->count; i++) {
            const char *tok = vel_str(words->items[i]);
            if (i + 1 < words->count) {
                if      (!strcmp(tok, ">"))  { stdout_idx = i; stdout_app = 0; }
                else if (!strcmp(tok, ">>")) { stdout_idx = i; stdout_app = 1; }
                else if (!strcmp(tok, "2>")) { stderr_idx = i; }
                else if (!strcmp(tok, "<"))  { stdin_idx  = i; }
            }
        }

        if (stdout_idx != (size_t)-1 || stderr_idx != (size_t)-1 || stdin_idx != (size_t)-1) {
            size_t skip[6]; size_t nskip = 0;
            if (stdout_idx != (size_t)-1) { skip[nskip++] = stdout_idx; skip[nskip++] = stdout_idx + 1; }
            if (stderr_idx != (size_t)-1) { skip[nskip++] = stderr_idx; skip[nskip++] = stderr_idx + 1; }
            if (stdin_idx  != (size_t)-1) { skip[nskip++] = stdin_idx;  skip[nskip++] = stdin_idx  + 1; }

            vel_list_t cmd_words = vel_list_new();
            for (i = 0; i < words->count; i++) {
                int skipped = 0;
                for (k = 0; k < nskip; k++) if (i == skip[k]) { skipped = 1; break; }
                if (!skipped) vel_list_push(cmd_words, vel_val_clone(words->items[i]));
            }
            vel_val_t packed = vel_list_pack(cmd_words, 1);
            vel_list_free(cmd_words);

            vel_val_t stdout_file = (stdout_idx != (size_t)-1)
                ? vel_val_clone(words->items[stdout_idx + 1]) : NULL;
            vel_val_t stderr_file = (stderr_idx != (size_t)-1)
                ? vel_val_clone(words->items[stderr_idx + 1]) : NULL;
            vel_val_t stdin_file  = (stdin_idx  != (size_t)-1)
                ? vel_val_clone(words->items[stdin_idx  + 1]) : NULL;

            vel_list_free(words);
            words = vel_list_new();

            if (stdout_file && stderr_file) {
                /* redirect2 {cmd} stdout_file mode stderr_file
                 * mode = "append" | "" */
                vel_list_push(words, vel_val_str("redirect2"));
                vel_list_push(words, packed);
                vel_list_push(words, stdout_file);
                vel_list_push(words, vel_val_str(stdout_app ? "append" : ""));
                vel_list_push(words, stderr_file);
                if (stdin_file) vel_val_free(stdin_file);
            } else if (stdout_file) {
                vel_list_push(words, vel_val_str("redirect"));
                vel_list_push(words, packed);
                vel_list_push(words, stdout_file);
                if (stdout_app) vel_list_push(words, vel_val_str("append"));
                if (stdin_file) vel_val_free(stdin_file);
            } else if (stderr_file && !stdin_file) {
                vel_list_push(words, vel_val_str("redirect"));
                vel_list_push(words, packed);
                vel_list_push(words, stderr_file);
                vel_list_push(words, vel_val_str("stderr"));
            } else if (stdin_file) {
                /* stdin-only redirect: redirect_in {cmd} file */
                vel_list_push(words, vel_val_str("redirect_in"));
                vel_list_push(words, packed);
                vel_list_push(words, stdin_file);
                if (stderr_file) vel_val_free(stderr_file);
            }
        }
    }
#endif

    return words;
}

/* ============================================================
 * Public substitution API
 * ========================================================= */

vel_list_t vel_subst_list(vel_t vel, vel_val_t code)
{
    const char *saved_src  = vel->src;
    size_t      saved_len  = vel->src_len;
    size_t      saved_pos  = vel->pos;
    int         saved_eol  = vel->skip_eol;
    vel_list_t  words;

    vel->src      = vel_str(code);
    vel->src_len  = code->len;
    vel->pos      = 0;
    vel->skip_eol = 1;

    words = lex_tokenize(vel);
    if (!words) words = vel_list_new();

    vel->src      = saved_src;
    vel->src_len  = saved_len;
    vel->pos      = saved_pos;
    vel->skip_eol = saved_eol;
    return words;
}

vel_val_t vel_subst_val(vel_t vel, vel_val_t code)
{
    vel_list_t words = vel_subst_list(vel, code);
    vel_val_t  val   = vel_list_pack(words, 0);
    vel_list_free(words);
    return val;
}
