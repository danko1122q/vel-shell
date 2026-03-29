# Vel Language Reference

Version: 0.2.0

---

## Table of Contents

1. [Overview](#overview)
2. [Syntax Model](#syntax-model)
3. [Data Types](#data-types)
4. [Token Forms](#token-forms)
5. [Variables](#variables)
6. [Control Flow](#control-flow)
7. [Functions](#functions)
8. [Lists](#lists)
9. [Expressions](#expressions)
10. [String Operations](#string-operations)
11. [I/O Commands](#io-commands)
12. [File System Commands](#file-system-commands)
13. [Process and Job Commands](#process-and-job-commands)
14. [Reflection and Metaprogramming](#reflection-and-metaprogramming)
15. [Extended Commands](#extended-commands)
16. [Error Handling](#error-handling)
17. [Comments](#comments)
18. [Scoping Rules](#scoping-rules)
19. [Special Variables](#special-variables)
20. [Shebang Support](#shebang-support)

---

## Overview

Vel is a command language. Every statement is a command invocation. The first word on a line is the command name; subsequent words are its arguments. This model is similar to Tcl, but vel integrates shell-like features such as external program execution, pipelines, and job control directly into the language.

---

## Syntax Model

A vel program is a sequence of statements separated by newlines or semicolons. Each statement is a list of words. The first word is dispatched as a command name. If the command name is not a registered built-in or user-defined function, vel attempts to execute it as an external program found on `PATH`.

```
command arg1 arg2 arg3
```

Words are separated by horizontal whitespace (spaces and tabs). Line continuation is supported with a trailing backslash:

```
set result [expr \
    1 + 2 + 3]
```

---

## Data Types

Vel has a single value type: a string. All values are strings. Numeric operations convert strings to integers or floating-point numbers on demand. Lists are strings in a specific packed format (space-separated, brace-quoted when necessary). There are no separate boolean, integer, float, list, or dictionary types at the value level.

Integers are represented internally as `int64_t`. Floating-point values are `double`. Both are formatted back to strings when assigned to variables or returned from commands.

---

## Token Forms

### Bare Word

Any sequence of non-whitespace, non-special characters. Variable substitution does not occur inside bare words unless the word contains `$`.

```
write hello
```

### Variable Expansion: `$name`

A `$` followed by a name expands to the value of that variable in the current scope.

```
set x 42
write $x
```

### Bracket Evaluation: `[code]`

Square brackets evaluate their content as a vel script and substitute the result as a single word.

```
set result [expr 2 + 2]
```

Bracket evaluation is recursive and can be nested.

### Brace Quoting: `{text}`

Curly braces quote their content literally. No substitution occurs. This is the standard way to pass code blocks or multi-word strings without immediate evaluation.

```
set body {write "hello"}
eval $body
```

### Double-Quoted String: `"text"`

Double-quoted strings allow `$name` variable expansion and the following escape sequences:

| Escape | Meaning |
|--------|---------|
| `\n` | newline |
| `\t` | tab |
| `\r` | carriage return |
| `\"` | literal double quote |
| `\\` | literal backslash |
| `\o` | literal `{` (used in template output) |
| `\c` | literal `}` (used in template output) |

### Single-Quoted String: `'text'`

Single-quoted strings allow `$name` variable expansion but no other escape processing. Useful when the string contains backslashes that should not be interpreted.

### Here-Document: `<<DELIM`

Multi-line literal strings. Lines are accumulated until a line consisting solely of the delimiter (optionally with leading whitespace). The delimiter is consumed and not included in the result.

```
set text <<END
line one
line two
END
```

---

## Variables

### `set name value`

Assigns `value` to `name` in the current scope. If the variable exists in a parent scope, that parent binding is updated. If no such variable exists, a new one is created in the current scope.

```
set x 10
set greeting "hello world"
```

### `local name value`

Creates a variable in the current scope only, even if a variable with the same name exists in a parent scope.

```
local counter 0
```

### `$name` Expansion

Expands to the string value of `name` in the nearest enclosing scope that contains it.

### Dollar Prefix Behavior

By default, a bare `$name` token is equivalent to `set name`. This can be changed via `reflect dollar-prefix`.

---

## Control Flow

### `if condition {then-body} [else {else-body}]`

Evaluates `condition` as a vel expression (via `expr`). Executes `then-body` if the result is non-zero and non-empty. Optionally executes `else-body`.

```
if {$x > 0} {
    write "positive"
} else {
    write "non-positive"
}
```

The condition may also be a command or block whose result is tested for truthiness (non-empty, non-zero string).

### `while condition {body}`

Repeats `body` as long as `condition` is true.

```
set i 0
while {$i < 5} {
    write $i
    inc i
}
```

### `for var list {body}`

Iterates over the items in `list`, assigning each to `var` and executing `body`.

```
for item {apple banana cherry} {
    write $item
}
```

### `foreach var list {body}`

Alias for `for` with slightly different argument handling. Also supports two-variable form for index-value iteration.

### `break`

Exits the innermost `while` or `for` loop.

### `continue`

Skips to the next iteration of the innermost loop.

### `return [value]`

Returns from the current function with an optional value.

### `result value`

Sets the implicit return value of the current block without stopping execution. Useful for building up a return value across multiple statements.

### `switch value { pattern body ... }` or `switch value {pat} {body} ...`

Matches `value` against each `pattern` in order. Executes the corresponding `body` on first match and returns its result. The keyword `default` matches any value and acts as a fallthrough clause.

Two equivalent syntaxes are supported:

**Single-block syntax (recommended)** — all pattern-body pairs are enclosed in one outer block. Patterns are bare words; they are not evaluated as commands.

```
switch $color {
    red     { write "stop"    }
    green   { write "go"      }
    yellow  { write "caution" }
    default { write "unknown" }
}
```

**Flat syntax** — each pattern and each body is a separate argument. Patterns must be enclosed in `{}` to prevent them from being evaluated as commands before `switch` is reached.

```
switch $color {red} { write "stop" } {green} { write "go" } {default} { write "unknown" }
```

The body argument is always executed as vel code, not returned as a literal string. The return value of `switch` is the return value of the matched body, or empty if no pattern matches.

```
set label [switch $code {
    0 { set "" "ok"      }
    1 { set "" "warning" }
    default { set "" "error" }
}]
```

---

## Functions

### `func name {params} {body}`

Defines a named function. `params` is a space-separated list of parameter names. `body` is the code to execute. The function name is registered globally.

```
func add {a b} {
    expr $a + $b
}
set result [add 3 4]
```

### `func {params} {body}` (anonymous)

When called with two arguments, creates an anonymous function with a generated name and returns that name.

```
set fn [func {x} { expr $x * 2 }]
set result [$fn 5]
```

### `func {body}` (anonymous, variadic)

Creates an anonymous function that accepts all arguments as a list in the built-in `args` variable.

### `rename old new`

Renames a function. If `new` is empty, deletes the function.

### Function Calls

A function is called by using its name as a command:

```
func greet {name} { write "hello $name" }
greet "world"
```

Recursive calls are supported up to a depth of 1000 (configurable via `VEL_MAX_DEPTH`).

---

## Lists

Lists in vel are strings where items are separated by whitespace. Items containing spaces are brace-quoted. This is the same model as Tcl.

### `list arg1 arg2 ...`

Constructs a properly-quoted list from arguments.

```
set items [list "hello world" foo bar]
```

### `count list`

Returns the number of items in `list`.

```
set n [count {a b c}]
```

### `index list i`

Returns the item at zero-based index `i`.

```
set second [index {a b c} 1]
```

### `indexof list value`

Returns the zero-based index of `value` in `list`, or -1 if not found.

### `append list item ...`

Returns a new list with items appended.

```
set l [append $l newitem]
```

### `slice list start [end]`

Returns a sublist from `start` to `end` (exclusive). If `end` is omitted, slices to the end of the list.

### `filter list {predicate}`

Returns a new list containing only items for which the predicate returns a non-empty, non-zero value. The predicate receives the item as its first argument.

```
set evens [filter {1 2 3 4 5} {x} { expr $x % 2 == 0 }]
```

### `lmap list {transform}`

Returns a new list where each item has been replaced by the result of applying the transform function.

```
set doubled [lmap {1 2 3} {x} { expr $x * 2 }]
```

### `join list [separator]`

Joins list items into a single string with `separator` (default: space).

```
set s [join {a b c} ","]
```

### `foreach var list {body}`

Iterates over list items. When `var` is a two-element list, the first element receives the index and the second receives the value.

### `lreverse list`

Returns the list with elements in reverse order.

```
set rev [lreverse {a b c}]   ;# c b a
```

### `lsort list`

Returns the list sorted lexicographically (ascending).

```
set sorted [lsort {banana apple cherry}]   ;# apple banana cherry
```

### `luniq list`

Returns the list with consecutive duplicate elements removed. For complete deduplication, sort the list first.

```
set u [luniq [lsort {a b a c b}]]   ;# a b c
```

### `lassign list var1 var2 ... [*rest]`

Assigns elements of `list` to the named variables in order. Variables with no corresponding element are set to the empty string. If the last variable name starts with `*`, it collects all remaining elements as a sublist.

```
lassign {a b c d} x y *rest
# x=a  y=b  rest={c d}

lassign {p q} first second third
# first=p  second=q  third=""
```

---

## Expressions

### `expr expression`

Evaluates an arithmetic/logical expression and returns the result as a string.

Supported operators in precedence order (lowest to highest):

| Operator | Description |
|----------|-------------|
| `\|\|` | logical OR |
| `&&` | logical AND |
| `\|` | bitwise OR |
| `&` | bitwise AND |
| `==` `!=` | equality |
| `<` `>` `<=` `>=` | comparison |
| `<<` `>>` | bit shift |
| `+` `-` | addition, subtraction |
| `*` `/` `\` `%` | multiply, float-divide, integer-divide, modulo |
| `!` `~` `-` `+` | unary logical-not, bitwise-not, negate, plus |

Integer overflow on `+`, `-`, `*` is detected and raises an error. Division by zero raises an error.

```
set x [expr 10 * 3 + 2]
set y [expr $x > 20]
set z [expr ($x + $y) << 1]
```

### `inc name [amount]`

Increments the variable `name` by `amount` (default: 1).

### `dec name [amount]`

Decrements the variable `name` by `amount` (default: 1).

### `abs n`

Returns the absolute value of `n`. Returns an integer if `n` is an integer, float otherwise.

```
set v [abs -7]    ;# 7
set v [abs -3.5]  ;# 3.5
```

### `max a b`

Returns the larger of two values.

### `min a b`

Returns the smaller of two values.

### `math subcommand [args]`

Dispatcher for mathematical functions. All results are returned as integer when the result has no fractional part, float otherwise.

| Subcommand | Args | Description |
|------------|------|-------------|
| `pi` | — | π (3.14159...) |
| `e` | — | Euler's number (2.71828...) |
| `sin` | x | Sine (radians) |
| `cos` | x | Cosine (radians) |
| `tan` | x | Tangent (radians) |
| `asin` | x | Arcsine |
| `acos` | x | Arccosine |
| `atan` | x | Arctangent |
| `atan2` | y x | Two-argument arctangent |
| `sqrt` | x | Square root |
| `pow` | base exp | Exponentiation |
| `log` | x | Natural logarithm |
| `log2` | x | Base-2 logarithm |
| `log10` | x | Base-10 logarithm |
| `abs` | x | Absolute value |
| `floor` | x | Floor |
| `ceil` | x | Ceiling |
| `round` | x | Round to nearest integer |

```
set hyp [math sqrt [expr $a*$a + $b*$b]]
set angle [math atan2 $dy $dx]
set pi [math pi]
```

---

## String Operations

### `length string`

Returns the byte length of `string`.

### `substr string start [length]`

Returns a substring starting at `start` for `length` bytes. If `length` is omitted, returns to the end.

### `strpos string needle [start]`

Returns the byte offset of the first occurrence of `needle` in `string`, starting at optional `start`. Returns -1 if not found.

### `charat string index`

Returns the character at byte position `index`.

### `codeat string index`

Returns the ASCII/byte value of the character at byte position `index`.

### `char code`

Returns the single-character string for the given byte value.

### `trim string`

Removes leading and trailing whitespace.

### `ltrim string`

Removes leading whitespace.

### `rtrim string`

Removes trailing whitespace.

### `toupper string`

Converts string to uppercase.

### `tolower string`

Converts string to lowercase.

### `strcmp a b`

Returns negative, zero, or positive based on lexicographic comparison.

### `streq a b`

Returns 1 if strings are equal, empty otherwise.

### `startswith string prefix`

Returns 1 if `string` begins with `prefix`.

### `endswith string suffix`

Returns 1 if `string` ends with `suffix`.

### `repstr string old new`

Replaces all occurrences of `old` with `new` in `string`.

### `split string [separator]`

Splits `string` by `separator` (default: whitespace) and returns a list.

### `concat arg1 arg2 ...`

Concatenates arguments into a single string without separators.

### `quote value`

Returns a properly brace-quoted version of `value` suitable for inclusion in a list.

### `subst string`

Performs variable and bracket substitution on `string` as if it were source code.

### `format fmt arg1 arg2 ...`

Formats a string using printf-style format specifiers (`%s`, `%d`, `%f`, `%x`, etc.).

### `printf fmt arg1 arg2 ...`

Like `format` but writes the result to stdout.

### `string repeat str n`

Returns `str` concatenated `n` times. Returns empty if `n` is 0 or `str` is empty.

```
set line [string repeat "-" 40]
```

### `string reverse str`

Returns the byte-reversed string. Not Unicode-aware.

```
set rev [string reverse "hello"]   ;# olleh
```

### `string is type str`

Returns 1 if all characters of `str` match `type`, empty otherwise. An empty string always returns empty.

| Type | Condition |
|------|-----------|
| `integer` | Parseable as a 64-bit integer |
| `double` | Parseable as a floating-point number |
| `alpha` | All alphabetic |
| `alnum` | All alphanumeric |
| `space` | All whitespace |
| `upper` | All uppercase letters |
| `lower` | All lowercase letters |
| `print` | All printable characters |
| `ascii` | All bytes < 128 |

```
if {[string is integer $input]} {
    write "got a number"
}
```

### `string map pairs str`

Multi-pair substitution. `pairs` is a flat list `{old1 new1 old2 new2 ...}`. Scans `str` left-to-right; the first matching pair at each position wins. `pairs` must have an even number of elements.

```
set result [string map {hello hi world earth} "hello world"]
# hi earth
```

---

## I/O Commands

### `write arg1 [arg2 ...]`

Writes arguments to the write callback (stdout by default), separated by spaces, without a trailing newline.

### `print arg1 [arg2 ...]`

Like `write` but appends a newline.

### `echo arg1 [arg2 ...]`

Writes arguments separated by spaces followed by a newline (shell-style).

### `read [prompt]`

Reads a line of input. If a `VEL_CB_READ` callback is installed, delegates to it. Otherwise reads from stdin.

### `readline`

Reads a raw line from stdin. Returns an error at EOF.

### `input [prompt]`

Displays `prompt` (if given) and reads a line from stdin.

### `canread`

Returns 1 if stdin is readable (not at EOF or error).

### `store name data`

Invokes the `VEL_CB_STORE` callback to persist key-value data outside the interpreter.

### `source filename`

Reads and executes the contents of `filename`. Searches via the `VEL_CB_SOURCE` callback if registered.

### `cat [file ...]`

Reads and writes file contents to the write callback. If no file is given, reads from stdin.

### `head [-n N] [file]`

Outputs the first N lines (default 10) of a file.

### `tail [-n N] [file]`

Outputs the last N lines (default 10) of a file.

### `wc [-l] [-w] [-c] [file]`

Counts lines, words, or bytes in a file.

### `grep [-i] [-v] [-n] [-c] [-r] pattern [file ...]`

Searches for lines matching `pattern` in files. Supports basic substring matching. Flags: `-i` case-insensitive, `-v` invert match, `-n` show line numbers, `-c` count only, `-r` recursive directory search.

### `tee file`

Reads from the last pipe stage or current output and writes to both stdout and `file`.

### `redirect {command} file [mode]`

Runs `command` and redirects its stdout to `file`. Mode `a` appends; default overwrites.

### `redirect2 {command} stdout-file stderr-file`

Redirects both stdout and stderr of `command` to separate files.

### `redirect_in {command} file`

Runs `command` with stdin redirected from `file`.

---

## File System Commands

### `exists path`

Returns 1 if `path` exists (file or directory).

### `listdir [dir]`

Returns a list of filenames in `dir` (default: current directory).

### `ls [-l] [-a] [path]`

Lists directory contents. `-l` for long format, `-a` to include hidden files.

### `tree [path] [-d] [-L depth]`

Prints directory tree. `-d` directories only, `-L depth` limits depth.

### `find path [-name pattern] [-type f|d] [-maxdepth N]`

Finds files matching criteria under `path`.

### `stat path`

Returns file metadata as a string (type, size, permissions, mtime).

### `touch path`

Creates file if it does not exist; updates mtime if it does.

### `mkdir [-p] path`

Creates a directory. `-p` creates intermediate directories.

### `rmdir [-r] path`

Removes a directory. `-r` removes recursively.

### `remove path`

Removes a file.

### `copy src dst`

Copies a file from `src` to `dst`.

### `move src dst`

Moves or renames a file.

### `chmod mode path`

Changes file permissions. `mode` is an octal string (e.g., `755`) or symbolic (e.g., `+x`).

### `chown user[:group] path`

Changes file ownership (Unix only).

### `ln [-s] target link`

Creates a hard link or, with `-s`, a symbolic link.

### `cd [dir]`

Changes the working directory of the vel process.

### `pwd`

Returns the current working directory.

### `getwd`

Alias for `pwd`.

### `basename path`

Returns the filename component of a path.

### `dirname path`

Returns the directory component of a path.

### `glob pattern ...`

Expands shell glob patterns and returns a list of matching paths. Supports tilde expansion.

---

## Process and Job Commands

### `run cmd [arg ...]`

Executes an external program with stdin, stdout, and stderr inherited from the vel process (live output). Returns the exit code.

### `exec cmd [arg ...]`

Executes an external program and captures its stdout, returning it as a string.

### `system cmd [arg ...]`

Runs a command with output capture and a 30-second timeout (configurable via `VEL_SYSTEM_TIMEOUT` environment variable; set to 0 for no limit).

### `sh {command string}`

Runs a command string via `/bin/sh -c` and returns captured output.

### `pipe {cmd1 args} {cmd2 args} ...`

Creates a Unix pipeline. Each argument is a command specification. Returns the output of the last stage.

### `shpipe {cmd1 args} {cmd2 args} ...`

Pipeline via `/bin/sh -c` for each stage.

### `spawn cmd [arg ...]`

Forks and executes a command in the background. Returns the job ID.

### `bg cmd [arg ...]`

Alias for `spawn`.

### `wait [jid]`

Waits for background job `jid` to finish. If `jid` is omitted, waits for all jobs.

### `waitall`

Waits for all background jobs.

### `fg [jid]`

Brings background job `jid` to the foreground.

### `jobs`

Lists all background jobs with their state and exit code.

### `bglist`

Returns a list of active background job IDs.

### `bgpid jid`

Returns the PID of background job `jid`.

### `killjob jid_or_pid [signal]`

Sends `signal` (default: `SIGTERM`) to a job or PID.

### `jobstatus jid`

Returns the exit status of a finished background job.

### `sighandle SIGNAL {vel-code}`

Registers vel code to run when `SIGNAL` is received. Signal handling is deferred to the main loop.

### `exitcode`

Returns the exit code of the most recently executed external command.

### `getpid`

Returns the PID of the vel process.

### `envget name [default]`

Gets the value of environment variable `name`. Returns `default` if not set.

### `envset name value`

Sets an environment variable in the current process.

### `env`

Returns a list of all current environment variables as `NAME=VALUE` strings.

### `whoami`

Returns the current username.

### `hostname`

Returns the system hostname.

### `uname [-a] [-s] [-r] [-m]`

Returns system information.

### `uptime`

Returns system uptime.

### `which name`

Returns the full path of a command found on `PATH`.

### `date [format]`

Returns the current date and time, formatted by `format` (strftime syntax). Default is `%Y-%m-%d %H:%M:%S`.

### `sleep seconds`

Pauses execution for the given number of seconds (supports fractional values).

### `rand [max]`

Returns a random integer between 0 and `max-1` (default: `RAND_MAX`).

### `df [path]`

Returns disk usage information for the filesystem containing `path`.

### `seq start [end] [step]`

Generates a sequence of integers.

### `yes [string]`

Outputs `string` (default `y`) repeatedly until interrupted.

### `clear`

Clears the terminal screen.

---

## Reflection and Metaprogramming

### `reflect version`

Returns the vel version string.

### `reflect funcs`

Returns a list of all registered function names.

### `reflect func-count`

Returns the number of registered functions.

### `reflect vars`

Returns a list of all visible variable names in the current scope chain.

### `reflect globals`

Returns a list of all global variable names.

### `reflect args name`

Returns the parameter list of function `name`.

### `reflect body name`

Returns the body of scripted function `name`. Not available for native functions.

### `reflect has-func name`

Returns 1 if function `name` is registered.

### `reflect has-var name`

Returns 1 if variable `name` is visible in the current scope.

### `reflect has-global name`

Returns 1 if variable `name` exists in the global scope.

### `reflect error`

Returns the current error message, if any.

### `reflect this`

Returns the body of the currently executing function or block.

### `reflect name`

Returns the name of the currently executing function.

### `reflect dollar-prefix [new-prefix]`

Gets or sets the prefix used when expanding `$name` tokens. Default is `set `.

### `reflect level`

Returns the current call stack depth (number of active scripted function frames).

```
func inner {} {
    write [reflect level]   ;# 2 if called from outer
}
func outer {} { inner }
outer
```

### `reflect depth`

Returns the current recursion depth (`vel->depth`), which counts all nested `vel_parse` calls including internal ones.

### `eval code`

Evaluates `code` as vel script in the current scope.

### `topeval code`

Evaluates `code` at the global (root) scope.

### `upeval code`

Evaluates `code` in the parent scope.

### `enveval env-name {params} {body} arg ...`

Evaluates `body` in a named environment context.

### `jaileval code`

Evaluates `code` in an isolated scope with no access to parent variables.

### `watch name {code}`

Registers `code` to be executed whenever variable `name` is assigned a new value.

### `catcher {code}`

Registers `code` as the default handler for unknown command names.

### `unusedname [hint]`

Returns a unique function name not currently in use, based on `hint`.

### `upvar localname parentname`

Creates a local alias for a variable in the parent (calling) scope. Reads the parent's current value into `localname` and installs a write-back watch so that subsequent assignments to `localname` propagate back to `parentname` in the caller.

```
func bump {} {
    upvar n counter
    inc n
}

set counter 0
bump
write $counter   ;# 1
```

The write-back targets `root_env` if `parentname` lives at global scope (using `set global`), or the immediate parent frame otherwise (using `upeval`).

---

## Extended Commands

This section documents commands from `vel_newcmds.c` that do not fit neatly into the categories above.

### `clock [unit]`

Returns the current wall-clock time using `CLOCK_REALTIME`.

| Unit | Return | Description |
|------|--------|-------------|
| `s` (default) | float | Seconds since the Unix epoch with nanosecond precision |
| `ms` | integer | Milliseconds since the Unix epoch |
| `us` | integer | Microseconds since the Unix epoch |
| `ns` | integer | Nanoseconds since the Unix epoch |

```
set t0 [clock ms]
# ... work ...
set elapsed [expr [clock ms] - $t0]
write "elapsed: ${elapsed}ms"
```

### `dict subcommand ...`

Implements an associative array stored as a flat key-value list `{k1 v1 k2 v2 ...}`. This format is compatible with Tcl dicts and vel list operations.

#### `dict set varname key value`

Sets `key` to `value` in the dict stored in variable `varname`. Updates in place if the key exists; appends a new pair otherwise.

```
dict set d name Alice
dict set d age 30
```

#### `dict get dict key`

Returns the value for `key`, or empty if the key is not present.

```
set name [dict get $d name]
```

#### `dict exists dict key`

Returns 1 if `key` is present in the dict, empty otherwise.

```
if {[dict exists $d email]} { write "has email" }
```

#### `dict unset varname key`

Removes the key-value pair for `key` from the dict in `varname`. No-op if the key is absent.

#### `dict keys dict`

Returns a list of all keys in the dict.

```
for k [dict keys $d] { write $k }
```

#### `dict values dict`

Returns a list of all values in the dict (in insertion order).

#### `dict size dict`

Returns the number of key-value pairs.

```
set n [dict size $d]
```

#### `dict for {kvar vvar} dict body`

Iterates over the dict, binding each key and value to `kvar` and `vvar` and executing `body`. Supports `break` and `return`.

```
dict for {k v} $d {
    write "$k = $v"
}
```

**Example — building and querying a dict:**

```
dict set config host localhost
dict set config port 8080
dict set config debug 1

write "connecting to [dict get $config host]:[dict get $config port]"

if {[dict get $config debug]} {
    write "debug mode enabled"
}
```

### `base64 encode str`

Encodes the byte string `str` to Base64 using the standard alphabet. No external library is required.

```
set encoded [base64 encode "hello world"]
# aGVsbG8gd29ybGQ=
```

### `base64 decode str`

Decodes a Base64 string. Returns an error if the input contains characters outside the Base64 alphabet. The result is a raw byte string.

```
set decoded [base64 decode "aGVsbG8gd29ybGQ="]
# hello world
```

---

## Error Handling

### `try body [errvar catch_body] [finally fin_body]`

Executes `body`. Supports optional catch and finally clauses. All clause combinations are valid:

```
try body
try body errvar catch_body
try body finally fin_body
try body errvar catch_body finally fin_body
```

If an error occurs in `body` and `catch_body` is provided, the error message is bound to `errvar` and `catch_body` is executed.

`fin_body` (the `finally` clause) **always** runs — even if `body` or `catch_body` used `return`, `break`, or raised an uncaught error. After `fin_body` completes, the original error state, stop signal, and return value are restored. Any side effects inside `fin_body` do not override the outer control flow.

```
try {
    error "something went wrong"
} msg {
    write "caught: $msg"
} finally {
    write "cleanup always runs"
}
```

```
func open_and_process {file} {
    set f [fopen $file]
    try {
        process $f
    } finally {
        fclose $f   ;# runs even if process errors or returns early
    }
}
```

### `error message`

Raises an error with the given message. Halts execution of the current statement chain and propagates up to the nearest `try` block.

---

## Comments

### Line comment

```
# this is a comment
set x 1  # inline comment
```

### Block comment

```
## this is a
   multi-line block comment ##
```

Block comments start with `##` and end with `##`. A triple `###` is not a block comment marker.

---

## Scoping Rules

Vel uses lexical scoping with dynamic variable lookup. Each function call creates a new environment. Variable lookup walks the environment chain from innermost to outermost.

- `set` writes to the nearest existing binding, or creates a new one in the current scope.
- `local` always creates a new binding in the current scope.
- `topeval` executes code at the global scope.
- `upeval` executes code one scope level up.
- `jaileval` executes code in a completely isolated scope.
- `upvar` creates a local alias that mirrors a variable in the parent scope.

---

## Special Variables

| Variable | Description |
|----------|-------------|
| `?` | Exit code of the most recently executed external command |
| `argv` | List of command-line arguments when running a script file |

---

## Shebang Support

Vel treats `#` as a line comment, so a shebang line is silently ignored:

```vel
#!/usr/bin/env vel
write "hello"
```

Mark the file executable and ensure `vel` is on `PATH`:

```sh
chmod +x script.vel
./script.vel
```
