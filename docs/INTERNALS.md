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
15. [Compile-Time Configuration](#compile-time-configuration)
16. [Platform Notes](#platform-notes)

---

## Source File Overview

### Project Layout

```
vel/
├── include/          # Public and internal header files
│   ├── vel.h
│   ├── vel_priv.h
│   └── vel_jobs.h
├── src/              # C implementation files
│   ├── main.c
│   ├── vel_run.c
│   ├── vel_lex.c
│   ├── vel_cmd.c
│   ├── vel_sys.c
│   ├── vel_extra.c
│   ├── vel_jobs.c
│   ├── vel_expr.c
│   ├── vel_mem.c
│   ├── vel_map.c
│   ├── vel_tmpl.c
│   └── vel_newcmds.c
├── docs/             # Documentation
├── tests/            # Test scripts
├── Makefile
├── LICENSE
└── README.md
```

### File Descriptions

| File | Responsibility |
|------|----------------|
| `include/vel.h` | Public API declarations and opaque types |
| `include/vel_priv.h` | Internal struct definitions and declarations |
| `include/vel_jobs.h` | Job control types and declarations |
| `src/main.c` | REPL, file runner, stdin runner, native commands (`system`, `readline`, `writechar`, `canread`) |
| `src/vel_run.c` | Core execution loop (`vel_parse`), error API, callback API, public lifecycle API |
| `src/vel_lex.c` | Lexer: whitespace skipping, token reading, heredoc, line tokenization |
| `src/vel_cmd.c` | Built-in language commands (control flow, strings, lists, functions, I/O, eval, scan) |
| `src/vel_sys.c` | System and filesystem commands; `run`, `exec`, `pipe`, `redirect`, `file`, `writefile`, `readfile` |
| `src/vel_extra.c` | Shell-utility commands: `ls`, `tree`, `cat`, `grep`, `wc`, `head`, `tail`, `stat`, etc. |
| `src/vel_jobs.c` | POSIX job control: `spawn`, `wait`, `fg`, `jobs`, `sighandle`, pipeline management |
| `src/vel_expr.c` | Arithmetic/logical expression evaluator |
| `src/vel_mem.c` | Value, list, environment, variable, and function allocation/lifecycle |
| `src/vel_map.c` | Hash map used for function and variable lookup |
| `src/vel_tmpl.c` | Template engine: `<?vel ... ?>` processing |
| `src/vel_newcmds.c` | Extended built-in commands: list utilities, math, string extras, clock, dict, base64, upvar, reflect extensions, `try...finally` |

---

## Build System

The Makefile compiles all `.c` files in `src/` to object files and links them together. Every object file depends on the headers in `include/` to ensure recompilation on any header change.

Flags:
- `-std=c11` — C11 is the language standard.
- `-D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L` — enables POSIX.1-2008 APIs on glibc.
- `-lm` — links `libm` for `fmod` in the expression evaluator and math subcommands.

Optional:
- `READLINE=1` defines `VEL_USE_READLINE` and links `-lreadline` for the REPL.

---

## Core Data Structures

### `struct vel_s` (the interpreter, `vel_t`)

The central interpreter state. Key fields:

- `src`, `src_len`, `pos`: lexer cursor into the current source string.
- `root_src`, `root_src_buf`: pointer to and owned copy of the original script.
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

`data` points into a heap block with a hidden `size_t` refcount stored immediately before the string bytes.

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

Each bucket holds an array of `map_entry_t` (key-value pairs). The hash function is djb2.

---

## Memory Management and GC

Vel uses reference-counted string data with copy-on-write semantics (implemented in `vel_mem.c`).

### Hidden-prefix refcount layout

```
[ size_t refcount | char data[len+1] ]
```

The `vel_val_t.data` pointer points to the start of the `char` area. The refcount is accessed via:

```c
#define GC_RC(data)  (*(size_t *)((char *)(data) - sizeof(size_t)))
```

### Reference counting

- `val_make_len` / `val_make` allocate a fresh buffer with `refcount = 1`.
- `vel_val_clone` increments the refcount and shares the buffer: O(1) clone.
- `vel_val_free` decrements the refcount; frees the buffer when it reaches 0.

### Copy-on-write (COW)

Before any mutation (`vel_val_cat_*` functions), `gc_cow` checks whether `refcount > 1`. If so, it makes a private copy before proceeding.

---

## Lexer

The lexer is in `vel_lex.c` and operates by reading characters from `vel->src` starting at `vel->pos`.

### `lex_skip_ws(vel_t vel)`

Advances past whitespace, line comments (`# ...`), block comments (`## ... ##`), and line continuations.

### `lex_next_token(vel_t vel)`

Reads and returns the next token as a `vel_val_t`. Token forms:

1. `$name` — looks up the variable and returns its value.
2. `{text}` — reads brace-quoted literal text (no substitution).
3. `[code]` — reads bracket-quoted code, calls `vel_parse` recursively, returns the result.
4. `"text"` or `'text'` — quoted string with substitution and escape processing.
5. Bare word — reads until whitespace or a special character.

Heredoc (`<<DELIM`) is handled as a special case in the bare-word path.

### `lex_tokenize(vel_t vel)`

Reads all tokens on the current line and returns them as a `vel_list_t`.

---

## Execution Engine

The execution engine is in `vel_run.c`.

### `vel_parse`

The main execution loop:

1. Saves and restores lexer state for nested calls.
2. Records `root_src` on the first call for error messages.
3. Increments `depth`; errors if `depth > VEL_MAX_DEPTH`.
4. Loops: calls `lex_tokenize` to get the next statement, then dispatches.

### Command dispatch

1. The first word is the command name.
2. `mem_find_fn` looks it up in the function hash map.
3. If found as a native function, calls it directly.
4. If found as a scripted function, pushes a new environment, binds parameters, calls `vel_parse` on the body.
5. If not found, tries the `catcher` if set, then attempts auto-exec.

### Auto-exec (Unix only)

If a command name is not in the function table but is found on `PATH`, vel forks and execs it with inherited stdio. The exit code is stored in `g_last_exit` and the `?` variable.

### Stop propagation

The `env->stop` flag is used by `break`, `continue`, and `return` to abort the current execution loop without entering error state.

---

## Expression Evaluator

The expression evaluator is in `vel_expr.c`. It operates on a flat string using a recursive descent parser.

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
mul     := unary ( ('**' | '*' | '/' | '\' | '%') unary )*
unary   := ('-' | '+' | '~' | '!') unary | paren
paren   := '(' expr ')' | atom
atom    := number | non-numeric (truthy)
```

The `**` operator (power/exponentiation) is handled before single `*` in the mul level to ensure correct tokenization.

### Type system

The evaluator tracks whether each operand is integer (`EXPR_INT`) or float (`EXPR_FLT`). Integer division (`\`) always returns integer. Float division (`/`) always returns float.

### Overflow detection

Integer `+`, `-`, `*` check for `int64_t` overflow using GCC/Clang builtins or a portable manual fallback. Overflow raises an error.

---

## Hash Map

The hash map (`vel_map.c`) is used for the function table and per-environment variable maps.

### Structure

1024 buckets (power-of-two, `MAP_MASK = 0x3FF`). Each bucket is a dynamic array of `map_entry_t { char *key; void *val; }`.

### Hash function

djb2: `h = ((h << 5) + h) + c` for each byte of the key.

### Operations

- `vmap_set(m, key, val)`: insert or update. `val = NULL` deletes the entry.
- `vmap_get(m, key)`: linear scan within a bucket; returns `NULL` if not found.
- `vmap_has(m, key)`: returns an int.

---

## Environment and Scope

Environments form a singly-linked chain. `vel->env` points to the current environment; `vel->root_env` is the global scope.

Variable lookup walks from `vel->env` toward `vel->root_env`, using per-environment `varmap` hash maps for O(1) lookup per scope.

`vel_env_push` / `vel_env_pop` manage the stack for function calls and `jaileval`.

---

## Function Table

Functions are stored in a dynamic array `vel->fn` and indexed in `vel->fnmap` for O(1) name lookup.

`vel->fn_sys` records the number of built-in functions registered during initialization, distinguishing built-ins from user-defined functions.

`mem_add_fn` creates a new `vel_fn_t`, appends it to the array, and inserts it into the hash map.

`mem_del_fn` removes a function from both the array and the hash map.

---

## Call Stack and Error Reporting

Vel maintains a fixed-size call stack (`vel->stack_names[VEL_STACK_MAX]`, `vel->stack_pos[VEL_STACK_MAX]`) with depth counter `vel->stack_depth`.

Each time a scripted function is called, the function name and call-site position are pushed. On return, they are popped.

`vel_stack_trace` formats the stack into a human-readable string for error messages:

```
  in add (pos 42)
  in calculate (pos 107)
```

The cap of `VEL_STACK_MAX = 256` is set well below the 1000-frame recursion limit.

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

Signal handlers only set a `volatile sig_atomic_t` flag. Vel code registered via `sighandle` is executed from `vel_jobs_dispatch_signals`, called from the main loop after each command.

### Pipeline implementation

`cmd_pipe` in `vel_sys.c` creates a chain of `pipe(2)` file descriptors, forks one process per stage, and chains them using `dup2`. When all arguments are single tokens (no spaces), they are treated as one command with arguments (exec-style).

### `run_system` timeout

The `system` command uses `SIGALRM` with a configurable timeout (default 30 seconds) and `sigprocmask` to prevent race conditions.

---

## Template Engine

The template engine (`vel_tmpl.c`) preprocesses a source string into a vel script:

1. Text outside `<?vel ... ?>` tags is transformed into `write "..."` statements with `{` and `}` escaped as `\o` and `\c`.
2. Code inside `<?vel ... ?>` tags is emitted verbatim.
3. The resulting script is executed with `vel->cb[VEL_CB_WRITE]` temporarily replaced by a function that appends output to `vel->tmpl_buf`.

Nested template calls are safe because the engine saves and restores `tmpl_buf`, `tmpl_len`, and the write callback.

---

## Extended Built-in Commands

`vel_newcmds.c` registers a second batch of built-in commands via `register_new_builtins(vel_t vel)`.

### List utilities

| Command | Signature | Description |
|---------|-----------|-------------|
| `lreverse` | `lreverse list` | Returns the list with elements in reverse order. |
| `lsort` | `lsort list` | Returns the list sorted lexicographically (ascending). |
| `luniq` | `luniq list` | Returns the list with consecutive duplicate elements removed. |
| `lassign` | `lassign list var1 var2 ... [*rest]` | Assigns list elements to named variables. Last variable may have `*` prefix to collect remaining elements (splat). |

Additional list commands in `vel_cmd.c`:

| Command | Description |
|---------|-------------|
| `lappend varname item ...` | Appends items to the list in the named variable. |
| `linsert list idx val ...` | Returns a new list with values inserted before `idx`. |
| `lreplace list first last ?val ...?` | Replaces elements `first..last` with `val ...`. |
| `llength list` | Alias for `count`. |
| `lindex list i` | Alias for `index`. |
| `lrange list first last` | Returns a sublist, inclusive both ends. |
| `lsearch list value` | Alias for `indexof`. |

### Math helpers

| Command | Signature | Description |
|---------|-----------|-------------|
| `abs` | `abs n` | Absolute value. |
| `max` | `max n1 n2 [n3 ...]` | Returns the largest of all given values. |
| `min` | `min n1 n2 [n3 ...]` | Returns the smallest of all given values. |
| `math` | `math subcommand [args]` | Dispatcher for mathematical functions. |

**`math` subcommands:** `pi`, `e`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `sqrt`, `pow`, `log`, `log2`, `log10`, `abs`, `floor`, `ceil`, `round`. All results are returned as integer if the result has no fractional part, float otherwise.

### String extras

`cmd_string` overrides any existing `string` command and provides the following subcommands:

| Subcommand | Signature | Description |
|------------|-----------|-------------|
| `length` | `string length str` | Byte length of `str`. |
| `toupper` | `string toupper str` | Uppercase conversion. |
| `tolower` | `string tolower str` | Lowercase conversion. |
| `index` | `string index str idx` | Character at zero-based `idx`. |
| `range` | `string range str first last` | Substring from `first` to `last` (inclusive). |
| `first` | `string first needle hay [start]` | First occurrence index of `needle` in `hay`, or -1. |
| `last` | `string last needle hay` | Last occurrence index of `needle`, or -1. |
| `replace` | `string replace str first last [newstr]` | Replace characters `first..last` with `newstr`. |
| `trim` | `string trim str [chars]` | Trim characters from both ends. |
| `trimleft` | `string trimleft str [chars]` | Trim from the left. |
| `trimright` | `string trimright str [chars]` | Trim from the right. |
| `compare` | `string compare str1 str2` | Lexicographic comparison, returns negative/zero/positive. |
| `equal` | `string equal str1 str2` | Returns 1 if equal, 0 otherwise. |
| `repeat` | `string repeat str n` | Returns `str` concatenated `n` times. |
| `reverse` | `string reverse str` | Byte-reversed string (not Unicode-aware). |
| `is` | `string is type str` | Type test. Returns 1 (true) or 0 (false). Types: `integer`, `double`, `alpha`, `alnum`, `space`, `upper`, `lower`, `print`, `ascii`. |
| `map` | `string map pairs str` | Multi-pair substitution. `pairs` is `{old1 new1 old2 new2 ...}`. |

### Clock

`clock [unit]` returns the current wall-clock time using `clock_gettime(CLOCK_REALTIME)`.

| Unit | Return type | Value |
|------|-------------|-------|
| `s` (default) | float | Seconds since the Unix epoch |
| `ms` | integer | Milliseconds since the Unix epoch |
| `us` | integer | Microseconds since the Unix epoch |
| `ns` | integer | Nanoseconds since the Unix epoch |

### Dict (flat key-value list)

`dict` implements an associative array stored as a flat list `{k1 v1 k2 v2 ...}`.

| Subcommand | Signature | Description |
|------------|-----------|-------------|
| `set` | `dict set varname key value` | Sets key in dict variable. Updates in place if key exists; appends new pair otherwise. |
| `get` | `dict get dict key` | Returns the value for key, or empty if not found. |
| `exists` | `dict exists dict key` | Returns 1 if key is present, empty otherwise. |
| `unset` | `dict unset varname key` | Removes the key/value pair. No-op if absent. |
| `keys` | `dict keys dict` | Returns a list of all keys. |
| `values` | `dict values dict` | Returns a list of all values (insertion order). |
| `size` | `dict size dict` | Returns the number of key-value pairs. |
| `for` | `dict for {kvar vvar} dict body` | Iterates key-value pairs. Supports `break` and `return`. |

`dict_find` performs a linear scan of the flat list (O(n)). Acceptable for small to medium dicts.

### Base64

Pure-C implementation using the standard alphabet (`A-Z a-z 0-9 + /`) with `=` padding.

| Subcommand | Signature | Description |
|------------|-----------|-------------|
| `encode` | `base64 encode str` | Encodes to Base64. |
| `decode` | `base64 decode str` | Decodes from Base64. |

### upvar

`upvar localname parentname` creates a local variable that mirrors a variable in the parent scope.

The write-back strategy:
- If `parentname` is in `root_env`: the watch runs `set global parentname $localname`.
- Otherwise: the watch runs `upeval { set parentname $localname }`.

```vel
func bump {} {
    upvar n counter
    incr n
}
set counter 0
bump
write $counter   ;# 1
```

### Reflect extensions

`vel_newcmds.c` extends `reflect` by registering the original as `__reflect_orig` and installing a wrapper.

| Subcommand | Description |
|------------|-------------|
| `reflect level` | Returns the current call stack depth (`vel->stack_depth`). |
| `reflect depth` | Returns the current recursion depth (`vel->depth`). |

### try...finally

Replaces the simpler `try` from `vel_cmd.c` with full `try...finally` support.

```vel
try body
try body errvar catch_body
try body finally fin_body
try body errvar catch_body finally fin_body
```

`fin_body` always runs. After `fin_body`, the original error state, stop signal, and return value are restored. `fin_body`'s side effects do not override the outer control flow.

### scan

`scan string fmt var1 var2 ...` parses `string` according to scanf-style format specifiers and stores results in named variables. Returns the number of successfully converted items. Supported specifiers: `%d`, `%i`, `%f`, `%g`, `%e`, `%s`.

### File I/O helpers

`writefile path data` writes `data` to `path`. `readfile path` reads a file and returns its contents as a string. Both are thin wrappers over the standard `store`/`read` mechanism.

### file command

`file subcommand path` provides unified file information: `exists`, `isfile`, `isdir`, `size`, `extension`, `tail`, `dir`, `readable`, `writable`, `executable`, `mtime`, `join`.

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

`VEL_SYSTEM_TIMEOUT` can be set as a process environment variable at runtime to override the compiled-in default. Setting it to `0` disables the timeout.

---

## Platform Notes

- The code is written in C11 with POSIX.1-2008 extensions enabled via feature-test macros.
- Job control, pipelines, signal handling, `glob`, `chmod`, `chown`, `ln`, and several other features are conditionally compiled out on `WIN32`.
- MSVC compatibility is partially supported: `atoll` is mapped to `_atoi64`, some warnings are suppressed.
- `vel_newcmds.c` uses `clock_gettime(CLOCK_REALTIME)` for the `clock` command; this requires POSIX.1-2008 and is not available on Windows without a compatibility layer.
