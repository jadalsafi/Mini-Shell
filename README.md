# Mini Shell (C, Linux)

A Unix-style mini shell built in C with support for process execution, 
pipes, redirection, and basic job control.

## Features
- Built-ins: cd, pwd, history, jobs, exit
- Quoted arguments: echo "hello world"
- Pipes: cmd1 | cmd2
- Redirection: <, >, >>
- Background jobs: &
- Ctrl+C forwards to the running command (shell stays alive)
- Basic env var expansion: $HOME
- Globbing: *.c

## Build
```bash
clang -Wall -Wextra -Werror -std=c11 mini_shell.c -o myshell

