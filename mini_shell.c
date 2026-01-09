#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define MAX_TOKENS 64

void trim_newline(char *s) {
    size_t len = strlen(s);
    if (len > 0 && s[len - 1] == '\n') {
        s[len - 1] = '\0';
    }
}

int tokenize(char *line, char **argv) {
    int argc = 0;
    char *token = strtok(line, " \t");

    while (token && argc < MAX_TOKENS - 1) {
        argv[argc++] = token;
        token = strtok(NULL, " \t");
    }

    argv[argc] = NULL;
    return argc;
}

int handle_builtin(char **argv) {
    if (strcmp(argv[0], "exit") == 0) {
        return -1;
    }

    if (strcmp(argv[0], "cd") == 0) {
        const char *path = argv[1] ? argv[1] : getenv("HOME");
        if (!path) path = "/";
        if (chdir(path) != 0) {
            perror("cd");
        }
        return 1;
    }

    return 0;
}

void run_command(char **argv) {
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        return;
    }

    if (pid == 0) {
        execvp(argv[0], argv);
        perror("execvp");
        _exit(127);
    }

    int status;
    waitpid(pid, &status, 0);
}

int main(void) {
    char *line = NULL;
    size_t len = 0;
    char *argv[MAX_TOKENS];

    while (1) {
        printf("myshell> ");
        fflush(stdout);

        ssize_t nread = getline(&line, &len, stdin);
        if (nread < 0) break;

        trim_newline(line);
        if (line[0] == '\0') continue;

        char *copy = strdup(line);
        if (!copy) break;

        int argc = tokenize(copy, argv);
        if (argc == 0) {
            free(copy);
            continue;
        }

        int builtin = handle_builtin(argv);
        if (builtin == -1) {
            free(copy);
            break;
        } else if (builtin == 0) {
            run_command(argv);
        }

        free(copy);
    }

    free(line);
    printf("\n");
    return 0;
}


