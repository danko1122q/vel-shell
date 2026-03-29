# vel

Vel is a lightweight, embeddable scripting language interpreter written in C99. It is designed as a shell-like command language with a Tcl-inspired syntax, built-in POSIX job control, a template engine, and a clean C embedding API.

## Features

- Tcl-inspired command/word syntax
- First-class functions with closures via lexical scoping
- Reference-counted string values with copy-on-write semantics
- Built-in POSIX job control: background jobs, pipelines, signal handling
- Embedded template engine with `<?vel ... ?>` tags
- Auto-execution of external programs found on `PATH`
- Shebang (`#!/usr/bin/env vel`) support
- Expression evaluator with integer overflow detection
- Readline support (optional, compile-time flag)
- Clean public C API for embedding in other applications
- Cross-platform: Linux, macOS, and partial Windows support

## Requirements

- C11-compatible compiler (GCC or Clang recommended)
- POSIX-compatible system (Linux, macOS, BSDs)
- GNU Make
- `libm` (standard on all Unix systems)
- Optional: `libreadline` for interactive history and line editing

## Building

```sh
make
```

With readline support:

```sh
make READLINE=1
```

## Installation

```sh
sudo make install
```

Installs the `vel` binary to `/usr/local/bin`. Override the prefix:

```sh
sudo make install PREFIX=/usr
```

## Usage

Start the interactive REPL:

```sh
vel
```

Run a script file:

```sh
vel script.vel
```

Run from stdin (pipe):

```sh
echo "write hello" | vel
```

Use as a shebang interpreter:

```vel
#!/usr/bin/env vel
write "hello from vel"
```

## Quick Example

```vel
set name "world"
write "hello $name"

func greet {who} {
    write "greetings, $who"
}
greet "user"

for i {1 2 3} {
    write $i
}
```

## Documentation

Detailed documentation is provided in the `docs/` directory:

| File | Description |
|---|---|
| [docs/LANGUAGE.md](docs/LANGUAGE.md) | Language syntax, data types, and built-in commands |
| [docs/EMBEDDING.md](docs/EMBEDDING.md) | C API reference for embedding vel in other programs |
| [docs/INTERNALS.md](docs/INTERNALS.md) | Architecture and internal implementation details |
| [docs/COMPARISON.md](docs/COMPARISON.md) | Comparison with standard Tcl and gap analysis |
| [docs/JOBCONTROL.md](docs/JOBCONTROL.md) | POSIX job control and signal handling reference |
| [docs/TEMPLATE.md](docs/TEMPLATE.md) | Embedded template engine reference |

## License

See LICENSE file.
