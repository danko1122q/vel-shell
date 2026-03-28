# Vel Template Engine

This document describes the vel embedded template engine.

---

## Table of Contents

1. [Overview](#overview)
2. [Tag Syntax](#tag-syntax)
3. [C API](#c-api)
4. [Output Filtering](#output-filtering)
5. [Brace Escaping](#brace-escaping)
6. [Nesting and State](#nesting-and-state)
7. [Examples](#examples)

---

## Overview

The template engine processes a source string that contains a mix of literal text and vel code blocks. Text outside vel blocks is emitted verbatim; code inside vel blocks is executed. The combined output is returned as a single string.

This is similar in concept to PHP, ERB (Ruby), or JSP, but uses the vel scripting language inside the tags.

---

## Tag Syntax

Vel code blocks are delimited by `<?vel` and `?>`:

```
<?vel code ?>
```

Everything outside these tags is literal text and is written as-is to the output. Everything inside is executed as a vel script. Output produced by `write` or `print` inside a code block is included in the template output at that position.

Example:

```
Hello <?vel write $name ?>!
Your score is <?vel write [expr $score * 100] ?>.
```

If `name` is `"Alice"` and `score` is `0.95`, the output is:

```
Hello Alice!
Your score is 95.
```

The opening tag must be exactly `<?vel` (lowercase, no space between `<?` and `vel`).

---

## C API

### `char *vel_template(vel_t vel, const char *src, unsigned int flags)`

Processes `src` as a template string. Returns the complete output as a heap-allocated NUL-terminated string. The caller must free this string with `free()` (or `vel_freemem()` when using the DLL).

`flags` is reserved for future use. Pass `VEL_TMPL_NONE` (0).

```c
vel_var_set(vel, "name", vel_val_str("Alice"), VEL_VAR_GLOBAL);

char *output = vel_template(vel, "Hello <?vel write $name ?>!", VEL_TMPL_NONE);
printf("%s\n", output);  // Hello Alice!
free(output);
```

The template is executed using the interpreter's current state. Variables set before calling `vel_template` are accessible inside the template. Variables set inside the template persist after it returns.

If no output is produced (e.g., the template contains only code with no `write` calls), the function returns an empty string, never NULL.

---

## Output Filtering

If the `VEL_CB_FILTER` callback is set on the interpreter, it is called for each string that the template engine is about to write to the output buffer. The callback receives the string and returns either a replacement string (which is used instead) or NULL (which suppresses the output entirely).

```c
static const char *html_escape(vel_t vel, const char *msg) {
    // return an HTML-escaped version of msg
    // (the returned pointer must remain valid until the next call)
    static char buf[4096];
    // ... escape < > & " into &lt; &gt; &amp; &quot; ...
    return buf;
}

vel_set_callback(vel, VEL_CB_FILTER, (vel_cb_t)html_escape);
char *html = vel_template(vel, src, VEL_TMPL_NONE);
```

This allows the host application to apply output encoding (HTML escaping, XML escaping, etc.) to all template output automatically.

---

## Brace Escaping

Inside the literal text sections of a template, `{` and `}` have special meaning in vel string literals. The template engine escapes them before generating the internal `write "..."` statements:

- `{` in literal text is encoded as `\o` in the generated write statement.
- `}` in literal text is encoded as `\c` in the generated write statement.

These escape sequences are processed by the vel lexer when reading double-quoted strings, so they are transparently converted back to `{` and `}` in the output.

This means template text may freely contain literal `{` and `}` characters:

```
<?vel set x 42 ?>
Value is: {<?vel write $x ?>}
```

Output:

```
Value is: {42}
```

Similarly, double-quote characters in literal text are escaped as `\"` in the generated code.

---

## Nesting and State

The template engine saves and restores the interpreter's write callback and output buffer before and after execution. This means:

- Template calls can be nested (a vel code block inside a template can call `vel_template` again).
- Each nested call has its own output buffer.
- The write callback installed by the outer template is saved and restored correctly.

Variables and functions set or defined during template execution persist in the interpreter state after `vel_template` returns, just as they would after any `vel_parse` call.

---

## Examples

### Simple variable substitution

```
Hello, <?vel write $username ?>. You have <?vel write $message_count ?> messages.
```

### Conditional output

```
<?vel if {$logged_in} { ?>
Welcome back, <?vel write $username ?>.
<?vel } else { ?>
Please log in.
<?vel } ?>
```

### Loop

```
Items:
<?vel for item $cart { ?>
  - <?vel write $item ?>
<?vel } ?>
```

### Computed value

```
Total: <?vel write [format "%.2f" [expr $price * $quantity]] ?>
```

### Using a filter for HTML escaping

Given a filter callback that escapes HTML special characters, template output is automatically safe:

```
<p><?vel write $user_input ?></p>
```

The filter receives the value of `$user_input` and can replace `<` with `&lt;`, etc., before it is included in the output buffer.

### Generating a file from a template

```c
vel_var_set(vel, "title", vel_val_str("My Report"), VEL_VAR_GLOBAL);
vel_var_set(vel, "body",  vel_val_str("Content here."), VEL_VAR_GLOBAL);

const char *tmpl =
    "<!DOCTYPE html>\n"
    "<html><head><title><?vel write $title ?></title></head>\n"
    "<body><?vel write $body ?></body></html>\n";

char *html = vel_template(vel, tmpl, VEL_TMPL_NONE);
FILE *f = fopen("output.html", "w");
fputs(html, f);
fclose(f);
free(html);
```
