#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <readline/history.h>
#include <readline/readline.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

extern char **environ;

#define MAX_PIPES 64

typedef struct {
    char **argv;
    int argc;
    char *infile;
    char *outfile;
    char *errfile;
    int out_append;
    int err_append;
    int valid;
} command_t;

static char oldpwd[PATH_MAX] = "";

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;

    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        end--;

    *end = '\0';
    return s;
}

static void reap_children(int sig)
{
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
}

static const char *prompt_dir(void)
{
    static char out[PATH_MAX];
    char cwd[PATH_MAX];

    if (!getcwd(cwd, sizeof(cwd))) {
        snprintf(out, sizeof(out), "?");
        return out;
    }

    const char *home = getenv("HOME");
    if (home && *home) {
        size_t home_len = strlen(home);
        if (strncmp(cwd, home, home_len) == 0 &&
            (cwd[home_len] == '\0' || cwd[home_len] == '/')) {
            if (cwd[home_len] == '\0')
                snprintf(out, sizeof(out), "~");
            else
                snprintf(out, sizeof(out), "~%s", cwd + home_len);
            return out;
        }
    }

    snprintf(out, sizeof(out), "%s", cwd);
    return out;
}

static void make_prompt(char *buf, size_t size)
{
    snprintf(buf, size, "%s %c ", prompt_dir(), geteuid() == 0 ? '#' : '$');
}

static int has_top_level_pipe(const char *line)
{
    int in_s = 0, in_d = 0, esc = 0;

    for (const char *p = line; *p; p++) {
        char c = *p;

        if (esc) {
            esc = 0;
            continue;
        }

        if (!in_s && c == '\\') {
            esc = 1;
            continue;
        }

        if (!in_d && c == '\'') {
            in_s = !in_s;
            continue;
        }

        if (!in_s && c == '"') {
            in_d = !in_d;
            continue;
        }

        if (!in_s && !in_d && c == '|')
            return 1;
    }

    return 0;
}

static int strip_background_ampersand(char *line)
{
    int in_s = 0, in_d = 0, esc = 0;
    char *last_unquoted_nonspace = NULL;

    for (char *p = line; *p; p++) {
        char c = *p;

        if (esc) {
            esc = 0;
        } else if (!in_s && c == '\\') {
            esc = 1;
            continue;
        } else if (!in_d && c == '\'') {
            in_s = !in_s;
            continue;
        } else if (!in_s && c == '"') {
            in_d = !in_d;
            continue;
        }

        if (!in_s && !in_d && !isspace((unsigned char)c))
            last_unquoted_nonspace = p;
    }

    if (last_unquoted_nonspace && *last_unquoted_nonspace == '&') {
        *last_unquoted_nonspace = '\0';
        return 1;
    }

    return 0;
}

static char *next_token(char **sp)
{
    char *s = *sp;

    while (*s && isspace((unsigned char)*s))
        s++;

    if (!*s) {
        *sp = s;
        return NULL;
    }

    if (s[0] == '2' && s[1] == '>' && s[2] == '>') {
        *sp = s + 3;
        return strdup("2>>");
    }
    if (s[0] == '2' && s[1] == '>') {
        *sp = s + 2;
        return strdup("2>");
    }
    if (s[0] == '>' && s[1] == '>') {
        *sp = s + 2;
        return strdup(">>");
    }
    if (s[0] == '<') {
        *sp = s + 1;
        return strdup("<");
    }
    if (s[0] == '>') {
        *sp = s + 1;
        return strdup(">");
    }

    char buf[8192];
    size_t len = 0;
    int in_s = 0, in_d = 0, esc = 0;

    while (*s) {
        char c = *s;

        if (!in_s && !in_d && !esc) {
            if (isspace((unsigned char)c) || c == '|' || c == '<' || c == '>')
                break;
        }

        if (!in_s && c == '\\' && !esc) {
            esc = 1;
            s++;
            continue;
        }

        if (!esc && c == '\'' && !in_d) {
            in_s = !in_s;
            s++;
            continue;
        }

        if (!esc && c == '"' && !in_s) {
            in_d = !in_d;
            s++;
            continue;
        }

        if (len + 1 < sizeof(buf))
            buf[len++] = c;

        esc = 0;
        s++;
    }

    buf[len] = '\0';
    *sp = s;
    return strdup(buf);
}

static int argv_push(char ***argv, int *argc, int *cap, char *item)
{
    if (*argc + 1 >= *cap) {
        int new_cap = (*cap) * 2;
        char **tmp = realloc(*argv, sizeof(char *) * new_cap);
        if (!tmp)
            return -1;
        *argv = tmp;
        *cap = new_cap;
    }

    (*argv)[(*argc)++] = item;
    (*argv)[*argc] = NULL;
    return 0;
}

static command_t parse_command(char *segment)
{
    command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.valid = 1;

    int cap = 8;
    cmd.argv = malloc(sizeof(char *) * cap);
    if (!cmd.argv) {
        cmd.valid = 0;
        return cmd;
    }
    cmd.argv[0] = NULL;

    char *p = segment;
    char *tok;

    while ((tok = next_token(&p)) != NULL) {
        if (strcmp(tok, "<") == 0) {
            free(tok);
            tok = next_token(&p);
            if (!tok) {
                fprintf(stderr, "syntax error near '<'\n");
                cmd.valid = 0;
                break;
            }
            cmd.infile = tok;
            continue;
        }

        if (strcmp(tok, ">") == 0) {
            free(tok);
            tok = next_token(&p);
            if (!tok) {
                fprintf(stderr, "syntax error near '>'\n");
                cmd.valid = 0;
                break;
            }
            cmd.outfile = tok;
            cmd.out_append = 0;
            continue;
        }

        if (strcmp(tok, ">>") == 0) {
            free(tok);
            tok = next_token(&p);
            if (!tok) {
                fprintf(stderr, "syntax error near '>>'\n");
                cmd.valid = 0;
                break;
            }
            cmd.outfile = tok;
            cmd.out_append = 1;
            continue;
        }

        if (strcmp(tok, "2>") == 0) {
            free(tok);
            tok = next_token(&p);
            if (!tok) {
                fprintf(stderr, "syntax error near '2>'\n");
                cmd.valid = 0;
                break;
            }
            cmd.errfile = tok;
            cmd.err_append = 0;
            continue;
        }

        if (strcmp(tok, "2>>") == 0) {
            free(tok);
            tok = next_token(&p);
            if (!tok) {
                fprintf(stderr, "syntax error near '2>>'\n");
                cmd.valid = 0;
                break;
            }
            cmd.errfile = tok;
            cmd.err_append = 1;
            continue;
        }

        if (argv_push(&cmd.argv, &cmd.argc, &cap, tok) != 0) {
            free(tok);
            cmd.valid = 0;
            break;
        }
    }

    if (cmd.argc == 0)
        cmd.argv[0] = NULL;

    return cmd;
}

static void free_command(command_t *cmd)
{
    if (cmd->argv) {
        for (int i = 0; i < cmd->argc; i++)
            free(cmd->argv[i]);
        free(cmd->argv);
    }

    free(cmd->infile);
    free(cmd->outfile);
    free(cmd->errfile);
    memset(cmd, 0, sizeof(*cmd));
}

static char *resolve_path(const char *cmd)
{
    if (!cmd || !*cmd)
        return NULL;

    if (strchr(cmd, '/'))
        return strdup(cmd);

    const char *path_env = getenv("PATH");
    const char *fallback = "/bin:/usr/bin";
    char *paths = strdup((path_env && *path_env) ? path_env : fallback);
    if (!paths)
        return NULL;

    char *saveptr = NULL;
    char *dir = strtok_r(paths, ":", &saveptr);

    while (dir) {
        char *full = NULL;

        if (dir[0] == '\0') {
            size_t len = strlen(cmd) + 3;
            full = malloc(len);
            if (!full) {
                free(paths);
                return NULL;
            }
            snprintf(full, len, "./%s", cmd);
        } else {
            size_t len = strlen(dir) + 1 + strlen(cmd) + 1;
            full = malloc(len);
            if (!full) {
                free(paths);
                return NULL;
            }
            snprintf(full, len, "%s/%s", dir, cmd);
        }

        if (access(full, X_OK) == 0) {
            free(paths);
            return full;
        }

        free(full);
        dir = strtok_r(NULL, ":", &saveptr);
    }

    free(paths);
    return NULL;
}

static int apply_redirections(const command_t *cmd)
{
    if (cmd->infile) {
        int fd = open(cmd->infile, O_RDONLY);
        if (fd < 0) {
            perror(cmd->infile);
            return -1;
        }
        if (dup2(fd, STDIN_FILENO) < 0) {
            perror("dup2");
            close(fd);
            return -1;
        }
        close(fd);
    }

    if (cmd->outfile) {
        int flags = O_WRONLY | O_CREAT | (cmd->out_append ? O_APPEND : O_TRUNC);
        int fd = open(cmd->outfile, flags, 0644);
        if (fd < 0) {
            perror(cmd->outfile);
            return -1;
        }
        if (dup2(fd, STDOUT_FILENO) < 0) {
            perror("dup2");
            close(fd);
            return -1;
        }
        close(fd);
    }

    if (cmd->errfile) {
        int flags = O_WRONLY | O_CREAT | (cmd->err_append ? O_APPEND : O_TRUNC);
        int fd = open(cmd->errfile, flags, 0644);
        if (fd < 0) {
            perror(cmd->errfile);
            return -1;
        }
        if (dup2(fd, STDERR_FILENO) < 0) {
            perror("dup2");
            close(fd);
            return -1;
        }
        close(fd);
    }

    return 0;
}

static void print_history(void)
{
    HIST_ENTRY **list = history_list();
    if (!list)
        return;

    for (int i = 0; list[i]; i++)
        printf("%5d  %s\n", i + history_base, list[i]->line);
}

static char *expand_cd_target(const char *arg)
{
    const char *home = getenv("HOME");
    if (!home || !*home)
        home = "/";

    if (!arg || !*arg) {
        return strdup(home);
    }

    if (strcmp(arg, "-") == 0) {
        if (oldpwd[0] == '\0')
            return NULL;
        return strdup(oldpwd);
    }

    if (arg[0] == '~' && (arg[1] == '\0' || arg[1] == '/')) {
        size_t len = strlen(home) + strlen(arg);
        char *out = malloc(len + 1);
        if (!out)
            return NULL;
        snprintf(out, len + 1, "%s%s", home, arg + 1);
        return out;
    }

    return strdup(arg);
}

static int is_builtin(const char *name)
{
    return name && (!strcmp(name, "cd") || !strcmp(name, "exit") || !strcmp(name, "history"));
}

static int run_builtin(command_t *cmd)
{
    if (!strcmp(cmd->argv[0], "exit")) {
        int code = 0;
        if (cmd->argv[1])
            code = atoi(cmd->argv[1]);
        exit(code);
    }

    if (!strcmp(cmd->argv[0], "cd")) {
        char cwd[PATH_MAX];
        const char *target_arg = cmd->argv[1];
        char *target = expand_cd_target(target_arg);

        if (!target) {
            fprintf(stderr, "cd: OLDPWD not set\n");
            return 1;
        }

        if (getcwd(cwd, sizeof(cwd)) == NULL) {
            cwd[0] = '\0';
        }

        if (chdir(target) != 0) {
            perror("cd");
            free(target);
            return 1;
        }

        if (cwd[0] != '\0') {
            snprintf(oldpwd, sizeof(oldpwd), "%s", cwd);
        }

        if (target_arg && strcmp(target_arg, "-") == 0) {
            printf("%s\n", target);
        }

        free(target);
        return 0;
    }

    if (!strcmp(cmd->argv[0], "history")) {
        print_history();
        return 0;
    }

    return -1;
}

static int run_single(char *segment, int background)
{
    command_t cmd = parse_command(segment);
    if (!cmd.valid) {
        free_command(&cmd);
        return 1;
    }

    if (cmd.argc == 0) {
        free_command(&cmd);
        return 0;
    }

    if (is_builtin(cmd.argv[0])) {
        int saved_stdin = dup(STDIN_FILENO);
        int saved_stdout = dup(STDOUT_FILENO);
        int saved_stderr = dup(STDERR_FILENO);

        if (saved_stdin < 0 || saved_stdout < 0 || saved_stderr < 0) {
            perror("dup");
            if (saved_stdin >= 0) close(saved_stdin);
            if (saved_stdout >= 0) close(saved_stdout);
            if (saved_stderr >= 0) close(saved_stderr);
            free_command(&cmd);
            return 1;
        }

        if (apply_redirections(&cmd) == 0)
            run_builtin(&cmd);

        dup2(saved_stdin, STDIN_FILENO);
        dup2(saved_stdout, STDOUT_FILENO);
        dup2(saved_stderr, STDERR_FILENO);
        close(saved_stdin);
        close(saved_stdout);
        close(saved_stderr);

        free_command(&cmd);
        (void)background;
        return 0;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        free_command(&cmd);
        return 1;
    }

    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);

        if (apply_redirections(&cmd) != 0)
            _exit(1);

        char *exe = resolve_path(cmd.argv[0]);
        if (!exe) {
            fprintf(stderr, "%s: command not found\n", cmd.argv[0]);
            _exit(127);
        }

        execve(exe, cmd.argv, environ);
        perror(exe);
        _exit(126);
    }

    if (background) {
        printf("[bg] %d\n", pid);
    } else {
        int status;
        while (waitpid(pid, &status, 0) < 0) {
            if (errno == EINTR)
                continue;
            perror("waitpid");
            break;
        }
    }

    free_command(&cmd);
    return 0;
}

static int run_pipeline(char *line, int background)
{
    char *segments[MAX_PIPES];
    int n = 0;

    int in_s = 0, in_d = 0, esc = 0;
    char *start = line;

    for (char *p = line; ; p++) {
        char c = *p;

        if (esc) {
            esc = 0;
        } else if (!in_s && c == '\\') {
            esc = 1;
        } else if (!in_d && c == '\'') {
            in_s = !in_s;
        } else if (!in_s && c == '"') {
            in_d = !in_d;
        } else if ((c == '|' || c == '\0') && !in_s && !in_d) {
            *p = '\0';

            char *seg = trim(start);
            if (*seg) {
                if (n >= MAX_PIPES) {
                    fprintf(stderr, "too many pipe stages\n");
                    return 1;
                }
                segments[n++] = seg;
            }

            start = p + 1;
            if (c == '\0')
                break;
        }

        if (c == '\0')
            break;
    }

    if (n == 0)
        return 0;

    int prev_read = -1;
    pid_t pids[MAX_PIPES];

    for (int i = 0; i < n; i++) {
        command_t cmd = parse_command(segments[i]);
        if (!cmd.valid) {
            free_command(&cmd);
            if (prev_read != -1)
                close(prev_read);
            return 1;
        }

        if (cmd.argc == 0) {
            free_command(&cmd);
            if (prev_read != -1)
                close(prev_read);
            continue;
        }

        if (is_builtin(cmd.argv[0])) {
            fprintf(stderr, "builtins are not supported inside pipelines yet\n");
            free_command(&cmd);
            if (prev_read != -1)
                close(prev_read);
            return 1;
        }

        int pipefd[2] = {-1, -1};
        if (i < n - 1) {
            if (pipe(pipefd) < 0) {
                perror("pipe");
                free_command(&cmd);
                if (prev_read != -1)
                    close(prev_read);
                return 1;
            }
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            free_command(&cmd);
            if (prev_read != -1)
                close(prev_read);
            if (i < n - 1) {
                close(pipefd[0]);
                close(pipefd[1]);
            }
            return 1;
        }

        if (pid == 0) {
            signal(SIGINT, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);

            if (prev_read != -1) {
                if (dup2(prev_read, STDIN_FILENO) < 0) {
                    perror("dup2");
                    _exit(1);
                }
            }

            if (i < n - 1) {
                if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
                    perror("dup2");
                    _exit(1);
                }
            }

            if (prev_read != -1)
                close(prev_read);

            if (i < n - 1) {
                close(pipefd[0]);
                close(pipefd[1]);
            }

            if (apply_redirections(&cmd) != 0)
                _exit(1);

            char *exe = resolve_path(cmd.argv[0]);
            if (!exe) {
                fprintf(stderr, "%s: command not found\n", cmd.argv[0]);
                _exit(127);
            }

            execve(exe, cmd.argv, environ);
            perror(exe);
            _exit(126);
        }

        pids[i] = pid;

        if (prev_read != -1)
            close(prev_read);

        if (i < n - 1) {
            close(pipefd[1]);
            prev_read = pipefd[0];
        } else {
            prev_read = -1;
        }

        free_command(&cmd);
    }

    if (prev_read != -1)
        close(prev_read);

    if (background) {
        printf("[bg] pipeline started\n");
        return 0;
    }

    int status = 0;
    for (int i = 0; i < n; i++) {
        while (waitpid(pids[i], &status, 0) < 0) {
            if (errno == EINTR)
                continue;
            perror("waitpid");
            break;
        }
    }

    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static int starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static int match_exists(char **matches, size_t count, const char *s)
{
    for (size_t i = 0; i < count; i++) {
        if (strcmp(matches[i], s) == 0)
            return 1;
    }
    return 0;
}

static int add_match(char ***matches, size_t *count, size_t *cap, const char *s)
{
    if (match_exists(*matches, *count, s))
        return 0;

    if (*count + 1 >= *cap) {
        size_t new_cap = (*cap) * 2;
        char **tmp = realloc(*matches, sizeof(char *) * new_cap);
        if (!tmp)
            return -1;
        *matches = tmp;
        *cap = new_cap;
    }

    (*matches)[(*count)++] = strdup(s);
    return (*matches)[*count - 1] ? 0 : -1;
}

static void free_match_list(char **matches, size_t count)
{
    if (!matches)
        return;
    for (size_t i = 0; i < count; i++)
        free(matches[i]);
    free(matches);
}

static int cmp_cstrings(const void *a, const void *b)
{
    const char *const *sa = (const char *const *)a;
    const char *const *sb = (const char *const *)b;
    return strcmp(*sa, *sb);
}

static char **build_command_matches(const char *text)
{
    size_t cap = 64;
    size_t count = 0;
    char **matches = malloc(sizeof(char *) * cap);
    if (!matches)
        return NULL;

    const char *builtins[] = {"cd", "exit", "history", NULL};
    for (int i = 0; builtins[i]; i++) {
        if (starts_with(builtins[i], text)) {
            if (add_match(&matches, &count, &cap, builtins[i]) != 0) {
                free_match_list(matches, count);
                return NULL;
            }
        }
    }

    const char *path_env = getenv("PATH");
    const char *fallback = "/bin:/usr/bin";
    char *paths = strdup((path_env && *path_env) ? path_env : fallback);
    if (!paths) {
        free_match_list(matches, count);
        return NULL;
    }

    char *saveptr = NULL;
    char *dir = strtok_r(paths, ":", &saveptr);

    while (dir) {
        DIR *dp = opendir(*dir ? dir : ".");
        if (dp) {
            struct dirent *de;
            while ((de = readdir(dp)) != NULL) {
                if (de->d_name[0] == '.' && text[0] != '.')
                    continue;
                if (!starts_with(de->d_name, text))
                    continue;

                char pathbuf[PATH_MAX];
                if (*dir == '\0')
                    snprintf(pathbuf, sizeof(pathbuf), "%s", de->d_name);
                else
                    snprintf(pathbuf, sizeof(pathbuf), "%s/%s", dir, de->d_name);

                if (access(pathbuf, X_OK) == 0) {
                    if (add_match(&matches, &count, &cap, de->d_name) != 0) {
                        closedir(dp);
                        free(paths);
                        free_match_list(matches, count);
                        return NULL;
                    }
                }
            }
            closedir(dp);
        }

        dir = strtok_r(NULL, ":", &saveptr);
    }

    free(paths);

    if (count == 0) {
        free(matches);
        return NULL;
    }

    qsort(matches, count, sizeof(char *), cmp_cstrings);

    char **out = malloc(sizeof(char *) * (count + 1));
    if (!out) {
        free_match_list(matches, count);
        return NULL;
    }

    for (size_t i = 0; i < count; i++)
        out[i] = matches[i];
    out[count] = NULL;
    free(matches);
    return out;
}

static char *command_completion_generator(const char *text, int state)
{
    static char **matches = NULL;
    static size_t count = 0;
    static size_t index = 0;

    if (state == 0) {
        free_match_list(matches, count);
        matches = build_command_matches(text);
        count = 0;
        index = 0;
        if (matches) {
            while (matches[count])
                count++;
        }
    }

    if (!matches || index >= count) {
        if (state == 0) {
            free_match_list(matches, count);
            matches = NULL;
            count = 0;
            index = 0;
        }
        return NULL;
    }

    return strdup(matches[index++]);
}

static char **esh_completion(const char *text, int start, int end)
{
    (void)end;

    if (start == 0 && !strchr(text, '/')) {
        return rl_completion_matches(text, command_completion_generator);
    }

    return rl_completion_matches(text, rl_filename_completion_function);
}

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = reap_children;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    struct sigaction ign;
    memset(&ign, 0, sizeof(ign));
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    ign.sa_flags = 0;
    sigaction(SIGINT, &ign, NULL);
    sigaction(SIGQUIT, &ign, NULL);

    using_history();
    rl_attempted_completion_function = esh_completion;
    rl_completion_query_items = 10;
    rl_bind_keyseq("\t", rl_complete);

    while (1) {
        char prompt[PATH_MAX + 8];
        make_prompt(prompt, sizeof(prompt));

        char *raw = readline(prompt);
        if (!raw)
            break;

        char *line = trim(raw);
        if (*line == '\0') {
            free(raw);
            continue;
        }

        int background = strip_background_ampersand(line);
        line = trim(line);

        if (*line == '\0') {
            free(raw);
            continue;
        }

        add_history(line);

        if (has_top_level_pipe(line)) {
            run_pipeline(line, background);
        } else {
            run_single(line, background);
        }

        free(raw);
    }

    putchar('\n');
    return 0;
}
