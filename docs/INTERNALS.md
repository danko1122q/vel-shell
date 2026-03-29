# Vel Internals

This document describes the internal architecture of the vel interpreter for developers who need to understand, modify, or extend the implementation.

---

## Table of Contents

1. [Source File Overview](#source-file-overview)
2. [Build System](#build-system)
3. [Core Data Structures](#core-data-structures)
4. [Memory Management and GC](#memory-management-and-gc)
5. [Lexer](#lexer)
6. [Execution Engine](#execution-engine)
7. [Expression Evaluator](#expression-evaluator)
8. [Hash Map](#hash-map)
9. [Environment and Scope](#environment-and-scope)
10. [Function Table](#function-table)
11. [Call Stack and Error Reporting](#call-stack-and-error-reporting)
12. [Job Control Subsystem](#job-control-subsystem)
13. [Template Engine](#template-engine)
14. [Extended Built-in Commands](#extended-built-in-commands)
15. [Notable Bug Fixes](#notable-bug-fixes)
16. [Compile-Time Configuration](#compile-time-configuration)
17. [Platform Notes](#platform-notes)

---

## Source File Overview

| File | Responsibility |
|------|----------------|
| `vel.h` | Public API declarations and opaque types |
| `vel_priv.h` | Internal struct definitions and declarations |
| `main.c` | REPL, file runner, stdin runner, native commands (`system`, `readline`, `writechar`, `canread`) |
| `vel_run.c` | Core execution loop (`vel_parse`), error API, callback API, public lifecycle API |
| `vel_lex.c` | Lexer: whitespace skipping, token reading, heredoc, line tokenization |
| `vel_cmd.c` | Built-in language commands (control flow, strings, lists, functions, I/O, eval) |
| `vel_sys.c` | System and filesystem commands; `run`, `exec`, `pipe`, `redirect`, file operations |
| `vel_extra.c` | Shell-utility commands: `ls`, `tree`, `cat`, `grep`, `wc`, `head`, `tail`, `stat`, etc. |
| `vel_jobs.c` | POSIX job control: `spawn`, `wait`, `fg`, `jobs`, `sighandle`, pipeline management |
| `vel_expr.c` | Arithmetic/logical expression evaluator |
| `vel_mem.c` | Value, list, environment, variable, and function allocation/lifecycle |
| `vel_map.c` | Hash map used for function and variable lookup |
| `vel_tmpl.c` | Template engine: `<?vel ... ?>` processing |
| `vel_newcmds.c` | Extended built-in commands: list utilities, math, string extras, clock, dict, base64, upvar, reflect extensions, `try...finally` |

---

## Build System

The Makefile compiles all `.c` files to object files and links them together. Every object file depends on `vel.h`, `vel_priv.h`, and `vel_jobs.h` to ensure recompilation on any header change.

Flags:
- `-std=c99` — C99 is the language standard throughout.
- `-D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L` — enables POSIX.1-2008 APIs (`strtok_r`, `sigaction`, `pread`, etc.) on glibc.
- `-lm` — links `libm` for `fmod` in the expression evaluator and math subcommands.

Optional:
- `READLINE=1` defines `VEL_USE_READLINE` and links `-lreadline` for the REPL.

---

## Core Data Structures

### `struct vel_s` (the interpreter, `vel_t`)

The central interpreter state. Key fields:

- `src`, `src_len`, `pos`: lexer cursor into the current source string.
- `root_src`, `root_src_buf`: pointer to and owned copy of the original script (used in error messages and `reflect this`).
- `fn`, `fn_count`, `fn_sys`, `fnmap`: function table (array + hash map for O(1) lookup).
- `env`, `root_env`: current and root environment pointers.
- `catcher`, `catcher_depth`: default handler for unknown commands.
- `dollar_prefix`: string prepended when expanding `$name` (default `"set "`).
- `err_code`, `err_pos`, `err_msg`: error state.
- `cb[VEL_CB_COUNT]`: callback function pointers.
- `depth`: current recursion depth.
- `stack_names[VEL_STACK_MAX]`, `stack_pos[VEL_STACK_MAX]`, `stack_depth`: call stack for error traces.
- `tmpl_buf`, `tmpl_len`: output accumulation buffer used by the template engine.
- `data`: user-supplied opaque pointer.
- `empty`: shared singleton empty value (never freed).

### `struct vel_value_s` (`vel_val_t`)

```c
struct vel_value_s {
    size_t len;
    char  *data;
};
```

`data` points into a heap block with a hidden `size_t` refcount stored immediately before the string bytes. See the GC section below.

### `struct vel_var_s` (`vel_var_t`)

```c
struct vel_var_s {
    char      *name;
    char      *watch;    /* code to run on assignment */
    vel_env_t  env;
    vel_val_t  value;
};
```

### `struct vel_env_s` (`vel_env_t`)

```c
struct vel_env_s {
    vel_env_t   parent;
    vel_fn_t    func;          /* function this env belongs to, or NULL */
    vel_val_t   catcher_name;
    vel_var_t  *vars;
    size_t      var_count;
    vel_map_t   varmap;        /* O(1) variable lookup */
    vel_val_t   retval;
    int         retval_set;
    int         stop;          /* break/return signal */
};
```

### `struct vel_func_s` (`vel_fn_t`)

```c
struct vel_func_s {
    char         *name;
    vel_val_t     body;       /* source code for scripted functions */
    vel_list_t    params;     /* parameter names */
    vel_fn_proc_t native;     /* C function pointer, or NULL for scripted */
};
```

### `struct vel_list_s` (`vel_list_t`)

```c
struct vel_list_s {
    vel_val_t *items;
    size_t     count;
    size_t     cap;
};
```

A dynamic array of `vel_val_t` items. Items are owned by the list.

### `vel_map_t`

```c
typedef struct vel_map_s {
    map_bucket_t bucket[MAP_BUCKETS]; /* MAP_BUCKETS = 1024 */
} vel_map_t;
```

Each bucket holds an array of `map_entry_t` (key-value pairs). The hash function is djb2. See the Hash Map section.

---

## Memory Management and GC

Vel uses reference-counted string data with copy-on-write semantics (implemented in `vel_mem.c`).

### Hidden-prefix refcount layout

Every string data buffer is allocated as:

```
[ size_t refcount | char data[len+1] ]
```

The `vel_val_t.data` pointer points to the start of the `char` area, not to the `size_t`. The refcount is accessed via:

```c
#define GC_RC(data)  (*(size_t *)((char *)(data) - sizeof(size_t)))
```

### Reference counting

- `val_make_len` / `val_make` allocate a fresh buffer with `refcount = 1`.
- `vel_val_clone` increments the refcount and shares the buffer: O(1) clone.
- `vel_val_free` decrements the refcount; frees the buffer when it reaches 0.

### Copy-on-write (COW)

Before any mutation (the `vel_val_cat_*` functions), the internal `gc_cow` function checks whether `refcount > 1`. If so, it makes a private copy before proceeding. This ensures that a cloned value is not silently modified when the original is mutated.

### Value struct lifetime

`vel_val_t` (the struct itself, not the string buffer) is allocated with `calloc` and freed by `vel_val_free`. The `vel->empty` field holds a shared singleton empty value with a refcount managed separately to avoid double-free.

---

## Lexer

The lexer is in `vel_lex.c` and operates by reading characters from `vel->src` starting at `vel->pos`.

### `lex_skip_ws(vel_t vel)`

Advances `vel->pos` past whitespace, line comments (`# ...`), block comments (`## ... ##`), and line continuations (`\` at end of line). Block comments are delimited by `##` (not `###`).

### `lex_next_token(vel_t vel)`

Reads and returns the next token as a `vel_val_t`. The five token forms:

1. `$name` — looks up the variable and returns its value.
2. `{text}` — reads brace-quoted literal text (no substitution, brace nesting tracked).
3. `[code]` — reads bracket-quoted code, calls `vel_parse` recursively, returns the result.
4. `"text"` or `'text'` — reads a quoted string, performing `$name` and `[code]` substitution and processing escape sequences.
5. Bare word — reads until whitespace or a special character.

Heredoc (`<<DELIM`) is handled as a special case in the bare-word path.

### `lex_tokenize(vel_t vel)`

Reads all tokens on the current line (up to an EOL or semicolon) and returns them as a `vel_list_t`. `vel->skip_eol` can be set to suppress EOL as a statement terminator (used internally during some multi-token reads).

---

## Execution Engine

The execution engine is in `vel_run.c`.

### `vel_parse`

The main execution loop:

1. Saves and restores lexer state (`src`, `src_len`, `pos`) for nested calls.
2. Records `root_src` on the first call (depth == 1) for error messages.
3. Increments `depth`; errors if `depth > VEL_MAX_DEPTH`.
4. Loops: calls `lex_tokenize` to get the next statement, then dispatches.

### Command dispatch

For each tokenized statement:

1. The first word is the command name.
2. `mem_find_fn` looks it up in the function hash map.
3. If found as a native function (`fn->native != NULL`), calls it directly.
4. If found as a scripted function, pushes a new environment, binds parameters, calls `vel_parse` on the body.
5. If not found and not on `PATH` (Unix), tries the `catcher` if set.

### Auto-exec (Unix only)

If a command name is not in the function table but contains `/` or is found on `PATH` via `access(fullpath, X_OK)`, vel forks and execs it directly with inherited stdio. The exit code is stored in `g_last_exit` and the `?` variable.

### Stop propagation

The `env->stop` flag is used by `break`, `continue`, and `return` to abort the current execution loop without entering error state. The flag is checked at the top of the execution loop and propagated up through `vel_parse` recursion.

---

## Expression Evaluator

The expression evaluator is in `vel_expr.c`. It operates on a flat string (after variable substitution) using a recursive descent parser.

### Grammar (simplified)

```
expr    := lor
lor     := land ( '||' land )*
land    := bor  ( '&&' bor  )*
bor     := band ( '|'  band )*
band    := eq   ( '&'  eq   )*
eq      := cmp  ( ('==' | '!=') cmp )*
cmp     := shift ( ('<' | '>' | '<=' | '>=') shift )*
shift   := add  ( ('<<' | '>>') add )*
add     := mul  ( ('+' | '-') mul )*
mul     := unary ( ('*' | '/' | '\' | '%') unary )*
unary   := ('-' | '+' | '~' | '!') unary | paren
paren   := '(' expr ')' | atom
atom    := number | non-numeric (truthy)
```

### Type system

The evaluator tracks whether each operand is integer (`EXPR_INT`) or float (`EXPR_FLT`) in the `expr_ctx_t`. Integer division (`\`) always returns integer. Float division (`/`) always returns float. Arithmetic promotes to float if either operand is float.

### Overflow detection

Integer addition, subtraction, and multiplication check for `int64_t` overflow using either GCC/Clang builtins (`__builtin_add_overflow` etc.) or a portable manual fallback for MSVC. Overflow raises `EXERR_OVERFLOW` which is reported as `"integer overflow"`.

---

## Hash Map

The hash map (`vel_map.c`) is used for both the function table (`vel->fnmap`) and per-environment variable maps (`env->varmap`).

### Structure

1024 buckets (power-of-two, `MAP_MASK = 0x3FF`). Each bucket is a dynamic array of `map_entry_t { char *key; void *val; }`.

### Hash function

djb2: `h = ((h << 5) + h) + c` for each byte of the key. Fast and distributes well for short identifier strings.

### Operations

- `vmap_set(m, key, val)`: insert or update. Passing `val = NULL` deletes the entry by shifting the bucket array.
- `vmap_get(m, key)`: linear scan within a bucket; returns `NULL` if not found.
- `vmap_has(m, key)`: same as `vmap_get` but returns an int.

The choice of 1024 buckets was a deliberate fix from the original 256-bucket design to reduce average chain length for interpreters with 100+ registered functions.

---

## Environment and Scope

Environments form a singly-linked chain: each `vel_env_t` points to its `parent`. The interpreter's `vel->env` points to the current (innermost) environment; `vel->root_env` is the global scope.

Variable lookup (in `mem_find_var`) walks from `vel->env` toward `vel->root_env`, using the per-environment `varmap` hash map for O(1) lookup in each scope.

`vel_env_push` / `vel_env_pop` manage the stack for function calls and `jaileval`.

---

## Function Table

Functions are stored in a dynamic array `vel->fn` (size `vel->fn_count`) and also indexed in `vel->fnmap` for O(1) name lookup.

`vel->fn_sys` records the number of built-in functions registered during `vel_new`. This allows distinguishing built-ins from user-defined functions for potential future features.

`mem_add_fn` creates a new `vel_fn_t`, appends it to the array, and inserts it into the hash map.

`mem_del_fn` removes a function from both the array and the hash map.

---

## Call Stack and Error Reporting

Vel maintains a fixed-size call stack (`vel->stack_names[VEL_STACK_MAX]`, `vel->stack_pos[VEL_STACK_MAX]`) with a depth counter `vel->stack_depth`.

Each time a scripted function is called in `vel_parse`, the function name and call-site position are pushed onto the stack. On return, they are popped.

`vel_stack_trace` formats the stack into a human-readable string for error messages:

```
  in add (pos 42)
  in calculate (pos 107)
```

Stack frames store pointers into function name strings owned by `vel_fn_t`; no copy is made. This is safe because functions persist for the lifetime of the interpreter.

The cap of `VEL_STACK_MAX = 256` is set well below the 1000-frame recursion limit so that stack trace arrays never overflow even in deep recursion.

---

## Job Control Subsystem

The job control subsystem (`vel_jobs.c`, `vel_jobs.h`) provides POSIX background job management.

### Job table

A global array `g_jobs[VEL_MAX_JOBS]` (max 256 jobs) of `vel_job_t`:

```c
typedef struct vel_job_s {
    int    jid;        /* 1-based job ID */
    pid_t  pid;
    int    state;      /* VJOB_RUNNING / VJOB_STOPPED / VJOB_DONE */
    int    exit_code;
    char  *cmd;        /* display string */
    int    pgid;       /* process group */
} vel_job_t;
```

### Signal safety

Signal handlers (for `SIGCHLD`, user-defined signals) only set a `volatile sig_atomic_t` flag. The actual vel code registered via `sighandle` is executed from `vel_jobs_dispatch_signals`, which is called from the main REPL loop and from `run_stdin`/`run_file` after each script execution. This ensures vel code never runs from a signal handler context.

### Pipeline implementation

`cmd_pipe` in `vel_sys.c` creates a chain of `pipe(2)` file descriptors, forks one process per stage, and chains them together using `dup2`. The final stage's output is captured or written to stdout depending on context.

### `run_system` timeout

The `system` command in `main.c` uses `SIGALRM` with a configurable timeout (default 30 seconds). The implementation uses `sigprocmask` to prevent a race condition where an alarm pending from a previous call could kill the wrong child process.

---

## Template Engine

The template engine (`vel_tmpl.c`) preprocesses a source string into a vel script:

1. Text outside `<?vel ... ?>` tags is transformed into `write "..."` statements with `{` and `}` escaped as `\o` and `\c` respectively.
2. Code inside `<?vel ... ?>` tags is emitted verbatim.
3. The resulting script is executed with `vel->cb[VEL_CB_WRITE]` temporarily replaced by a function that appends output to `vel->tmpl_buf`.

After execution, `vel->tmpl_buf` (and its saved predecessor) are restored, and the accumulated buffer is returned.

Nested template calls are safe because the engine saves and restores `tmpl_buf`, `tmpl_len`, and the write callback.

---

## Extended Built-in Commands

`vel_newcmds.c` registers a second batch of built-in commands via `register_new_builtins(vel_t vel)`, which must be called from the interpreter initialization sequence (e.g., in `vel_new` or `main`). These commands extend or override commands registered by `vel_cmd.c`.

### List utilities

| Command | Signature | Description |
|---------|-----------|-------------|
| `lreverse` | `lreverse list` | Returns the list with elements in reverse order. |
| `lsort` | `lsort list` | Returns the list sorted lexicographically (ascending). Uses `qsort` with `strcmp`. |
| `luniq` | `luniq list` | Returns the list with consecutive duplicate elements removed. Requires a pre-sorted list for full deduplication. |
| `lassign` | `lassign list var1 var2 ... [*rest]` | Assigns list elements to named variables. The last variable may be prefixed with `*` to collect all remaining elements as a sublist (splat). Variables with no corresponding list element are set to the empty string. |

### Math helpers

| Command | Signature | Description |
|---------|-----------|-------------|
| `abs` | `abs n` | Absolute value. Returns integer if `n` is an integer, float otherwise. |
| `max` | `max a b` | Returns the larger of two values. |
| `min` | `min a b` | Returns the smaller of two values. |
| `math` | `math subcommand [args]` | Dispatcher for mathematical functions (see below). |

**`math` subcommands:**

| Subcommand | Args | Description |
|------------|------|-------------|
| `pi` | — | Returns π as a float. |
| `e` | — | Returns Euler's number as a float. |
| `sin cos tan` | `x` | Trigonometric functions (radians). |
| `asin acos atan` | `x` | Inverse trigonometric functions. |
| `atan2` | `y x` | Two-argument arctangent. |
| `sqrt` | `x` | Square root. |
| `pow` | `base exp` | Exponentiation. |
| `log` | `x` | Natural logarithm. |
| `log2` | `x` | Base-2 logarithm. |
| `log10` | `x` | Base-10 logarithm. |
| `abs` | `x` | Absolute value (float path). |
| `floor ceil round` | `x` | Rounding functions. |

All `math` results are returned as integer if the result has no fractional part, float otherwise (via `smart_num`).

### String extras

`cmd_string` overrides (or supplements) any existing `string` command registered by `vel_cmd.c`.

| Subcommand | Signature | Description |
|------------|-----------|-------------|
| `repeat` | `string repeat str n` | Returns `str` concatenated `n` times. Returns empty if `n` is 0 or `str` is empty. |
| `reverse` | `string reverse str` | Returns the byte-reversed string. (Note: not Unicode-aware.) |
| `is` | `string is type str` | Type test. Returns 1 if all characters of `str` match `type`, empty otherwise. Types: `integer`, `double`, `alpha`, `alnum`, `space`, `upper`, `lower`, `print`, `ascii`. |
| `map` | `string map pairs str` | Multi-pair substitution. `pairs` is a flat list `{old1 new1 old2 new2 ...}`. Scans `str` left-to-right; first matching pair wins. Error if `pairs` has an odd count. |

### Clock

`clock [unit]` returns the current wall-clock time using `clock_gettime(CLOCK_REALTIME)`.

| Unit | Return type | Value |
|------|-------------|-------|
| `s` (default) | float | Seconds since the Unix epoch, with nanosecond precision in the fractional part. |
| `ms` | integer | Milliseconds since the Unix epoch. |
| `us` | integer | Microseconds since the Unix epoch. |
| `ns` | integer | Nanoseconds since the Unix epoch. |

### Dict (flat key-value list)

`dict` implements an associative array stored as a flat list `{k1 v1 k2 v2 ...}`. This is the same format as Tcl dicts. All subcommands that modify a dict take a variable name and update the variable in place.

| Subcommand | Signature | Description |
|------------|-----------|-------------|
| `set` | `dict set varname key value` | Sets `key` to `value` in the dict stored in `varname`. Updates in place if the key already exists; appends a new pair otherwise. |
| `get` | `dict get dict key` | Returns the value for `key`, or empty if not found. |
| `exists` | `dict exists dict key` | Returns 1 if `key` is present, empty otherwise. |
| `unset` | `dict unset varname key` | Removes the `key`/value pair from the dict in `varname`. No-op if the key is absent. |
| `keys` | `dict keys dict` | Returns a list of all keys. |
| `values` | `dict values dict` | Returns a list of all values (in insertion order). |
| `size` | `dict size dict` | Returns the number of key-value pairs. |
| `for` | `dict for {kvar vvar} dict body` | Iterates over the dict, binding each key and value to `kvar` and `vvar` respectively, and executing `body`. Supports `break` and `return`. |

**Internal lookup:** `dict_find` does a linear scan of the flat list. This is O(n) in the number of entries, which is acceptable for small to medium dicts (< ~100 entries). For performance-critical large associative arrays, a `vel_map_t` structure would be more appropriate.

### Base64

Pure-C implementation with no external library dependency. Uses the standard alphabet (`A-Z a-z 0-9 + /`) with `=` padding.

| Subcommand | Signature | Description |
|------------|-----------|-------------|
| `encode` | `base64 encode str` | Encodes the byte string `str` to Base64. |
| `decode` | `base64 decode str` | Decodes a Base64 string. Returns an error if the input contains invalid characters. |

Note: `base64 decode` treats the input as a raw binary string. The result is a byte string that may contain NUL bytes. Since vel values are NUL-terminated C strings, embedded NULs will truncate the result when passed to C string functions.

### upvar

`upvar localname parentname` creates a local variable `localname` that mirrors the variable `parentname` in the parent (calling) scope.

**Mechanism:** `upvar` reads the current value of `parentname` from the parent environment and creates a local variable `localname` initialized to that value. It then installs a `watch` callback on `localname` that writes the new value back to `parentname` whenever `localname` is assigned.

The write-back strategy depends on where `parentname` lives:
- If `parentname` is in `root_env`: the watch runs `set global parentname $localname`.
- Otherwise: the watch runs `upeval { set parentname $localname }`.

```vel
func bump {} {
    upvar n counter   ;# n mirrors caller's counter
    inc n             ;# writes back to counter in caller scope
}

set counter 0
bump
write $counter   ;# outputs 1
```

**Limitation:** The write-back is triggered by the `watch` mechanism, which fires on each assignment to `localname`. Only the immediate parent frame is supported by the `upeval` path; deeper nesting may not behave as expected.

### Reflect extensions

`vel_newcmds.c` extends the existing `reflect` command by registering the original implementation under the alias `__reflect_orig` and installing a wrapper `cmd_reflect_ext` as the new `reflect`.

Two new subcommands are added:

| Subcommand | Description |
|------------|-------------|
| `reflect level` | Returns the current call stack depth (`vel->stack_depth`) as an integer. |
| `reflect depth` | Returns the current recursion depth (`vel->depth`) as an integer. |

All other `reflect` subcommands are forwarded transparently to `__reflect_orig`.

### try...finally

`vel_newcmds.c` replaces the simpler `try` from `vel_cmd.c` with a full `try...finally` implementation.

**Syntax (all forms):**

```vel
try body
try body errvar catch_body
try body finally fin_body
try body errvar catch_body finally fin_body
```

**Semantics:**

- `body` is executed.
- If an error occurs and `catch_body` is provided, the error message is bound to `errvar` and `catch_body` is executed.
- `fin_body` (if present) **always** runs, even if `body` or `catch_body` used `return`, `break`, or raised an uncaught error.
- After `fin_body`, the original error state, stop signal, and return value are restored. `fin_body`'s own side effects do not override the state from `body`/`catch_body`.

**Implementation notes:**

- The `stop` and `retval` fields of `vel->env` are saved before executing `body` and restored carefully after `fin_body`.
- `fin_body` runs with `stop = 0`, `retval_set = 0`, and `err_code = 0` so that any `return`/`break` inside `fin_body` is isolated.
- If both `body` and `fin_body` produce errors, the error from `body` takes precedence.

---

## Notable Bug Fixes

The codebase contains explicit comments documenting resolved bugs. They are summarized here for reference.

| Tag | Description |
|-----|-------------|
| BUG 1 FIX | `g_run_child_pid` changed from `volatile pid_t` to `volatile sig_atomic_t` to avoid undefined behavior when the signal fires during a non-atomic store. |
| BUG 3 FIX | Background jobs spawned from script files and stdin now have `vel_jobs_reap()` called after the script completes, preventing zombie processes. |
| BUG 4 FIX | `gc_data_alloc` failure now returns an empty value rather than NULL, preventing downstream null-pointer dereferences. |
| BUG 5 FIX | Error position is captured before `lex_tokenize()` advances the cursor, so error messages now point to the command name rather than the character after it. |
| BUG 6 FIX | Signed integer overflow in the expression evaluator is now detected for `+`, `-`, and `*` using compiler builtins or a portable fallback, and raises an explicit error instead of invoking undefined behavior. |
| FIX 1 | Hash map bucket count increased from 256 to 1024 to reduce collision chains for large function tables. |
| FIX 2 | `vmap_get` / `vmap_has` use an explicit early return for clarity and to make O(1) intent unambiguous. |
| FIX 4 | `run_system` (the `system` command) gained a `SIGALRM`-based timeout with a `sigprocmask` race-condition guard. |
| FIX 5 | `count_depth` in the REPL now correctly skips brace/bracket characters inside block comments, preventing the REPL from hanging waiting for a closing brace that is inside a comment. |
| FIX SWITCH | `cmd_switch` had two bugs: (1) the matched body was returned as a raw string via `vel_val_clone` instead of being executed; (2) bare-word patterns in flat syntax were evaluated as commands before reaching `cmd_switch`. Both are fixed: bodies are now executed with `vel_parse_val`, and a Tcl-style single-block syntax is added so patterns are always treated as literal strings. |

---

## Compile-Time Configuration

| Macro | Default | Description |
|-------|---------|-------------|
| `VEL_MAX_DEPTH` | 1000 | Maximum recursion depth before a controlled error |
| `VEL_MAX_CATCH_DEPTH` | 16384 | Maximum catcher invocation depth |
| `VEL_STACK_MAX` | 256 | Maximum call stack frames for error traces |
| `VEL_UNUSED_NAME_MAX` | 10000 | Maximum iterations when generating a unique name |
| `MAP_BUCKETS` | 1024 | Hash map bucket count (must be power of two) |
| `VEL_SYSTEM_TIMEOUT_SEC` | 30 | Default timeout for the `system` command (seconds) |
| `VEL_USE_READLINE` | undefined | Enable GNU readline in the REPL |
| `VELDLL` | undefined | Build as a Windows DLL with `__declspec(dllexport)` |

`VEL_SYSTEM_TIMEOUT` can also be set as a process environment variable at runtime to override the compiled-in default. Setting it to `0` disables the timeout.

---

## Platform Notes

- The code is written in C99 with POSIX.1-2008 extensions enabled via feature-test macros.
- Job control, pipelines, signal handling, `glob`, `chmod`, `chown`, `ln`, and several other features are conditionally compiled out on `WIN32`.
- MSVC compatibility is partially supported: `atoll` is mapped to `_atoi64`, some warnings are suppressed, and the integer-overflow fallback path in `vel_expr.c` is used instead of GCC/Clang builtins.
- The `WATCOMC` preprocessor symbol disables the `run_system` timeout and related features for Watcom C compatibility.
- `vel_newcmds.c` uses `clock_gettime(CLOCK_REALTIME)` for the `clock` command; this is POSIX.1-2008 and not available on Windows without a compatibility layer.
