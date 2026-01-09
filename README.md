# Mini Shell in C

A Unix-style command-line shell implemented in C.

## Features
- Execute external commands using fork() and execvp()
- Built-in commands: cd, exit
- Tokenized command parsing
- Process management with waitpid()

## Build & Run
clang -Wall -Wextra mini_shell.c -o myshell
./myshell

