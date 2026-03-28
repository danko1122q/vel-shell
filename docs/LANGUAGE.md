# Vel Language Reference

Version: 0.1.0

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
15. [Error Handling](#error-handling)
16. [Comments](#comments)
17. [Scoping Rules](#scoping-rules)
18. [Special Variables](#special-variables)
19. [Shebang Support](#shebang-support)

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

### `switch value {pattern body ...}`

Matches `value` against each `pattern` in order. Executes the corresponding `body` on first match. The pattern `default` matches anything.

```
switch $color {
    red   { write "stop" }
    green { write "go" }
    default { write "unknown" }
}
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

---

## Error Handling

### `try {body} [name {handler}]`

Executes `body`. If an error occurs, optionally binds the error message to `name` and executes `handler`.

```
try {
    error "something went wrong"
} msg {
    write "caught: $msg"
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
