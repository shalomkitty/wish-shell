# wish — A Simple Unix Shell (OSTEP)

A **C (POSIX)** implementation of the OSTEP Unix Shell project.

## Requirements

- Linux or WSL2 (on Windows).
- A C compiler (GCC or Clang) and `make`.
- Python 3 (only needed to run the automated tests).

## Build and Run

From the project directory, run:

```bash
make
./wish
```

In interactive mode, the shell displays the `wish> ` prompt. To quit, enter `exit` or press Ctrl+D (EOF).

To run commands from a file in batch mode:

```bash
./wish examples.txt
```

The `wish> ` prompt is **not displayed** in batch mode. If the file cannot be opened or the program receives more than one command-line argument, the shell prints the standard error message and exits with status code 1.

## Features

| Feature | Implementation |
|---|---|
| Execute external programs | Uses `fork()`, `execv()`, and `waitpid()` |
| Executable search path | Initially contains only `/bin` |
| `exit` | Exits the shell; does not accept arguments |
| `cd DIR` | Changes the working directory using `chdir()`; requires exactly one argument |
| `path [DIR ...]` | Replaces the list of directories used to locate executables; `path` without arguments clears the list |
| `cmd > file` | Redirects **both stdout and stderr** to the same file using `open()` and `dup2()`; overwrites an existing file |
| `cmd1 & cmd2` | Starts external commands concurrently and waits for all child processes to finish |
| Error handling | Prints only `An error has occurred` to `stderr` |
| Command parsing | Accepts spaces and tabs, including `>` and `&` without surrounding whitespace |

This is an educational Unix shell, not a replacement for Bash. The assignment does not require quoting, pipelines (`|`), command history, environment-variable expansion, wildcard expansion (`*`), or executing `./program` outside the configured search path.

**Implementation notes:** Redirection for built-in commands is not required by the assignment, so this implementation treats it as an error. If a line separated by `&` contains built-in commands, they run in the main shell process in their written order; external commands start without waiting for earlier external commands to finish.

## Example Session

```text
$ ./wish
wish> pwd
/home/student/wish-shell
wish> cd /tmp
wish> pwd
/tmp
wish> path /bin /usr/bin
wish> echo hello > message.txt
wish> cat message.txt
hello
wish> sleep 1 & echo done
done
wish> exit
```

The output order of concurrent commands is not guaranteed.

## Automated Tests

Run the test suite with:

```bash
make test
```

The 25 tests cover interactive and batch modes, `exit`, `cd`, `path`, redirection of both output streams, concurrent execution, operators without surrounding spaces, error handling, and long input lines. Tests use temporary directories and do not modify project files.

Remove the compiled executable with:

```bash
make clean
```

## How It Works

1. `getline()` reads a line from the terminal or batch file.
2. The lexer separates words from the `>` and `&` operators.
3. The parser validates each command's syntax and constructs its `argv` array.
4. Built-in commands execute in the main shell process.
5. For external commands, the shell searches the directories in `path` using `access(..., X_OK)`, then calls `fork()` to create a child process.
6. If redirection is requested, the child opens the output file and redirects `stdout` and `stderr` with `dup2()`. It then starts the requested program with `execv()`.
7. After starting all commands on the line, the parent process waits for each child using `waitpid()`.

## Running on Windows

`fork()`, `execv()`, and `waitpid()` are POSIX APIs, so this source code cannot be compiled as a native Windows executable. Open an Ubuntu terminal in WSL, navigate to the project directory, and run `make`.
