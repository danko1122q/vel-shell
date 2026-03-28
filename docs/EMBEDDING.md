# Vel Embedding API

This document describes the public C API for embedding the vel interpreter in a host application.

---

## Table of Contents

1. [Header and Linking](#header-and-linking)
2. [Types](#types)
3. [Interpreter Lifecycle](#interpreter-lifecycle)
4. [Execution](#execution)
5. [Values](#values)
6. [Variables](#variables)
7. [Lists](#lists)
8. [Callbacks](#callbacks)
9. [Error Handling](#error-handling)
10. [Expressions](#expressions)
11. [Environments](#environments)
12. [Registering Native Commands](#registering-native-commands)
13. [Template Engine](#template-engine)
14. [Diagnostics](#diagnostics)
15. [Helper Macros](#helper-macros)
16. [User Data](#user-data)
17. [Complete Embedding Example](#complete-embedding-example)

---

## Header and Linking

Include `vel.h` in your source file:

```c
#include "vel.h"
```

Link with the vel object files and libm:

```sh
gcc myapp.c vel_run.c vel_cmd.c vel_sys.c vel_jobs.c vel_lex.c \
    vel_expr.c vel_mem.c vel_map.c vel_tmpl.c vel_extra.c -lm -o myapp
```

On Windows with DLL export, define `VELDLL` before including `vel.h`.

---

## Types

All types are opaque handles (pointers to incomplete structs). Never access struct members directly.

| Type | Description |
|------|-------------|
| `vel_t` | Interpreter instance |
| `vel_val_t` | A string value (may be NULL, meaning empty) |
| `vel_fn_t` | A function (built-in or scripted) |
| `vel_var_t` | A variable binding |
| `vel_env_t` | An execution environment (scope) |
| `vel_list_t` | A mutable list of `vel_val_t` items |
| `vel_int_t` | Signed 64-bit integer (`int64_t`) |

---

## Interpreter Lifecycle

### `vel_t vel_new(void)`

Creates and returns a new interpreter instance. Registers all built-in commands. Returns NULL on allocation failure.

```c
vel_t vel = vel_new();
```

### `void vel_free(vel_t vel)`

Destroys the interpreter and frees all associated memory including functions, variables, and environments.

```c
vel_free(vel);
```

---

## Execution

### `vel_val_t vel_parse(vel_t vel, const char *code, size_t len, int fnlevel)`

Parses and executes `code`. If `len` is 0, the length is computed via `strlen`. `fnlevel` should be 1 when executing a script as a top-level function body (enables `return` to work correctly), and 0 for REPL-style evaluation.

Returns the result of the last executed statement. The caller must free the returned value with `vel_val_free`.

```c
vel_val_t result = vel_parse(vel, "expr 1 + 1", 0, 0);
printf("%s\n", vel_str(result));  // "2"
vel_val_free(result);
```

### `vel_val_t vel_parse_val(vel_t vel, vel_val_t code, int fnlevel)`

Like `vel_parse` but takes a `vel_val_t` as input. Does not free `code`.

### `vel_val_t vel_call(vel_t vel, const char *name, size_t argc, vel_val_t *argv)`

Calls a registered function by name with the given argument array. Returns the result (caller must free) or NULL on error.

```c
vel_val_t args[2];
args[0] = vel_val_str("hello");
args[1] = vel_val_str("world");
vel_val_t r = vel_call(vel, "concat", 2, args);
vel_val_free(args[0]);
vel_val_free(args[1]);
vel_val_free(r);
```

### `int vel_break_run(vel_t vel, int do_break)`

If `do_break` is non-zero, sets a break signal that stops the current execution loop. Returns the previous state. Used to implement safe interruption of a running interpreter from another thread.

---

## Values

### `vel_val_t vel_val_str(const char *s)`

Creates a new value from a C string. Returns NULL on allocation failure.

### `vel_val_t vel_val_int(vel_int_t n)`

Creates a new value from a 64-bit integer.

### `vel_val_t vel_val_dbl(double n)`

Creates a new value from a double.

### `vel_val_t vel_val_clone(vel_val_t src)`

Creates a shallow clone of `src`. The internal string buffer is shared (reference-counted), making this an O(1) operation.

### `void vel_val_free(vel_val_t val)`

Releases a value. Safe to call with NULL. Decrements the reference count of the internal string buffer; the buffer is freed only when the count reaches zero.

### `const char *vel_str(vel_val_t val)`

Returns a C string pointer to the value's content. If `val` is NULL, returns an empty string `""`. The pointer is valid until the value is modified or freed.

### `double vel_dbl(vel_val_t val)`

Converts the value to a double.

### `vel_int_t vel_int(vel_val_t val)`

Converts the value to a 64-bit integer.

### `int vel_bool(vel_val_t val)`

Returns 1 if the value is non-empty and not the string `"0"`.

### Mutation: Concatenation

These functions mutate an existing value by appending content. They perform a copy-on-write if the internal buffer is shared.

```c
int vel_val_cat_ch     (vel_val_t val, char ch);
int vel_val_cat_str    (vel_val_t val, const char *s);
int vel_val_cat_str_len(vel_val_t val, const char *s, size_t len);
int vel_val_cat        (vel_val_t val, vel_val_t other);
```

All return 0 on success, non-zero on allocation failure.

---

## Variables

### `vel_var_t vel_var_set(vel_t vel, const char *name, vel_val_t val, int mode)`

Sets variable `name` to `val` using the specified mode:

| Mode constant | Value | Behavior |
|---------------|-------|----------|
| `VEL_VAR_GLOBAL` | 0 | Write to the root (global) environment |
| `VEL_VAR_LOCAL` | 1 | Write to the current scope; walk up to find existing binding |
| `VEL_VAR_LOCAL_NEW` | 2 | Always create in the current scope (used for function parameters; does not invoke `VEL_CB_SETVAR`) |
| `VEL_VAR_LOCAL_ONLY` | 3 | Write to the current scope only, never the root |

Returns the variable handle, or NULL on error.

```c
vel_val_t v = vel_val_str("hello");
vel_var_set(vel, "greeting", v, VEL_VAR_GLOBAL);
vel_val_free(v);
```

### `vel_val_t vel_var_get(vel_t vel, const char *name)`

Returns the value of variable `name`, or NULL if not found. The caller does not own the returned value and must not free it. Clone it with `vel_val_clone` if it needs to outlive the next modification of the variable.

### `vel_val_t vel_var_get_or(vel_t vel, const char *name, vel_val_t def)`

Returns the value of `name`, or `def` if not found. `def` is not freed.

---

## Lists

### `vel_list_t vel_list_new(void)`

Creates a new empty list.

### `void vel_list_free(vel_list_t list)`

Frees the list and all its items.

### `void vel_list_push(vel_list_t list, vel_val_t val)`

Appends `val` to the list. The list takes ownership of `val`.

### `size_t vel_list_len(vel_list_t list)`

Returns the number of items.

### `vel_val_t vel_list_get(vel_list_t list, size_t idx)`

Returns the item at index `idx`. The returned pointer is owned by the list; do not free it.

### `vel_val_t vel_list_pack(vel_list_t list, int escape)`

Returns a packed string representation of the list. If `escape` is non-zero, items containing spaces or special characters are brace-quoted. Caller must free the returned value.

### Substitution

```c
vel_list_t vel_subst_list(vel_t vel, vel_val_t code);
vel_val_t  vel_subst_val (vel_t vel, vel_val_t code);
```

`vel_subst_list` tokenizes `code` (performing variable and bracket substitution) and returns the resulting word list.

`vel_subst_val` substitutes `code` and returns it as a single string value.

---

## Callbacks

Callbacks are set with:

```c
void vel_set_callback(vel_t vel, int slot, vel_cb_t proc);
```

The `slot` argument is one of the constants below. Cast your callback to `vel_cb_t` when registering.

### `VEL_CB_EXIT` (slot 0)

```c
typedef void (*vel_exit_cb_t)(vel_t vel, vel_val_t code);
```

Called when the vel `exit` command is executed. `code` is the exit value.

### `VEL_CB_WRITE` (slot 1)

```c
typedef void (*vel_write_cb_t)(vel_t vel, const char *msg);
```

Called for all output from `write`, `print`, `echo`, and related commands. Default behavior is `fputs(msg, stdout)`.

```c
static void my_write(vel_t vel, const char *msg) {
    (void)vel;
    my_buffer_append(msg);
}
vel_set_callback(vel, VEL_CB_WRITE, (vel_cb_t)my_write);
```

### `VEL_CB_READ` (slot 2)

```c
typedef char *(*vel_read_cb_t)(vel_t vel, const char *prompt);
```

Called by the `read` command. Return a heap-allocated string; vel will free it.

### `VEL_CB_STORE` (slot 3)

```c
typedef void (*vel_store_cb_t)(vel_t vel, const char *name, const char *data);
```

Called by the `store` command for key-value persistence.

### `VEL_CB_SOURCE` (slot 4)

```c
typedef char *(*vel_source_cb_t)(vel_t vel, const char *filename);
```

Called by the `source` command. Return the file contents as a heap-allocated string; vel will free it. Return NULL to signal that the file could not be found.

### `VEL_CB_ERROR` (slot 5)

```c
typedef void (*vel_error_cb_t)(vel_t vel, size_t pos, const char *msg);
```

Called when an error is set. `pos` is the byte offset in the source where the error occurred.

### `VEL_CB_SETVAR` (slot 6)

```c
typedef int (*vel_setvar_cb_t)(vel_t vel, const char *name, vel_val_t *val);
```

Called before a variable is set (for modes other than `VEL_VAR_LOCAL_NEW`). Return 0 to allow the assignment; return non-zero to suppress it. You may modify `*val` to intercept and transform the value.

### `VEL_CB_GETVAR` (slot 7)

```c
typedef int (*vel_getvar_cb_t)(vel_t vel, const char *name, vel_val_t *val);
```

Called when a variable lookup finds no binding. Set `*val` to provide a value; return non-zero to signal that you handled it.

### `VEL_CB_FILTER` (slot 8)

```c
typedef const char *(*vel_filter_cb_t)(vel_t vel, const char *msg);
```

Called by the template write path. Receives the string about to be output and returns a replacement string (or NULL to suppress output). The returned pointer does not need to be heap-allocated; it need only remain valid until the next call.

---

## Error Handling

### `void vel_error_set(vel_t vel, const char *msg)`

Sets an error with no specific position. Execution stops at the current statement.

### `void vel_error_set_at(vel_t vel, size_t pos, const char *msg)`

Sets an error at a specific source position.

### `int vel_error_get(vel_t vel, const char **msg, size_t *pos)`

Returns non-zero if an error is pending. Sets `*msg` to the error string and `*pos` to its source position. Both pointers are owned by the interpreter; do not free them. Returns 0 if no error.

```c
const char *err;
size_t pos;
if (vel_error_get(vel, &err, &pos)) {
    fprintf(stderr, "error at %zu: %s\n", pos, err);
}
```

---

## Expressions

### `vel_val_t vel_eval_expr(vel_t vel, vel_val_t code)`

Evaluates `code` as an arithmetic/logical expression after performing variable substitution. Returns the result (caller must free) or NULL on error.

---

## Environments

### `vel_env_t vel_env_new(vel_env_t parent)`

Creates a new environment with `parent` as its enclosing scope.

### `void vel_env_free(vel_env_t env)`

Frees an environment and all its variables. Do not call on the interpreter's live environments.

### `vel_env_t vel_env_push(vel_t vel)`

Pushes a new child environment onto the interpreter's scope stack and returns it.

### `void vel_env_pop(vel_t vel)`

Pops and frees the current scope, restoring the parent.

---

## Registering Native Commands

### `int vel_register(vel_t vel, const char *name, vel_fn_proc_t proc)`

Registers a native C function as a vel command. Returns 0 on success.

The function signature is:

```c
typedef vel_val_t (*vel_fn_proc_t)(vel_t vel, size_t argc, vel_val_t *argv);
```

- `argc` is the number of arguments (the command name itself is not included).
- `argv` is an array of argument values. Arguments are owned by the caller; do not free them.
- Return a new `vel_val_t` (caller takes ownership and will free it), or NULL for an empty result.

```c
static VELCB vel_val_t cmd_double(vel_t vel, size_t argc, vel_val_t *argv) {
    if (!argc) return NULL;
    vel_int_t n = vel_int(argv[0]);
    return vel_val_int(n * 2);
}
vel_register(vel, "double", cmd_double);
```

The `VELCB` macro applies the correct calling convention on Windows (`__stdcall`) and is empty on Unix.

---

## Template Engine

### `char *vel_template(vel_t vel, const char *src, unsigned int flags)`

Processes `src` as a template. Text outside `<?vel ... ?>` tags is written verbatim via the `VEL_CB_WRITE` callback. Code inside the tags is executed as vel script. Returns the accumulated output as a heap-allocated string. The caller must free it.

`flags` is currently unused; pass `VEL_TMPL_NONE` (0).

```c
char *out = vel_template(vel, "Hello <?vel write $name ?>!", VEL_TMPL_NONE);
printf("%s\n", out);
free(out);
```

---

## Diagnostics

### `void vel_stack_trace(vel_t vel, char *buf, size_t bufsz)`

Fills `buf` with a human-readable call stack trace showing up to `VEL_STACK_MAX` (256) frames. Each frame includes the function name and source position. The buffer is NUL-terminated. If no scripted frames are active, `buf[0]` is set to `\0`.

Call this after `vel_error_get` returns non-zero to provide context:

```c
char trace[2048];
vel_stack_trace(vel, trace, sizeof(trace));
if (trace[0])
    fputs(trace, stderr);
```

---

## Helper Macros

These macros reduce boilerplate in native command implementations.

### `VEL_CMD_RETURN(v)`

Returns `v` from a native command function.

### `VEL_CMD_RETURN_FREE(v, p)`

Frees heap pointer `p`, then returns `v`. For the common pattern of building a string in a temporary buffer.

```c
char *buf = malloc(64);
snprintf(buf, 64, "result=%d", n);
vel_val_t r = vel_val_str(buf);
VEL_CMD_RETURN_FREE(r, buf);
```

### `VEL_CMD_RETURN_NULL`

Returns NULL (empty result) from a native command.

### `VEL_CMD_ERROR(vel, msg)`

Sets an error with message `msg` and returns NULL.

```c
if (argc < 1)
    VEL_CMD_ERROR(vel, "expected at least one argument");
```

---

## Miscellaneous API

### `void vel_write(vel_t vel, const char *msg)`

Sends `msg` through the `VEL_CB_WRITE` callback.

### `vel_val_t vel_unused_name(vel_t vel, const char *hint)`

Returns a unique function name not currently registered. Used internally by anonymous function creation.

### `vel_val_t vel_arg(vel_val_t *argv, size_t idx)`

Returns `argv[idx]` or NULL if `argv` is NULL.

### `void vel_freemem(void *ptr)`

Frees a pointer that was allocated by the vel library and handed to the caller. Use this instead of `free()` when the pointer was returned by a vel API function that documents it as caller-owned and requires this specific free function (primarily relevant when vel is built as a DLL with its own heap on Windows).

---

## User Data

### `void vel_set_data(vel_t vel, void *data)`

Attaches an arbitrary pointer to the interpreter instance.

### `void *vel_get_data(vel_t vel)`

Retrieves the pointer previously set with `vel_set_data`. Returns NULL if none was set.

Use this to pass application context into native command callbacks:

```c
vel_set_data(vel, &my_app_context);

static VELCB vel_val_t cmd_foo(vel_t vel, size_t argc, vel_val_t *argv) {
    MyContext *ctx = vel_get_data(vel);
    // use ctx...
}
```

---

## Complete Embedding Example

```c
#include <stdio.h>
#include "vel.h"

static void on_write(vel_t vel, const char *msg) {
    (void)vel;
    fputs(msg, stdout);
}

static VELCB vel_val_t cmd_add(vel_t vel, size_t argc, vel_val_t *argv) {
    if (argc < 2) VEL_CMD_ERROR(vel, "add: need two arguments");
    vel_int_t a = vel_int(argv[0]);
    vel_int_t b = vel_int(argv[1]);
    return vel_val_int(a + b);
}

int main(void) {
    vel_t vel = vel_new();
    vel_set_callback(vel, VEL_CB_WRITE, (vel_cb_t)on_write);
    vel_register(vel, "add", cmd_add);

    vel_val_t result = vel_parse(vel, "set x [add 10 20]\nwrite $x\n", 0, 1);
    vel_val_free(result);

    const char *err;
    size_t pos;
    if (vel_error_get(vel, &err, &pos)) {
        fprintf(stderr, "error at %zu: %s\n", pos, err);
    }

    vel_free(vel);
    return 0;
}
```
