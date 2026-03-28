# Vel Job Control and Signal Handling

This document describes vel's POSIX job control system, background process management, pipeline execution, and signal handling.

Job control is available on Unix-like systems (Linux, macOS, BSDs). All commands in this document are conditionally compiled out on Windows.

---

## Table of Contents

1. [Overview](#overview)
2. [Background Job Commands](#background-job-commands)
3. [Pipeline Commands](#pipeline-commands)
4. [Signal Handling](#signal-handling)
5. [Exit Code Tracking](#exit-code-tracking)
6. [External Command Execution Modes](#external-command-execution-modes)
7. [Job Table](#job-table)
8. [Limits and Configuration](#limits-and-configuration)
9. [Example Patterns](#example-patterns)

---

## Overview

Vel provides a built-in job control system modeled on Unix shell job control. Background jobs are tracked in a global job table. Each job has a numeric job ID (`jid`), a PID, a state (running, stopped, done), an exit code, and a display string.

The job table is initialized by `vel_jobs_init()` on interpreter startup and cleaned up by `vel_jobs_cleanup()` on shutdown.

Signal handling in vel is deferred: signal handlers only set a flag, and the actual vel code is executed from `vel_jobs_dispatch_signals()` which is called from the main loop after each command or script execution. This ensures vel code never runs from a signal handler context, avoiding the restrictions of async-signal-safety.

---

## Background Job Commands

### `spawn cmd [arg ...]`

Forks and executes `cmd` in the background. Returns the job ID (`jid`) as a string.

```vel
set jid [spawn sleep 10]
write "started job $jid"
```

### `bg cmd [arg ...]`

Alias for `spawn`.

### `wait [jid]`

Blocks until background job `jid` has finished. If `jid` is omitted or zero, waits for all background jobs. Returns the exit code.

```vel
set jid [spawn make clean]
wait $jid
```

### `waitall`

Waits for all currently running background jobs. Equivalent to `wait` with no argument.

### `fg [jid]`

Brings background job `jid` to the foreground. Blocks until the job finishes. If `jid` is omitted, brings the most recent background job to the foreground.

### `jobs`

Lists all background jobs with their job ID, PID, state, and command string. Output is written via the `VEL_CB_WRITE` callback.

```
[1] 12345 running  make
[2] 12350 done     sleep 5
```

### `bglist`

Returns a vel list of job IDs for currently active (non-done) background jobs.

```vel
set active [bglist]
write "active jobs: $active"
```

### `bgpid jid`

Returns the PID of background job `jid`.

```vel
set pid [bgpid 1]
```

### `killjob jid_or_pid [signal]`

Sends `signal` to the process identified by `jid_or_pid`. `jid_or_pid` is treated as a job ID if it is positive and falls within the job table; otherwise it is treated as a PID.

`signal` may be specified as a number or as a standard signal name without the `SIG` prefix: `TERM`, `KILL`, `HUP`, `INT`, `CONT`, `STOP`, `USR1`, `USR2`.

Default signal is `SIGTERM` if not specified.

```vel
killjob 1 KILL
killjob 9876 15
```

### `jobstatus jid`

Returns the exit code of a finished background job. Returns an error if the job is still running.

---

## Pipeline Commands

### `pipe {cmd1 [args ...]} {cmd2 [args ...]} ...`

Creates a Unix pipeline connecting the stdout of each stage to the stdin of the next. The final stage's stdout is captured and returned as a string.

Each argument to `pipe` is a vel list representing one pipeline stage. The first element of each list is the command name; subsequent elements are arguments.

```vel
set result [pipe {ls -l} {grep ".c"} {wc -l}]
write "number of .c files: $result"
```

`pipe` is implemented in `vel_sys.c` using `fork`, `exec`, and `dup2`. All intermediate pipe file descriptors are properly closed.

### `shpipe {cmd1 args} {cmd2 args} ...`

Like `pipe` but each stage is invoked via `/bin/sh -c` rather than direct `exec`. This allows each stage to use shell syntax (globs, shell builtins, etc.).

```vel
set result [shpipe {ls *.c} {grep "vel_"} {wc -l}]
```

### `sh {command string}`

Runs `command string` through `/bin/sh -c` and returns the captured output.

```vel
set files [sh "find . -name '*.c' | sort"]
```

---

## Signal Handling

### `sighandle SIGNAL {vel-code}`

Registers `vel-code` to be executed when the process receives `SIGNAL`. The signal name may be specified with or without the `SIG` prefix.

```vel
sighandle INT {
    write "interrupted"
    exit 1
}

sighandle USR1 {
    write "received USR1"
}
```

Supported signals: `HUP`, `INT`, `QUIT`, `TERM`, `USR1`, `USR2`, `WINCH`, `CONT`, and `CHLD`.

### How deferred signal dispatch works

When a signal is received:

1. The OS invokes the C signal handler registered by `vel_jobs.c`.
2. The C handler sets a `volatile sig_atomic_t` flag and records which signal was received.
3. The C handler does not execute any vel code.
4. After each command or script completes, the main loop calls `vel_jobs_dispatch_signals(vel)`.
5. `vel_jobs_dispatch_signals` checks the pending flags and calls `vel_parse` with the registered vel code for each pending signal.

This design ensures that vel code registered via `sighandle` runs in a safe context, not from a signal handler. It means signal handlers are not truly asynchronous: they run after the current statement completes.

### `SIGCHLD` handling

`SIGCHLD` is handled internally by `vel_jobs_reap()`. When a child process exits, the OS sends `SIGCHLD`. The handler sets a flag; `vel_jobs_reap()` calls `waitpid` in a non-blocking loop to collect exit codes and update the job table. This prevents zombie processes.

---

## Exit Code Tracking

The global variable `g_last_exit` (defined in `vel_sys.c`) records the exit code of the most recently completed external command. It is updated by:

- `run` (direct exec with passthrough stdio)
- `exec` (capture output)
- `pipe` (final stage)
- Auto-exec (unrecognized command run from PATH)
- `system` (with timeout)

Within vel scripts, the exit code is accessible via the `?` variable:

```vel
run make
if {$? != 0} {
    write "build failed"
    exit 1
}
```

Or via the `exitcode` command:

```vel
exec git status
set code [exitcode]
```

When `system` times out, `g_last_exit` is set to -1 to distinguish timeout from a normal non-zero exit.

---

## External Command Execution Modes

Vel provides three distinct modes for running external programs:

### Auto-exec (implicit)

When a command name is not registered as a vel built-in or user function but is found on `PATH`, vel forks and execs it directly with inherited stdin, stdout, and stderr. This is the "shell-like" mode.

```vel
make clean
cmake -DCMAKE_BUILD_TYPE=Release ..
```

The exit code is stored in `?`. No return value is produced.

### `run` (explicit, live output)

Explicitly runs an external command with inherited stdio. Returns the exit code as a string.

```vel
set code [run make -j4]
```

Use `run` when you need the exit code as a value, or when you want to be explicit.

### `exec` (capture output)

Runs an external command and captures its stdout as a string. stderr is not captured (it goes to the terminal unless redirected).

```vel
set branch [exec git rev-parse --abbrev-ref HEAD]
```

### `system` (capture with timeout)

Like `exec` but applies a configurable timeout (default 30 seconds). Returns an empty string on timeout. Exit code is stored in `g_last_exit`.

```vel
set output [system curl https://example.com]
```

---

## Job Table

The job table is a global fixed-size array of up to `VEL_MAX_JOBS` (256) entries. Each entry has:

| Field | Type | Description |
|-------|------|-------------|
| `jid` | int | 1-based job ID, reused after job is done |
| `pid` | pid_t | Process ID |
| `state` | int | `VJOB_RUNNING` (0), `VJOB_STOPPED` (1), `VJOB_DONE` (2) |
| `exit_code` | int | Exit code once done |
| `cmd` | char* | Display string (copy of the command) |
| `pgid` | int | Process group ID |

Job IDs are assigned by finding the first unused slot. After a job finishes and its exit code has been collected, its slot can be reused.

---

## Limits and Configuration

| Item | Value | Notes |
|------|-------|-------|
| `VEL_MAX_JOBS` | 256 | Maximum concurrent background jobs |
| `VEL_SYSTEM_TIMEOUT_SEC` | 30 | Default timeout for `system` command (seconds) |
| `VEL_SYSTEM_TIMEOUT` env var | override | Set to 0 to disable timeout |

---

## Example Patterns

### Run a build in the background and wait

```vel
set jid [spawn make -j8]
write "building in background (job $jid)..."
wait $jid
if {$? != 0} {
    write "build failed"
    exit 1
}
write "build successful"
```

### Parallel jobs with waitall

```vel
spawn make -C module_a
spawn make -C module_b
spawn make -C module_c
waitall
write "all modules built"
```

### Pipeline: count matching lines

```vel
set count [pipe {grep -r "TODO" src/} {wc -l}]
write "TODO items: $count"
```

### Handle SIGINT for cleanup

```vel
sighandle INT {
    write "cleaning up..."
    remove /tmp/myapp.lock
    exit 1
}

# main work
touch /tmp/myapp.lock
run ./long_process
remove /tmp/myapp.lock
```

### Capture git log

```vel
set log [exec git log --oneline -10]
for line [split $log "\n"] {
    write $line
}
```
