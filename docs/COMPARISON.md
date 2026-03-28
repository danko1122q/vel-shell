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
- `switch` (value matching)

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
- `repstr` (equivalent to Tcl `string map` for single substitution)

### List Operations
- `count` (equivalent to Tcl `llength`)
- `index` (equivalent to Tcl `lindex`)
- `append` (equivalent to Tcl `lappend`)
- `slice` (equivalent to Tcl `lrange`)
- `filter` (equivalent to Tcl `lsearch -all` with predicate)
- `lmap` (equivalent to Tcl `lmap`)
- `list` (equivalent to Tcl `list`)
- `indexof` (equivalent to Tcl `lsearch`)

### I/O
- `write`, `print`, `echo`
- `read`
- `source` (load and execute file)

### Expressions
- `expr` with full arithmetic, bitwise, comparison, and logical operators
- `inc`, `dec` (shorthand increment/decrement)

### Error Handling
- `try` / `error` (equivalent to Tcl `catch` / `error`)

### Introspection
- `reflect` (covers Tcl `info` subset: version, funcs, vars, globals, has-func, has-var, args, body, this, name)

### Variables
- `set`, `local` (equivalent to Tcl `set` and `variable`)
- `watch` (equivalent to Tcl `trace add variable`)

### Evaluation
- `eval`, `topeval`, `upeval`, `enveval`, `jaileval`
- `subst`, `quote`
- `catcher` (equivalent to Tcl `unknown`)

---

## Tcl Features Missing from Vel

### Namespaces

Tcl has a full namespace system (`namespace eval`, `namespace export`, `namespace import`, `::global::path::syntax`). Vel has no namespaces. All functions and global variables share a single flat namespace.

**Impact**: large projects with many functions will experience name collisions. Vel currently has no mitigation beyond naming conventions.

### Arrays and Dictionaries

Tcl has two-dimensional array variables (`array set`, `array get`, `$arr(key)`) and dictionaries (`dict create`, `dict get`, `dict set`, `dict for`, etc.).

Vel has only flat string values. There is no built-in associative data structure. Structured data must be encoded as lists or passed as separate variables.

**Impact**: writing data-processing scripts that require key-value lookup requires manual encoding (e.g., parallel lists or a naming convention like `set data_key value`).

### Regular Expressions

Tcl has full POSIX and extended regular expression support via `regexp`, `regsub`, and pattern matching options on `string match`, `lsearch`, `switch`, etc.

Vel's `grep` command performs substring matching only. There is no `regexp` or `regsub` equivalent. The `switch` command matches by exact equality; there is no glob or regex pattern mode.

**Impact**: text processing tasks that require pattern matching (log parsing, config parsing, validation) require either shelling out or manual character-by-character parsing.

### `string` Subcommand Suite

Tcl's `string` command has many subcommands that vel does not implement:

| Tcl command | Status in vel |
|-------------|---------------|
| `string match` (glob patterns) | Not present |
| `string map` (multi-pair substitution) | Not present (`repstr` handles only one pair) |
| `string repeat` | Not present |
| `string reverse` | Not present |
| `string is` (type testing: integer, double, alpha, etc.) | Not present |
| `string cat` | Equivalent: use `concat` |
| `string index` | Equivalent: `charat` |
| `string range` | Equivalent: `substr` |
| `string first` / `string last` | Equivalent: `strpos` (first only) |
| `string bytelength` | Equivalent: `length` |

### `info` Subcommand Suite

Tcl's `info` command covers many cases vel does not:

| Tcl command | Status in vel |
|-------------|---------------|
| `info commands` | Equivalent: `reflect funcs` |
| `info procs` | Partial: `reflect funcs` includes all functions |
| `info vars` | Equivalent: `reflect vars` |
| `info globals` | Equivalent: `reflect globals` |
| `info args` | Equivalent: `reflect args` |
| `info body` | Equivalent: `reflect body` |
| `info exists` | Equivalent: `reflect has-var` |
| `info level` | Not present (current call depth) |
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

### `uplevel` and `upvar`

Tcl's `uplevel` evaluates code in a caller's scope by numeric level. Vel's `upeval` evaluates in the immediate parent scope only; there is no numeric level argument.

Tcl's `upvar` creates a local alias to a variable in another scope. Vel has no equivalent.

### Multiple Return Values via `lassign`

Tcl has `lassign list var1 var2 ...` to unpack a list into multiple variables in a single operation. Vel requires using `index` repeatedly.

### `dict` Commands

Tcl's `dict` command family is a complete key-value store built on an ordered dictionary type:

`dict create`, `dict get`, `dict set`, `dict unset`, `dict exists`, `dict keys`, `dict values`, `dict for`, `dict merge`, `dict size`, `dict with`, `dict filter`, `dict update`.

Vel has no equivalent. Associative data must be simulated with lists or parallel variables.

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

### `clock` Commands

Tcl has an extensive `clock` subsystem: `clock seconds`, `clock milliseconds`, `clock microseconds`, `clock format`, `clock scan`, `clock add`. Vel has a `date` command (wraps `strftime`) and `sleep`, but no millisecond-resolution timestamps, no date parsing, and no date arithmetic.

### `binary` Commands

Tcl has `binary format` and `binary scan` for packing and unpacking binary data. Vel has no equivalent.

### `trace` Command

Tcl's `trace` command allows monitoring variable reads, writes, and unsets, and command execution, at the language level. Vel has `watch` which covers variable write traces on assignment only.

---

## Vel Features Not in Standard Tcl

### Integrated Shell Commands

Vel includes a large set of Unix shell-utility commands as built-ins: `ls`, `tree`, `find`, `cat`, `head`, `tail`, `wc`, `grep`, `touch`, `stat`, `mkdir`, `rmdir`, `remove`, `copy`, `move`, `chmod`, `chown`, `ln`, `glob`, `cd`, `pwd`, `which`, `env`, `whoami`, `hostname`, `uname`, `uptime`, `df`, `seq`, `clear`, `yes`, `tee`, `echo`, `printf`.

Standard Tcl provides equivalent functionality through the `file` command family and `exec`, but does not implement these as first-class commands.

### POSIX Job Control

Vel has `spawn`/`bg`, `wait`, `waitall`, `fg`, `jobs`, `killjob`, `jobstatus`, `bglist`, `bgpid`, `sighandle`, and `shpipe` for shell-style job control. Standard Tcl's `exec` is synchronous; background execution requires the `&` suffix and lacks Vel's job table.

### Auto-Execution of External Programs

In vel, any unrecognized command that is found on `PATH` is automatically executed as an external program with inherited stdio. This behavior is similar to a Unix shell. Tcl requires explicit `exec` for external commands.

### Template Engine

Vel has a built-in `vel_template` C API and `<?vel ... ?>` tag syntax for embedding vel code in text documents. Tcl does not have a built-in template engine.

### Integer Overflow Detection

Vel detects and reports `int64_t` overflow in `+`, `-`, and `*` operations in the expression evaluator. Standard Tcl silently wraps or promotes to wide integers.

### `redirect`, `redirect2`, `redirect_in`, `pipe`

Vel provides direct control over file descriptor redirection for external processes through first-class language commands. Tcl uses `exec` with redirection syntax embedded in the argument list (e.g., `exec cmd < infile > outfile`).

---

## Syntax Differences

| Feature | Tcl | Vel |
|---------|-----|-----|
| Function definition | `proc name {params} {body}` | `func name {params} {body}` |
| Variable assignment | `set name value` | `set name value` (same) |
| List foreach | `foreach var list {body}` | `for var list {body}` or `foreach var list {body}` |
| Error handling | `catch {body} var` | `try {body} var {handler}` |
| Introspection | `info ...` | `reflect ...` |
| Increment | `incr var` | `inc var` |
| Decrement | no built-in | `dec var` |
| Block comment | no built-in | `## text ##` |
| Integer division | `expr {$a / $b}` (int if both int) | `expr $a \ $b` |

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

---

## Summary Table

| Category | Vel | Tcl 8.6 |
|----------|-----|---------|
| Basic syntax (commands, substitution, quoting) | Yes | Yes |
| Arithmetic expressions | Yes | Yes |
| String operations (basic) | Yes | Yes |
| Regular expressions | No | Yes |
| `string map` (multi-pair) | No | Yes |
| `string is` type testing | No | Yes |
| List operations | Partial | Full |
| `lassign` | No | Yes |
| Dictionaries | No | Yes |
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
| `uplevel N` / `upvar` | Partial (`upeval` only) | Full |
| `apply` (nameless lambda) | Partial (registers a name) | Yes |
| Job control (POSIX) | Yes | No (basic exec only) |
| Auto-exec from PATH | Yes | No |
| Shell utility commands | Yes (ls, grep, find, etc.) | No |
| Template engine | Yes | No |
| Integer overflow detection | Yes | No |
| `watch` (variable trace on write) | Yes | Yes (via `trace`) |
| Embedded C API | Yes | Yes |
