# Vel vs Tcl: Comparison and Gap Analysis

This document compares vel 0.1.0 against standard Tcl (specifically the Tcl 8.6 specification) and identifies features that vel has, features Tcl has that vel lacks, and features vel adds that Tcl does not have natively.

---

## Table of Contents

1. [Common Ground](#common-ground)
2. [Features Vel Has (Compared to Tcl)](#features-vel-has-compared-to-tcl)
3. [Tcl Features Missing from Vel](#tcl-features-missing-from-vel)
4. [Vel Features Not in Standard Tcl](#vel-features-not-in-standard-tcl)
5. [Syntax Differences](#syntax-differences)
6. [Behavioral Differences](#behavioral-differences)
7. [Summary Table](#summary-table)

---

## Common Ground

Vel and Tcl share the same fundamental execution model:

- Everything is a string.
- Every statement is a command invocation.
- The first word on a line is the command name.
- `{...}` quotes text literally (no substitution).
- `[...]` evaluates code and substitutes the result.
- `$name` expands a variable.
- Double-quoted strings allow `$` and `[...]` substitution.
- Lists are space-separated strings with brace-quoting.
- Functions are defined with parameters and a body.
- Lexical scoping with upward variable lookup.

---

## Features Vel Has (Compared to Tcl)

The following are features that exist in both vel and standard Tcl 8.6.

### Control Flow
- `if / else`
- `while`
- `for` (foreach-style over lists)
- `foreach` (with index-value two-variable form)
- `break`, `continue`, `return`
- `switch` (value matching with single-block or flat syntax)

### Functions
- `func` (equivalent to Tcl `proc`)
- Anonymous functions
- Variadic functions via `args`
- `rename` (equivalent to Tcl `rename`)

### String Operations
- `length`, `substr`, `strpos` (equivalent to Tcl `string length/range/first`)
- `toupper`, `tolower`
- `trim`, `ltrim`, `rtrim`
- `strcmp`, `streq`
- `startswith`, `endswith`
- `split` (list from string)
- `join` (string from list)
- `format` (printf-style)
- `repstr` (single-pair substitution)
- `string map` (multi-pair substitution — `vel_newcmds.c`)
- `string repeat` (`vel_newcmds.c`)
- `string reverse` (`vel_newcmds.c`)
- `string is` (type testing: `integer`, `double`, `alpha`, `alnum`, `space`, `upper`, `lower`, `print`, `ascii` — `vel_newcmds.c`)

### List Operations
- `count` (equivalent to Tcl `llength`)
- `index` (equivalent to Tcl `lindex`)
- `append` (equivalent to Tcl `lappend`)
- `slice` (equivalent to Tcl `lrange`)
- `filter` (equivalent to Tcl `lsearch -all` with predicate)
- `lmap` (equivalent to Tcl `lmap`)
- `list` (equivalent to Tcl `list`)
- `indexof` (equivalent to Tcl `lsearch`)
- `lreverse` (`vel_newcmds.c`)
- `lsort` (`vel_newcmds.c`)
- `luniq` (`vel_newcmds.c`)
- `lassign` with splat (`*rest`) (`vel_newcmds.c`)

### I/O
- `write`, `print`, `echo`
- `read`
- `source` (load and execute file)

### Expressions
- `expr` with full arithmetic, bitwise, comparison, and logical operators
- `inc`, `dec` (shorthand increment/decrement)
- `abs`, `max`, `min` (`vel_newcmds.c`)
- `math` — `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `sqrt`, `pow`, `log`, `log2`, `log10`, `floor`, `ceil`, `round`, `pi`, `e` (`vel_newcmds.c`)

### Dictionaries
- `dict set/get/exists/unset/keys/values/size/for` — flat key-value list format (`vel_newcmds.c`)

### Error Handling
- `try body [errvar handler] [finally fin]` — with optional `finally` clause (`vel_newcmds.c`)
- `error`

### Introspection
- `reflect` (covers Tcl `info` subset: version, funcs, vars, globals, has-func, has-var, args, body, this, name)
- `reflect level` — current call stack depth (`vel_newcmds.c`)
- `reflect depth` — current recursion depth (`vel_newcmds.c`)

### Variables
- `set`, `local` (equivalent to Tcl `set` and `variable`)
- `watch` (equivalent to Tcl `trace add variable`)
- `upvar localname parentname` — local alias for a parent-scope variable (`vel_newcmds.c`)

### Evaluation
- `eval`, `topeval`, `upeval`, `enveval`, `jaileval`
- `subst`, `quote`
- `catcher` (equivalent to Tcl `unknown`)

### Encoding/Clock
- `base64 encode/decode` — pure-C, no external library (`vel_newcmds.c`)
- `clock [s|ms|us|ns]` — wall-clock time with nanosecond resolution (`vel_newcmds.c`)

---

## Tcl Features Missing from Vel

### Namespaces

Tcl has a full namespace system (`namespace eval`, `namespace export`, `namespace import`, `::global::path::syntax`). Vel has no namespaces. All functions and global variables share a single flat namespace.

**Impact**: large projects with many functions will experience name collisions. Vel currently has no mitigation beyond naming conventions.

### Regular Expressions

Tcl has full POSIX and extended regular expression support via `regexp`, `regsub`, and pattern matching options on `string match`, `lsearch`, `switch`, etc.

Vel's `grep` command performs substring matching only. There is no `regexp` or `regsub` equivalent. The `switch` command matches by exact string equality; there is no glob or regex pattern mode (unlike Tcl's `-glob` and `-regexp` options).

**Impact**: text processing tasks that require pattern matching (log parsing, config parsing, validation) require either shelling out or manual character-by-character parsing.

### `string match` (glob patterns)

Tcl's `string match` performs glob-style pattern matching. Vel has no equivalent; `switch` matches only by exact equality.

### `info` Subcommand Suite

Tcl's `info` command covers several cases vel does not:

| Tcl command | Status in vel |
|-------------|---------------|
| `info commands` | Equivalent: `reflect funcs` |
| `info procs` | Partial: `reflect funcs` includes all functions |
| `info vars` | Equivalent: `reflect vars` |
| `info globals` | Equivalent: `reflect globals` |
| `info args` | Equivalent: `reflect args` |
| `info body` | Equivalent: `reflect body` |
| `info exists` | Equivalent: `reflect has-var` |
| `info level` | `reflect level` (call stack depth) |
| `info frame` | Not present (detailed frame inspection) |
| `info hostname` | Present separately as `hostname` command |
| `info script` | Not present (current script filename) |
| `info nameofexecutable` | Not present |
| `info tclversion` | Equivalent: `reflect version` (vel version) |
| `info loaded` | Not present (no package/extension system) |

### Package and Extension System

Tcl has `package require`, `package provide`, and a loadable extension mechanism (`load`, shared libraries, `Tcl_CreateCommand` from C). Vel has no package system. The C embedding API (`vel_register`) allows adding native commands, but there is no dynamic loading or version management.

### Channels (I/O System)

Tcl has a generic channel abstraction (`open`, `close`, `gets`, `puts`, `read`, `seek`, `tell`, `eof`, `flush`, `fconfigure`, `fileevent`). This covers:

- File I/O with buffering, seeking, and binary mode.
- TCP/UDP socket channels (`socket`).
- Pipe channels (`open "| cmd"` for bidirectional IPC).
- Non-blocking I/O with event-driven callbacks.

Vel has:
- `cat`, `head`, `tail` for reading files (no seeking, no binary mode).
- `read` for stdin only.
- `redirect`, `redirect_in` for redirecting file descriptors to external processes.
- No socket commands.
- No non-blocking I/O.
- No `fconfigure` equivalent.

**Impact**: vel cannot do network I/O, cannot read binary files reliably, and cannot do bidirectional IPC with child processes from within the scripting layer.

### Event Loop (`vwait`, `after`, `fileevent`)

Tcl has an event loop (`vwait`, `after`, `after cancel`, `fileevent`) for asynchronous programming. Vel has no event loop. Concurrency is limited to background jobs (`spawn`/`bg`) with blocking `wait`.

### `uplevel N`

Tcl's `uplevel` evaluates code in a caller's scope by numeric level. Vel's `upeval` evaluates in the immediate parent scope only; there is no numeric level argument.

### `dict merge`, `dict with`, `dict filter`, `dict update`

Vel's `dict` subcommand covers the most common operations (`set`, `get`, `exists`, `unset`, `keys`, `values`, `size`, `for`). The following Tcl dict subcommands are not present:

- `dict merge` — merge multiple dicts into one.
- `dict with` — execute a body with dict keys expanded as local variables.
- `dict filter` — filter entries by key, value, or script predicate.
- `dict update` — update multiple keys atomically.

### `apply` (Lambda Application)

Tcl 8.5+ has `apply {{params body} args}` for applying an anonymous function inline without registering it by name. Vel's anonymous functions (`func {params} {body}`) do register a name (generated internally); there is no truly nameless application.

### `coroutine` and `yield`

Tcl 8.6 has stackful coroutines (`coroutine`, `yield`, `yieldto`). Vel has no coroutine support.

### Object Systems

Tcl has TclOO (built-in object system since 8.6) with classes, instances, inheritance, and method dispatch. Vel has no object system.

### `tailcall`

Tcl 8.6 has `tailcall` for tail-call optimization. Vel does not. Deep tail-recursive functions in vel will exhaust the recursion limit (1000 frames).

### `encoding` and Unicode

Tcl has comprehensive Unicode and encoding support (`encoding names`, `encoding convertto`, `encoding convertfrom`, `string length` counts characters not bytes). Vel's string operations are byte-oriented and have no explicit encoding support.

### `clock format` / `clock scan` / `clock add`

Tcl's `clock` subsystem provides date formatting (`clock format`), date parsing (`clock scan`), and date arithmetic (`clock add`). Vel's `clock` command returns raw timestamps (seconds, milliseconds, microseconds, nanoseconds) and its `date` command wraps `strftime` for formatting only. Date parsing and arithmetic are not available.

### `binary` Commands

Tcl has `binary format` and `binary scan` for packing and unpacking binary data. Vel has no equivalent.

### `trace` Command (read and unset)

Tcl's `trace` command allows monitoring variable reads, writes, and unsets, and command execution, at the language level. Vel's `watch` covers variable write traces on assignment only; read traces and unset traces are not supported.

---

## Vel Features Not in Standard Tcl

### Integrated Shell Commands

Vel includes a large set of Unix shell-utility commands as built-ins: `ls`, `tree`, `find`, `cat`, `head`, `tail`, `wc`, `grep`, `touch`, `stat`, `mkdir`, `rmdir`, `remove`, `copy`, `move`, `chmod`, `chown`, `ln`, `glob`, `cd`, `pwd`, `which`, `env`, `whoami`, `hostname`, `uname`, `uptime`, `df`, `seq`, `clear`, `yes`, `tee`, `echo`, `printf`.

Standard Tcl provides equivalent functionality through the `file` command family and `exec`, but does not implement these as first-class commands.

### POSIX Job Control

Vel has `spawn`/`bg`, `wait`, `waitall`, `fg`, `jobs`, `killjob`, `jobstatus`, `bglist`, `bgpid`, `sighandle`, and `shpipe` for shell-style job control. Standard Tcl's `exec` is synchronous; background execution requires the `&` suffix and lacks vel's job table.

### Auto-Execution of External Programs

In vel, any unrecognized command that is found on `PATH` is automatically executed as an external program with inherited stdio. This behavior is similar to a Unix shell. Tcl requires explicit `exec` for external commands.

### Template Engine

Vel has a built-in `vel_template` C API and `<?vel ... ?>` tag syntax for embedding vel code in text documents. Tcl does not have a built-in template engine.

### Integer Overflow Detection

Vel detects and reports `int64_t` overflow in `+`, `-`, and `*` operations in the expression evaluator. Standard Tcl silently wraps or promotes to wide integers.

### `redirect`, `redirect2`, `redirect_in`, `pipe`

Vel provides direct control over file descriptor redirection for external processes through first-class language commands. Tcl uses `exec` with redirection syntax embedded in the argument list (e.g., `exec cmd < infile > outfile`).

### High-Resolution Clock

Vel's `clock [ms|us|ns]` returns integer timestamps at millisecond, microsecond, or nanosecond resolution using `clock_gettime(CLOCK_REALTIME)`. Tcl's `clock milliseconds` and `clock microseconds` cover milliseconds and microseconds but not nanoseconds.

### Base64

Vel has `base64 encode` and `base64 decode` as first-class language commands with no external library dependency. Tcl provides Base64 only through the `base64` package (not a built-in).

### `try...finally`

Vel's `try` supports a `finally` clause that is guaranteed to run regardless of errors, `return`, or `break` in the body. Tcl's `catch` does not have a native `finally` equivalent (it requires manual `finally` simulation with additional `catch` nesting).

---

## Syntax Differences

| Feature | Tcl | Vel |
|---------|-----|-----|
| Function definition | `proc name {params} {body}` | `func name {params} {body}` |
| Variable assignment | `set name value` | `set name value` (same) |
| List foreach | `foreach var list {body}` | `for var list {body}` or `foreach var list {body}` |
| Error handling | `catch {body} var` | `try {body} var {handler} [finally {fin}]` |
| Introspection | `info ...` | `reflect ...` |
| Increment | `incr var` | `inc var` |
| Decrement | no built-in | `dec var` |
| Block comment | no built-in | `## text ##` |
| Integer division | `expr {$a / $b}` (int if both int) | `expr $a \ $b` |
| List unpack | `lassign list v1 v2` | `lassign list v1 v2 *rest` (splat supported) |

---

## Behavioral Differences

### Variable scoping on `set`

In Tcl, `set` inside a procedure creates a local variable unless `global` or `upvar` is used. In vel, `set` walks up the scope chain and modifies the nearest existing binding, creating a new local only if no binding is found. This is closer to how many scripting languages work but differs from Tcl's strict procedure-local default.

### List format

Both use the same basic packed-list format, but Tcl's list parsing is more precisely specified and handles a wider range of quoting edge cases. Vel's list packing (`vel_list_pack`) uses brace-quoting for items containing spaces or special characters.

### `write` vs `puts`

Tcl's `puts` always appends a newline. Vel's `write` does not; `print` and `echo` do.

### Expression syntax

Tcl requires expressions in braces for performance: `if {$x > 0}`. Vel evaluates the condition through the same substitution pipeline as any other argument; braces are used only for quoting, not for optimization.

### `dict` storage format

Tcl's `dict` type is a native ordered dictionary with O(1) lookup. Vel's `dict` is implemented as a flat list `{k1 v1 k2 v2 ...}` with O(n) linear search. The on-disk/serialization format is identical, but performance differs for large dicts.

---

## Summary Table

| Category | Vel | Tcl 8.6 |
|----------|-----|---------|
| Basic syntax (commands, substitution, quoting) | Yes | Yes |
| Arithmetic expressions | Yes | Yes |
| String operations (basic) | Yes | Yes |
| `switch` (value matching) | Yes — exact equality; single-block and flat syntax; `default` fallthrough | Yes — exact, `-glob`, and `-regexp` modes |
| Regular expressions | No | Yes |
| `string map` (multi-pair) | Yes (`vel_newcmds.c`) | Yes |
| `string is` type testing | Yes (`vel_newcmds.c`) | Yes |
| `string repeat` / `string reverse` | Yes (`vel_newcmds.c`) | Yes |
| List operations | Yes | Yes |
| `lassign` (with splat) | Yes (`vel_newcmds.c`) | Yes (no splat) |
| `lsort` / `lreverse` / `luniq` | Yes (`vel_newcmds.c`) | Partial (`lsort` only natively) |
| Dictionaries (basic ops) | Yes — flat list format, O(n) (`vel_newcmds.c`) | Yes — native type, O(1) |
| `dict merge/with/filter/update` | No | Yes |
| Namespaces | No | Yes |
| File I/O (channels) | Partial (read-only, text) | Full |
| Sockets | No | Yes |
| Non-blocking I/O / event loop | No | Yes |
| Coroutines | No | Yes |
| OOP (TclOO) | No | Yes |
| `tailcall` | No | Yes |
| Unicode/encoding | No | Yes |
| Binary data | No | Yes |
| Package system | No | Yes |
| `uplevel N` | Partial (`upeval` — immediate parent only) | Full |
| `upvar` | Yes (`vel_newcmds.c`) | Yes |
| `apply` (nameless lambda) | Partial (registers a name) | Yes |
| `try...finally` | Yes (`vel_newcmds.c`) | No (requires manual simulation) |
| Math functions (`sin`, `cos`, `sqrt`, etc.) | Yes (`vel_newcmds.c`) | Via `expr` only |
| `reflect level` / `reflect depth` | Yes (`vel_newcmds.c`) | Partial (`info level`) |
| Base64 | Yes, built-in (`vel_newcmds.c`) | Via package only |
| High-resolution clock (ns) | Yes (`vel_newcmds.c`) | Up to µs |
| Job control (POSIX) | Yes | No (basic exec only) |
| Auto-exec from PATH | Yes | No |
| Shell utility commands | Yes (ls, grep, find, etc.) | No |
| Template engine | Yes | No |
| Integer overflow detection | Yes | No |
| `watch` (variable trace on write) | Yes | Yes (via `trace`) |
| Embedded C API | Yes | Yes |
