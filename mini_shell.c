#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_LINE     4096
#define MAX_TOKENS   256
#define MAX_CMDS     32
#define HISTORY_SIZE 50
#define JOBS_MAX     128

typedef struct {
    pid_t pgid;
    char  cmdline[MAX_LINE];
    int   running;
} Job;

static Job jobs[JOBS_MAX];
static int jobs_count = 0;
static volatile sig_atomic_t fg_pgid = 0;

static char history[HISTORY_SIZE][MAX_LINE];
static int hist_count = 0;

static void add_history(const char *line) {
    if (!line || !*line) return;
    snprintf(history[hist_count % HISTORY_SIZE], MAX_LINE, "%s", line);
    hist_count++;
}

static void print_history(void) {
    int start = (hist_count > HISTORY_SIZE) ? (hist_count - HISTORY_SIZE) : 0;
    for (int i = start; i < hist_count; i++) {
        printf("%d  %s\n", i + 1, history[i % HISTORY_SIZE]);
    }
}

static void on_sigint(int signo) {
    (void)signo;
    if (fg_pgid > 0) kill(-fg_pgid, SIGINT);
}

static void on_sigchld(int signo) {
    (void)signo;
    int saved = errno;

    while (1) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0) break;
    }

    for (int i = 0; i < jobs_count; i++) {
        if (!jobs[i].running) continue;
        if (kill(-jobs[i].pgid, 0) == -1 && errno == ESRCH) {
            jobs[i].running = 0;
        }
    }

    errno = saved;
}

static void trim_newline(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    if (n && s[n - 1] == '\n') s[n - 1] = '\0';
}

static void skip_ws(const char **p) {
    while (**p == ' ' || **p == '\t') (*p)++;
}

static char *expand_vars(const char *tok) {
    if (!strchr(tok, '$')) return strdup(tok);

    char out[MAX_LINE];
    size_t oi = 0;

    for (size_t i = 0; tok[i] && oi + 1 < sizeof(out); ) {
        if (tok[i] == '$') {
            i++;
            if (tok[i] == '{') {
                i++;
                char name[256];
                size_t ni = 0;
                while (tok[i] && tok[i] != '}' && ni + 1 < sizeof(name)) {
                    name[ni++] = tok[i++];
                }
                name[ni] = '\0';
                if (tok[i] == '}') i++;

                const char *val = getenv(name);
                if (!val) val = "";
                for (size_t k = 0; val[k] && oi + 1 < sizeof(out); k++) out[oi++] = val[k];
            } else {
                char name[256];
                size_t ni = 0;
                while (tok[i] && (isalnum((unsigned char)tok[i]) || tok[i] == '_') && ni + 1 < sizeof(name)) {
                    name[ni++] = tok[i++];
                }
                name[ni] = '\0';

                const char *val = getenv(name);
                if (!val) val = "";
                for (size_t k = 0; val[k] && oi + 1 < sizeof(out); k++) out[oi++] = val[k];
            }
        } else {
            out[oi++] = tok[i++];
        }
    }

    out[oi] = '\0';
    return strdup(out);
}

typedef struct {
    char *toks[MAX_TOKENS];
    int   ntok;
} Tokens;

static void free_tokens(Tokens *ts) {
    for (int i = 0; i < ts->ntok; i++) free(ts->toks[i]);
    ts->ntok = 0;
}

static int push_tok(Tokens *ts, char *s) {
    if (ts->ntok >= MAX_TOKENS - 1) {
        free(s);
        return -1;
    }
    ts->toks[ts->ntok++] = s;
    ts->toks[ts->ntok] = NULL;
    return 0;
}

static int tokenize_line(const char *line, Tokens *out) {
    out->ntok = 0;
    const char *p = line;

    while (*p) {
        skip_ws(&p);
        if (*p == '\0') break;

        if (*p == '|') { if (push_tok(out, strdup("|")) < 0) return -1; p++; continue; }
        if (*p == '&') { if (push_tok(out, strdup("&")) < 0) return -1; p++; continue; }
        if (*p == '<') { if (push_tok(out, strdup("<")) < 0) return -1; p++; continue; }
        if (*p == '>') {
            if (*(p + 1) == '>') { if (push_tok(out, strdup(">>")) < 0) return -1; p += 2; }
            else { if (push_tok(out, strdup(">")) < 0) return -1; p++; }
            continue;
        }

        if (*p == '"') {
            p++;
            const char *start = p;
            while (*p && *p != '"') p++;
            size_t len = (size_t)(p - start);

            char *raw = (char *)malloc(len + 1);
            if (!raw) return -1;
            memcpy(raw, start, len);
            raw[len] = '\0';
            if (*p == '"') p++;

            char *expanded = expand_vars(raw);
            free(raw);
            if (!expanded) return -1;
            if (push_tok(out, expanded) < 0) return -1;
            continue;
        }

        const char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '|' && *p != '&' && *p != '<' && *p != '>') p++;
        size_t len = (size_t)(p - start);

        char *raw = (char *)malloc(len + 1);
        if (!raw) return -1;
        memcpy(raw, start, len);
        raw[len] = '\0';

        char *expanded = expand_vars(raw);
        free(raw);
        if (!expanded) return -1;
        if (push_tok(out, expanded) < 0) return -1;
    }

    return 0;
}

static int expand_globs(char **argv_in, char **argv_out, int out_cap) {
    int outc = 0;

    for (int i = 0; argv_in[i]; i++) {
        const char *t = argv_in[i];
        int has_glob = strpbrk(t, "*?[]") != NULL;

        if (!has_glob) {
            if (outc >= out_cap - 1) return -1;
            argv_out[outc++] = argv_in[i];
            continue;
        }

        glob_t g;
        memset(&g, 0, sizeof(g));
        int rc = glob(t, 0, NULL, &g);

        if (rc == 0) {
            for (size_t k = 0; k < g.gl_pathc; k++) {
                if (outc >= out_cap - 1) { globfree(&g); return -1; }
                argv_out[outc++] = strdup(g.gl_pathv[k]);
            }
        } else {
            if (outc >= out_cap - 1) { globfree(&g); return -1; }
            argv_out[outc++] = argv_in[i];
        }

        globfree(&g);
    }

    argv_out[outc] = NULL;
    return outc;
}

typedef struct {
    char *argv[MAX_TOKENS];
    char *in_file;
    char *out_file;
    int   append;
} Cmd;

typedef struct {
    Cmd cmds[MAX_CMDS];
    int ncmd;
    int background;
} Pipeline;

static void init_cmd(Cmd *c) {
    memset(c, 0, sizeof(*c));
}

static void free_pipeline(Pipeline *pl) {
    for (int i = 0; i < pl->ncmd; i++) {
        for (int j = 0; pl->cmds[i].argv[j]; j++) free(pl->cmds[i].argv[j]);
        free(pl->cmds[i].in_file);
        free(pl->cmds[i].out_file);
    }
    pl->ncmd = 0;
}

static int parse_pipeline(Tokens *ts, Pipeline *pl) {
    pl->ncmd = 0;
    pl->background = 0;

    Cmd cur;
    init_cmd(&cur);
    int argc = 0;

    for (int i = 0; i < ts->ntok; i++) {
        char *t = ts->toks[i];

        if (strcmp(t, "&") == 0) {
            pl->background = 1;
            continue;
        }

        if (strcmp(t, "|") == 0) {
            if (argc == 0) return -1;
            cur.argv[argc] = NULL;
            pl->cmds[pl->ncmd++] = cur;
            if (pl->ncmd >= MAX_CMDS) return -1;
            init_cmd(&cur);
            argc = 0;
            continue;
        }

        if (strcmp(t, "<") == 0) {
            if (i + 1 >= ts->ntok) return -1;
            i++;
            cur.in_file = strdup(ts->toks[i]);
            continue;
        }

        if (strcmp(t, ">") == 0 || strcmp(t, ">>") == 0) {
            if (i + 1 >= ts->ntok) return -1;
            cur.append = (strcmp(t, ">>") == 0);
            i++;
            cur.out_file = strdup(ts->toks[i]);
            continue;
        }

        cur.argv[argc++] = strdup(t);
        if (argc >= MAX_TOKENS - 1) return -1;
    }

    if (argc == 0) return -1;
    cur.argv[argc] = NULL;
    pl->cmds[pl->ncmd++] = cur;

    return 0;
}

static int builtin_jobs(void) {
    if (jobs_count == 0) {
        printf("No jobs.\n");
        return 1;
    }
    for (int i = 0; i < jobs_count; i++) {
        printf("[%d] %s  (%s)\n", i + 1, jobs[i].cmdline, jobs[i].running ? "running" : "done");
    }
    return 1;
}

static int handle_builtin_single(char **argv) {
    if (!argv[0]) return 1;

    if (strcmp(argv[0], "exit") == 0) return -1;

    if (strcmp(argv[0], "cd") == 0) {
        const char *path = argv[1] ? argv[1] : getenv("HOME");
        if (!path) path = "/";
        if (chdir(path) != 0) perror("cd");
        return 1;
    }

    if (strcmp(argv[0], "pwd") == 0) {
        char buf[4096];
        if (!getcwd(buf, sizeof(buf))) perror("pwd");
        else printf("%s\n", buf);
        return 1;
    }

    if (strcmp(argv[0], "history") == 0) {
        print_history();
        return 1;
    }

    if (strcmp(argv[0], "jobs") == 0) {
        return builtin_jobs();
    }

    return 0;
}

static int open_in(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) perror("open <");
    return fd;
}

static int open_out(const char *path, int append) {
    int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
    int fd = open(path, flags, 0644);
    if (fd < 0) perror("open >");
    return fd;
}

static void remember_job(pid_t pgid, const char *cmdline) {
    if (jobs_count >= JOBS_MAX) return;
    jobs[jobs_count].pgid = pgid;
    jobs[jobs_count].running = 1;
    snprintf(jobs[jobs_count].cmdline, sizeof(jobs[jobs_count].cmdline), "%s", cmdline);
    jobs_count++;
}

static int run_pipeline(Pipeline *pl, const char *cmdline) {
    int n = pl->ncmd;
    int pipes[MAX_CMDS - 1][2];

    for (int i = 0; i < n - 1; i++) {
        if (pipe(pipes[i]) < 0) { perror("pipe"); return -1; }
    }

    pid_t pgid = 0;

    for (int i = 0; i < n; i++) {
        char *argv_globbed[MAX_TOKENS];
        memset(argv_globbed, 0, sizeof(argv_globbed));
        if (expand_globs(pl->cmds[i].argv, argv_globbed, MAX_TOKENS) < 0) {
            fprintf(stderr, "globbing: too many args\n");
            return -1;
        }

        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return -1; }

        if (pid == 0) {
            if (pgid == 0) pgid = getpid();
            setpgid(0, pgid);
            signal(SIGINT, SIG_DFL);

            if (i > 0) dup2(pipes[i - 1][0], STDIN_FILENO);
            if (i < n - 1) dup2(pipes[i][1], STDOUT_FILENO);

            if (pl->cmds[i].in_file) {
                int fd = open_in(pl->cmds[i].in_file);
                if (fd < 0) _exit(1);
                dup2(fd, STDIN_FILENO);
                close(fd);
            }
            if (pl->cmds[i].out_file) {
                int fd = open_out(pl->cmds[i].out_file, pl->cmds[i].append);
                if (fd < 0) _exit(1);
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }

            for (int k = 0; k < n - 1; k++) {
                close(pipes[k][0]);
                close(pipes[k][1]);
            }

            execvp(argv_globbed[0], argv_globbed);
            perror("execvp");
            _exit(127);
        }

        if (pgid == 0) pgid = pid;
        setpgid(pid, pgid);
    }

    for (int i = 0; i < n - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    if (pl->background) {
        remember_job(pgid, cmdline);
        printf("[bg] pgid=%d  %s\n", (int)pgid, cmdline);
        return 0;
    }

    fg_pgid = pgid;
    int status = 0;
    while (1) {
        pid_t w = waitpid(-pgid, &status, 0);
        if (w < 0) {
            if (errno == EINTR) continue;
            break;
        }
    }
    fg_pgid = 0;

    return 0;
}

int main(void) {
    struct sigaction sa_int;
    memset(&sa_int, 0, sizeof(sa_int));
    sa_int.sa_handler = on_sigint;
    sigemptyset(&sa_int.sa_mask);
    sa_int.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa_int, NULL);

    struct sigaction sa_chld;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = on_sigchld;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);

    char line[MAX_LINE];

    while (1) {
        printf("myshell> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            printf("\nexit\n");
            break;
        }
        trim_newline(line);

        const char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0') continue;

        add_history(line);

        Tokens ts;
        if (tokenize_line(line, &ts) < 0) {
            fprintf(stderr, "tokenize error\n");
            continue;
        }
        if (ts.ntok == 0) { free_tokens(&ts); continue; }

        Pipeline pl;
        memset(&pl, 0, sizeof(pl));
        if (parse_pipeline(&ts, &pl) < 0) {
            fprintf(stderr, "parse error\n");
            free_tokens(&ts);
            continue;
        }
        free_tokens(&ts);

        if (pl.ncmd == 1 && !pl.background && !pl.cmds[0].in_file && !pl.cmds[0].out_file) {
            int b = handle_builtin_single(pl.cmds[0].argv);
            if (b == -1) { free_pipeline(&pl); break; }
            if (b == 1) { free_pipeline(&pl); continue; }
        }

        run_pipeline(&pl, line);
        free_pipeline(&pl);
    }

    return 0;
}
