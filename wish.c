#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef enum {
    TOKEN_WORD,
    TOKEN_REDIRECT,
    TOKEN_PARALLEL
} TokenType;

typedef struct {
    TokenType type;
    char *word; 
} Token;

typedef struct {
    Token *items;
    size_t count;
    size_t capacity;
} TokenList;

typedef struct {
    char **directories;
    size_t count;
} ShellPath;

typedef struct {
    char **argv;      
    size_t argc;
    const char *output;
} Command;

static void print_error(void) {
    static const char message[] = "An error has occurred\n";
    size_t offset = 0;

    while (offset < sizeof(message) - 1) {
        ssize_t written = write(STDERR_FILENO, message + offset,
                                sizeof(message) - 1 - offset);

        if (written > 0) {
            offset += (size_t)written;
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
}

static void free_tokens(TokenList *tokens) {
    for (size_t i = 0; i < tokens->count; ++i) {
        free(tokens->items[i].word);
    }
    free(tokens->items);
    *tokens = (TokenList){0};
}

static bool append_token(TokenList *tokens, Token token) {
    if (tokens->count == tokens->capacity) {
        size_t next = tokens->capacity == 0 ? 8 : tokens->capacity * 2;
        if (next < tokens->capacity || next > SIZE_MAX / sizeof(Token)) {
            return false;
        }
        Token *items = realloc(tokens->items, next * sizeof(Token));
        if (!items) {
            return false;
        }
        tokens->items = items;
        tokens->capacity = next;
    }
    tokens->items[tokens->count++] = token;
    return true;
}

static bool tokenize(const char *line, TokenList *tokens) {
    const char *cursor = line;
    while (*cursor != '\0') {
        if (isspace((unsigned char)*cursor)) {
            ++cursor;
            continue;
        }

        Token token = {0};
        if (*cursor == '>') {
            token.type = TOKEN_REDIRECT;
            ++cursor;
        } else if (*cursor == '&') {
            token.type = TOKEN_PARALLEL;
            ++cursor;
        } else {
            const char *start = cursor;
            while (*cursor && !isspace((unsigned char)*cursor) &&
                   *cursor != '>' && *cursor != '&') {
                ++cursor;
            }
            token.type = TOKEN_WORD;
            token.word = strndup(start, (size_t)(cursor - start));
            if (!token.word) {
                return false;
            }
        }

        if (!append_token(tokens, token)) {
            free(token.word);
            return false;
        }
    }
    return true;
}

static void free_path(ShellPath *path) {
    for (size_t i = 0; i < path->count; ++i) {
        free(path->directories[i]);
    }
    free(path->directories);
    *path = (ShellPath){0};
}

static bool set_path(ShellPath *path, char *const *directories, size_t count) {
    ShellPath replacement = {0};
    if (count != 0) {
        if (count > SIZE_MAX / sizeof(char *)) {
            return false;
        }
        replacement.directories = calloc(count, sizeof(char *));
        if (!replacement.directories) {
            return false;
        }
        for (size_t i = 0; i < count; ++i) {
            replacement.directories[i] = strdup(directories[i]);
            if (!replacement.directories[i]) {
                replacement.count = i;
                free_path(&replacement);
                return false;
            }
            replacement.count = i + 1;
        }
    }
    free_path(path);
    *path = replacement;
    return true;
}

static bool parse_command(const TokenList *tokens, size_t begin, size_t end,
                          Command *command) {
    *command = (Command){0};
    if (begin == end) {
        return false;
    }
    command->argv = calloc(end - begin + 1, sizeof(char *));
    if (!command->argv) {
        return false;
    }

    bool saw_redirect = false;
    for (size_t i = begin; i < end; ++i) {
        Token token = tokens->items[i];
        if (token.type == TOKEN_REDIRECT) {
            if (saw_redirect || command->argc == 0) {
                return false;
            }
            saw_redirect = true;
        } else if (token.type == TOKEN_WORD) {
            if (saw_redirect) {
                if (command->output != NULL || i != end - 1) {
                    return false;
                }
                command->output = token.word;
            } else {
                command->argv[command->argc++] = token.word;
            }
        } else {
            return false;
        }
    }

    return command->argc > 0 && (!saw_redirect || command->output != NULL);
}

static char *find_executable(const ShellPath *path, const char *program) {
    for (size_t i = 0; i < path->count; ++i) {
        const char *dir = path->directories[i];
        size_t dir_len = strlen(dir);
        size_t program_len = strlen(program);
        if (dir_len > SIZE_MAX - program_len - 2) {
            continue;
        }
        char *candidate = malloc(dir_len + 1 + program_len + 1);
        if (!candidate) {
            return NULL;
        }
        memcpy(candidate, dir, dir_len);
        candidate[dir_len] = '/';
        memcpy(candidate + dir_len + 1, program, program_len + 1);
        if (access(candidate, X_OK) == 0) {
            return candidate;
        }
        free(candidate);
    }
    return NULL;
}

static void execute_external(const Command *command, const ShellPath *path,
                             pid_t *children, size_t *child_count) {
    char *executable = find_executable(path, command->argv[0]);
    if (!executable) {
        print_error();
        return;
    }

    pid_t child = fork();
    if (child < 0) {
        print_error();
        free(executable);
        return;
    }
    if (child == 0) {
        if (command->output) {
            int fd = open(command->output, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0 ||
                dup2(fd, STDERR_FILENO) < 0) {
                print_error();
                _exit(1);
            }
            if (fd > STDERR_FILENO) {
                close(fd);
            }
        }
        execv(executable, command->argv);
        print_error();
        _exit(1);
    }
    children[(*child_count)++] = child;
    free(executable);
}

static bool run_command(const Command *command, ShellPath *path,
                        pid_t *children, size_t *child_count) {
    const char *name = command->argv[0];
    bool built_in = strcmp(name, "exit") == 0 ||
                    strcmp(name, "cd") == 0 ||
                    strcmp(name, "path") == 0;

    if (built_in && command->output) {
        print_error();
        return false;
    }

    if (strcmp(name, "exit") == 0) {
        if (command->argc != 1) {
            print_error();
            return false;
        }
        return true;
    }
    if (strcmp(name, "cd") == 0) {
        if (command->argc != 2 || chdir(command->argv[1]) != 0) {
            print_error();
        }
        return false;
    }
    if (strcmp(name, "path") == 0) {
        if (!set_path(path, command->argv + 1, command->argc - 1)) {
            print_error();
        }
        return false;
    }

    execute_external(command, path, children, child_count);
    return false;
}

static bool execute_line(const char *line, ShellPath *path) {
    TokenList tokens = {0};
    if (!tokenize(line, &tokens)) {
        print_error();
        free_tokens(&tokens);
        return false;
    }
    if (tokens.count == 0) {
        free_tokens(&tokens);
        return false;
    }

    pid_t *children = calloc(tokens.count, sizeof(pid_t));
    if (!children) {
        print_error();
        free_tokens(&tokens);
        return false;
    }
    size_t child_count = 0;
    bool should_exit = false;
    size_t begin = 0;

    for (size_t i = 0; i <= tokens.count; ++i) {
        if (i < tokens.count && tokens.items[i].type != TOKEN_PARALLEL) {
            continue;
        }
        if (i == begin) {
            if (i != tokens.count) {
                print_error();
            }
        } else {
            Command command = {0};
            if (!parse_command(&tokens, begin, i, &command)) {
                print_error();
            } else if (run_command(&command, path, children, &child_count)) {
                should_exit = true;
            }
            free(command.argv);
        }
        begin = i + 1;
        if (should_exit) {
            break;
        }
    }

    for (size_t i = 0; i < child_count; ++i) {
        int status;
        while (waitpid(children[i], &status, 0) == -1) {
            if (errno != EINTR) {
                print_error();
                break;
            }
        }
    }
    free(children);
    free_tokens(&tokens);
    return should_exit;
}

int main(int argc, char **argv) {
    if (argc > 2) {
        print_error();
        return 1;
    }

    FILE *input = stdin;
    bool interactive = argc == 1;
    if (!interactive) {
        input = fopen(argv[1], "r");
        if (!input) {
            print_error();
            return 1;
        }
    }

    ShellPath path = {0};
    char *initial[] = {"/bin"};
    if (!set_path(&path, initial, 1)) {
        print_error();
        if (!interactive) {
            fclose(input);
        }
        return 1;
    }

    char *line = NULL;
    size_t capacity = 0;
    int result = 0;
    while (true) {
        if (interactive) {
            fputs("wish> ", stdout);
            fflush(stdout);
        }
        if (getline(&line, &capacity, input) == -1) {
            if (ferror(input)) {
                print_error();
                result = 1;
            }
            break;
        }
        if (execute_line(line, &path)) {
            break;
        }
    }

    free(line);
    free_path(&path);
    if (!interactive) {
        fclose(input);
    }
    return result;
}
