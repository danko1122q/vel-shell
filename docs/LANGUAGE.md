# Vel Language Reference

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

Vel is a command language. Every statement is a command invocation. The first word on a line is the command name; subsequent words are its arguments. This model integrates shell-like features such as external program execution, pipelines, and job control directly into the language.

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

Any sequence of non-whitespace, non-special characters.

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

Single-quoted strings allow `$name` variable expansion but no other escape processing.

### Here-Document: `<<DELIM`

Multi-line literal strings. Lines are accumulated until a line consisting solely of the delimiter.

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

### `set global name value`

Assigns `value` to `name` in the root (global) scope, regardless of the current scope.

```
func example {} {
    set global counter 42
}
```

### `local name value`

Creates a variable in the current scope only.

```
local counter 0
```

### `$name` Expansion

Expands to the string value of `name` in the nearest enclosing scope that contains it.

### Dollar Prefix Behavior

By default, a bare `$name` token is equivalent to `set name`. This can be changed via `reflect dollar-prefix`.

---

## Control Flow

### `if condition {then-body} [elseif condition {body} ...] [else {else-body}]`

Evaluates `condition` as a vel expression. Executes the matching branch.

```
if {$x > 0} {
    write "positive"
} elseif {$x == 0} {
    write "zero"
} else {
    write "negative"
}
```

### `while condition {body}`

Repeats `body` as long as `condition` is true.

```
set i 0
while {$i < 5} {
    write $i
    incr i
}
```

### `for var list {body}`

Iterates over the items in `list`, assigning each to `var` and executing `body`.

```
for item {apple banana cherry} {
    write $item
}
```

### `for {init} {cond} {step} {body}`

C-style for loop with explicit init, condition, step, and body.

```
for {set i 0} {$i < 5} {incr i} {
    write $i
}
```

### `foreach var list {body}`

Iterates over list items. When `var` is a brace-quoted list of variable names, each iteration consumes that many items from `list`:

```
foreach {a b} {1 2 3 4 5 6} {
    write "$a $b"
}
```

### `break`

Exits the innermost loop.

### `continue`

Skips to the next iteration of the innermost loop.

### `return [value]`

Returns from the current function with an optional value.

### `result value`

Sets the implicit return value of the current block without stopping execution.

### `switch value { pattern body ... }` or `switch value {pat} {body} ...`

Matches `value` against each `pattern` in order. Executes the corresponding `body` on first match. The keyword `default` matches any value.

```
switch $color {
    red     { write "stop"    }
    green   { write "go"      }
    default { write "unknown" }
}
```

---

## Functions

### `func name {params} {body}`

Defines a named function.

```
func add {a b} {
    expr $a + $b
}
set result [add 3 4]
```

### `func {params} {body}` (anonymous)

Creates an anonymous function and returns its name.

```
set fn [func {x} { expr $x * 2 }]
set result [$fn 5]
```

### `func {body}` (anonymous, variadic)

Creates an anonymous function that accepts all arguments as a list in `args`.

### `rename old new`

Renames a function. If `new` is empty, deletes the function.

### Function Calls

```
func greet {name} { write "hello $name" }
greet "world"
```

Recursive calls are supported up to a depth of 1000 (configurable via `VEL_MAX_DEPTH`).

---

## Lists

Lists in vel are strings where items are separated by whitespace. Items containing spaces are brace-quoted.

### `list arg1 arg2 ...`

Constructs a properly-quoted list from arguments.

```
set items [list "hello world" foo bar]
```

### `count list` / `llength list`

Returns the number of items in `list`.

```
set n [llength {a b c}]
```

### `index list i` / `lindex list i`

Returns the item at zero-based index `i`.

```
set second [lindex {a b c} 1]
```

### `indexof list value` / `lsearch list value`

Returns the zero-based index of `value` in `list`, or -1 if not found.

### `slice list start [end]` / `lrange list start end`

Returns a sublist. `lrange` is inclusive on both ends; `slice` treats end as exclusive.

```
set sub [lrange {a b c d} 1 2]   ;# b c
```

### `append varname str ...`

Appends one or more strings to the variable `varname` in place (string concatenation).

```
set s "hello"
append s " world"
# s is now "hello world"
```

### `lappend varname item ...`

Appends items to the list stored in variable `varname` and returns the new list.

```
set lst {a b c}
lappend lst d e
# lst is now "a b c d e"
```

### `linsert list idx val ...`

Returns a new list with `val ...` inserted before index `idx`.

```
set r [linsert {a b c} 1 X Y]   ;# a X Y b c
```

### `lreplace list first last ?val ...?`

Returns a new list with elements from `first` to `last` (inclusive) replaced by `val ...`. Pass no replacement values to delete the range.

```
set r [lreplace {a b c d} 1 2 X]   ;# a X d
```

### `filter list {predicate}`

Returns items for which the predicate returns a non-empty, non-zero value.

```
set evens [filter {1 2 3 4 5} {x} { expr $x % 2 == 0 }]
```

### `lmap list {transform}`

Returns a new list with each item replaced by the transform result.

### `join list [separator]`

Joins list items into a single string with `separator` (default: space).

```
set s [join {a b c} ","]
```

### `split string [separator]`

Splits `string` by `separator` (default: whitespace) and returns a list.

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

Returns the list with consecutive duplicate elements removed.

```
set u [luniq [lsort {a b a c b}]]   ;# a b c
```

### `lassign list var1 var2 ... [*rest]`

Assigns elements of `list` to the named variables in order. If the last variable name starts with `*`, it collects all remaining elements.

```
lassign {a b c d} x y *rest
# x=a  y=b  rest={c d}
```

### `concat arg1 arg2 ...`

Concatenates list arguments into a single flat list.

### `subst string`

Performs variable and bracket substitution on `string`.

### `quote value`

Returns a properly brace-quoted version of `value`.

---

## Expressions

### `expr expression`

Evaluates an arithmetic/logical expression and returns the result.

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
| `**` | power (exponentiation) |
| `!` `~` `-` `+` | unary: logical-not, bitwise-not, negate, plus |

Integer overflow on `+`, `-`, `*` is detected and raises an error. Division by zero raises an error.

```
set x [expr 10 * 3 + 2]
set y [expr 2 ** 8]       ;# 256
set z [expr $x > 20]
```

### `incr name [amount]` / `inc name [amount]`

Increments variable `name` by `amount` (default: 1).

```
set n 5
incr n      ;# n = 6
incr n 4    ;# n = 10
```

### `decr name [amount]` / `dec name [amount]`

Decrements variable `name` by `amount` (default: 1).

### `abs n`

Returns the absolute value of `n`.

```
set v [abs -7]    ;# 7
```

### `max n1 n2 [n3 ...]`

Returns the largest of all given values.

```
set m [max 3 7 1 9 2]   ;# 9
```

### `min n1 n2 [n3 ...]`

Returns the smallest of all given values.

### `math subcommand [args]`

| Subcommand | Args | Description |
|------------|------|-------------|
| `pi` | — | π |
| `e` | — | Euler's number |
| `sin` | x | Sine (radians) |
| `cos` | x | Cosine |
| `tan` | x | Tangent |
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
set pi  [math pi]
```

### `rand [max]`

Returns a random integer between 0 and `max-1`. With no argument, returns a float in `[0.0, 1.0)`.

### `sleep seconds`

Pauses execution. Supports fractional seconds.

---

## String Operations

### `length string`

Returns the byte length of `string`.

### `substr string start [length]`

Returns a substring starting at `start` for `length` bytes.

### `strpos string needle [start]`

Returns the byte offset of the first occurrence of `needle` in `string`, or -1 if not found.

### `charat string index`

Returns the character at byte position `index`.

### `codeat string index`

Returns the byte value of the character at byte position `index`.

### `char code`

Returns the single-character string for the given byte value.

### `trim string` / `ltrim string` / `rtrim string`

Remove leading and/or trailing whitespace.

### `toupper string` / `tolower string`

Convert case.

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

### `format fmt arg1 arg2 ...`

Formats a string using printf-style specifiers (`%s`, `%d`, `%f`, `%x`, etc.).

### `printf fmt arg1 arg2 ...`

Like `format` but writes the result to stdout.

### `scan string fmt var1 var2 ...`

Parses `string` according to scanf-style format and stores the results in the named variables. Returns the number of successfully converted items.

```
scan "42 3.14 hello" "%d %f %s" myint myfloat mystr
```

Supported format specifiers: `%d`, `%i`, `%f`, `%g`, `%e`, `%s`.

### `string length str`

Returns the byte length of `str`.

### `string toupper str` / `string tolower str`

Convert case.

### `string index str idx`

Returns the character at zero-based index `idx`.

### `string range str first last`

Returns characters from `first` to `last` (inclusive, zero-based).

```
set sub [string range "abcdefgh" 2 5]   ;# cdef
```

### `string first needle haystack [start]`

Returns the index of the first occurrence of `needle` in `haystack`, or -1.

### `string last needle haystack`

Returns the index of the last occurrence of `needle`.

### `string replace str first last [newstr]`

Returns a new string with characters `first` through `last` replaced by `newstr`.

```
set r [string replace "hello" 1 3 "ELL"]   ;# hELLo
```

### `string trim str [chars]` / `string trimleft str [chars]` / `string trimright str [chars]`

Remove characters in `chars` (default: whitespace) from the specified end(s).

### `string compare str1 str2`

Returns negative, zero, or positive.

### `string equal str1 str2`

Returns 1 if equal, 0 otherwise.

### `string repeat str n`

Returns `str` concatenated `n` times.

```
set line [string repeat "-" 40]
```

### `string reverse str`

Returns the byte-reversed string.

### `string is type str`

Returns 1 if all characters of `str` match `type`, 0 otherwise.

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

Multi-pair substitution. `pairs` is `{old1 new1 old2 new2 ...}`.

```
set result [string map {a A e E} "apple"]   ;# ApplE
```

---

## I/O Commands

### `write arg1 [arg2 ...]`

Writes arguments to stdout, separated by spaces, without a trailing newline.

### `print arg1 [arg2 ...]`

Like `write` but appends a newline.

### `echo arg1 [arg2 ...]`

Writes arguments separated by spaces followed by a newline (shell-style).

### `read [prompt]`

Reads a line of input from the `VEL_CB_READ` callback or stdin.

### `input [-p prompt] [varname]`

Displays `prompt` (if given) and reads a line from stdin. Optionally stores the result in `varname`.

### `store name data` / `writefile path data`

Writes `data` to `path` (or invokes `VEL_CB_STORE` if registered).

```
writefile "/tmp/output.txt" "hello\n"
```

### `read path` / `readfile path`

Reads the contents of `path` and returns it as a string.

```
set contents [readfile "/tmp/output.txt"]
```

### `source filename`

Reads and executes the contents of `filename`.

### `cat [file ...]`

Reads and writes file contents to the write callback.

### `head [-n N] [file]`

Outputs the first N lines (default 10) of a file.

### `tail [-n N] [file]`

Outputs the last N lines (default 10) of a file.

### `wc [-l] [-w] [-c] [file]`

Counts lines, words, or bytes in a file.

### `grep [-i] [-v] [-n] [-c] pattern [file ...]`

Searches for lines matching `pattern` in files. Flags may appear in any position relative to the pattern and filenames.

- `-i` case-insensitive match
- `-v` invert match
- `-n` show line numbers
- `-c` count only

Pattern anchors are supported: `^` matches start of line, `$` matches end of line.

```
set r [grep "^error" logfile.txt]
set r [grep "hello" -i myfile.txt]
```

### `tee file`

Writes to both stdout and `file`.

### `redirect {command} file [mode]`

Runs `command` with stdout redirected to `file`. Mode `append` appends; default overwrites.

### `redirect2 {command} stdout-file stderr-file`

Redirects both stdout and stderr to separate files.

### `redirect_in {command} file`

Runs `command` with stdin redirected from `file`.

---

## File System Commands

### `file subcommand path`

Unified file information command.

| Subcommand | Description |
|------------|-------------|
| `file exists path` | Returns 1 if path exists, 0 otherwise |
| `file isfile path` | Returns 1 if path is a regular file |
| `file isdir path` | Returns 1 if path is a directory |
| `file size path` | Returns file size in bytes |
| `file extension path` | Returns the file extension including the dot (e.g. `.vel`) |
| `file tail path` | Returns the filename component (basename) |
| `file dir path` | Returns the directory component (dirname) |
| `file readable path` | Returns 1 if path is readable |
| `file writable path` | Returns 1 if path is writable |
| `file executable path` | Returns 1 if path is executable |
| `file mtime path` | Returns the modification time as a Unix timestamp |
| `file join path1 path2 ...` | Joins path components |

```
if {[file exists "/tmp/lock"]} { write "locked" }
set ext [file extension "script.vel"]   ;# .vel
set dir [file dir "/tmp/foo/bar.txt"]   ;# /tmp/foo
```

### `exists path`

Returns 1 if `path` exists (any type).

### `listdir [dir]`

Returns a list of filenames in `dir` (default: current directory).

### `ls [-l] [-a] [path]`

Lists directory contents.

### `tree [path] [-L depth]`

Prints directory tree.

### `find path [-name pattern] [-type f|d] [-maxdepth N]`

Finds files matching criteria under `path`.

### `stat path`

Returns file metadata.

### `touch path`

Creates or updates a file.

### `mkdir [-p] path`

Creates a directory. `-p` creates intermediate directories.

### `rmdir [-r] path`

Removes a directory.

### `remove path`

Removes a file.

### `copy src dst`

Copies a file.

### `move src dst`

Moves or renames a file.

### `chmod mode path`

Changes file permissions (octal, e.g. `755`).

### `chown user[:group] path`

Changes file ownership (Unix only).

### `ln [-s] target link`

Creates a hard or symbolic link.

### `cd [dir]`

Changes the working directory.

### `pwd`

Returns the current working directory.

### `basename path`

Returns the filename component.

### `dirname path`

Returns the directory component.

### `glob pattern ...`

Expands shell glob patterns and returns a matching list.

---

## Process and Job Commands

### `run cmd [arg ...]`

Executes an external program with inherited stdio. Returns the exit code.

### `exec cmd [arg ...]`

Executes an external program and captures its stdout.

### `pipe cmd arg1 arg2 ...`

Executes a command and captures its output. When all arguments are single tokens, they are treated as one command with its arguments. When arguments contain spaces, each is treated as a pipeline stage.

```
set result [pipe echo "hello from pipe"]
set lines  [pipe {ls /tmp} {grep "vel"}]
```

### `sh {command string}`

Runs a command string via `/bin/sh -c` and returns captured output.

### `shpipe {cmd1 args} {cmd2 args} ...`

Pipeline via `/bin/sh -c` for each stage.

### `spawn cmd [arg ...]` / `bg cmd [arg ...]`

Forks and executes a command in the background. Returns the job ID.

### `wait [jid]`

Waits for background job `jid` to finish.

### `waitall`

Waits for all background jobs.

### `fg [jid]`

Brings a background job to the foreground.

### `jobs`

Lists all background jobs.

### `killjob jid_or_pid [signal]`

Sends `signal` (default: `SIGTERM`) to a job or PID.

### `jobstatus jid`

Returns the exit status of a finished background job.

### `sighandle SIGNAL {vel-code}`

Registers vel code to run when `SIGNAL` is received. Deferred to the main loop.

### `exitcode`

Returns the exit code of the most recently executed external command.

### `getpid`

Returns the PID of the vel process.

### `envget name [default]`

Gets the value of environment variable `name`.

### `envset name value`

Sets an environment variable.

### `env`

Returns all environment variables as `NAME=VALUE` strings.

### `whoami` / `hostname` / `uname` / `uptime` / `which name`

Standard system information commands.

### `date [format]`

Returns the current date/time, formatted by `format` (strftime syntax). Default: `%Y-%m-%d %H:%M:%S`.

### `df [path]`

Returns disk usage information.

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

Returns the body of scripted function `name`.

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

Returns the current call stack depth.

### `reflect depth`

Returns the current recursion depth.

### `eval code`

Evaluates `code` as vel script in the current scope.

### `topeval code`

Evaluates `code` at the global scope.

### `upeval code`

Evaluates `code` in the parent scope.

### `jaileval code`

Evaluates `code` in an isolated scope.

### `watch name {code}`

Registers `code` to run whenever variable `name` is assigned.

### `catcher {code}`

Registers `code` as the default handler for unknown command names.

### `unusedname [hint]`

Returns a unique function name not currently in use.

### `upvar localname parentname`

Creates a local alias that mirrors a variable in the parent (calling) scope.

```
func bump {} {
    upvar n counter
    incr n
}

set counter 0
bump
write $counter   ;# 1
```

---

## Extended Commands

### `clock [unit]`

Returns the current wall-clock time using `CLOCK_REALTIME`.

| Unit | Return | Description |
|------|--------|-------------|
| `s` (default) | float | Seconds since the Unix epoch |
| `ms` | integer | Milliseconds since the Unix epoch |
| `us` | integer | Microseconds since the Unix epoch |
| `ns` | integer | Nanoseconds since the Unix epoch |

```
set t0 [clock ms]
# ... work ...
set elapsed [expr [clock ms] - $t0]
```

### `dict subcommand ...`

Associative array stored as a flat key-value list `{k1 v1 k2 v2 ...}`.

| Subcommand | Description |
|------------|-------------|
| `dict set varname key value` | Set key in dict variable |
| `dict get dict key` | Get value for key |
| `dict exists dict key` | Returns 1 if key is present |
| `dict unset varname key` | Remove key from dict variable |
| `dict keys dict` | List of all keys |
| `dict values dict` | List of all values |
| `dict size dict` | Number of key-value pairs |
| `dict for {kvar vvar} dict body` | Iterate key-value pairs |

```
dict set d name "vel"
dict set d version [reflect version]
write [dict get $d name]

dict for {k v} $d {
    write "$k = $v"
}
```

### `base64 encode str`

Encodes `str` to Base64.

### `base64 decode str`

Decodes a Base64 string.

```
set enc [base64 encode "hello world"]
set dec [base64 decode $enc]   ;# hello world
```

---

## Error Handling

### `try body [errvar catch_body] [finally fin_body]`

Executes `body`. Supports optional catch and finally clauses.

```
try {
    error "something went wrong"
} msg {
    write "caught: $msg"
} finally {
    write "cleanup always runs"
}
```

`fin_body` always runs, even if `body` used `return`, `break`, or raised an error.

### `error message`

Raises an error with the given message.

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
- `set global` always writes to the root scope.
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

```vel
#!/usr/bin/env vel
write "hello"
```

```sh
chmod +x script.vel
./script.vel
```
