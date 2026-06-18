#include <arch/cpu.h>
#include <arch/io.h>
#include <arch/keyboard.h>
#include <arch/pci.h>
#include <arch/pit.h>
#include <tnu/applets.h>
#include <tnu/block.h>
#include <tnu/console.h>
#include <tnu/elf.h>
#include <tnu/linux_compat.h>
#include <tnu/linux_driver_runtime.h>
#include <tnu/log.h>
#include <tnu/memory.h>
#include <tnu/multiboot2.h>
#include <tnu/net.h>
#include <tnu/printf.h>
#include <tnu/process.h>
#include <tnu/procfs.h>
#include <tnu/string.h>
#include <tnu/syscall.h>
#include <tnu/time.h>
#include <tnu/user.h>
#include <tnu/version.h>
#include <tnu/vfs.h>

#define LINE_MAX 256
#define ARGV_MAX 48
#define HISTORY_MAX 16
#define ENV_MAX 16
#define SCRIPT_MAX_DEPTH 4
#define SCRIPT_BLOCK_MAX 4096
#define DEFAULT_EXEC_PATH "/bin:/sbin:/usr/bin:/usr/games"
#define DEFAULT_LINUX_EXEC_PATH "/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin:/usr/local/sbin"

struct env_pair {
    char key[32];
    char value[128];
};

static const char *env_get_value(const char *key);
static bool shell_can_execute_node(struct vfs_node *node);
static bool bin_entry_exists(const char *name);
static struct vfs_node *resolve_command_node(const char *command, char *resolved, size_t resolved_size);
static struct vfs_node *resolve_linux_command_node(const char *command, char *linux_path,
                                                   size_t linux_path_size,
                                                   char *host_path, size_t host_path_size);
static const char *command_basename(const char *path);

static int shell_apply_user_session(const char *name);
static int shell_authenticate_user(const char *name, bool interactive);
static char history[HISTORY_MAX][LINE_MAX];
static size_t history_count;
static struct env_pair envs[ENV_MAX];
static size_t env_count;
static char op_token_storage[ARGV_MAX][3];
static char stage_expanded_storage[ARGV_MAX][VFS_PATH_MAX];
static char stage_input_buf[4096];
static char stage_capture_buf[4096];
static char pipe_buf_a[4096];
static char pipe_buf_b[4096];
static char script_block_storage[SCRIPT_MAX_DEPTH][SCRIPT_BLOCK_MAX];
static char script_value_storage[SCRIPT_MAX_DEPTH][ARGV_MAX][VFS_PATH_MAX];
static char *script_value_argv[SCRIPT_MAX_DEPTH][ARGV_MAX];
static char script_arg_storage[ARGV_MAX][VFS_PATH_MAX];
static char *script_arg_argv[ARGV_MAX];
static char shell_clipboard[LINE_MAX];

static const char *env_get_value(const char *key);
static bool shell_can_execute_node(struct vfs_node *node);
static bool bin_entry_exists(const char *name);
static struct vfs_node *resolve_command_node(const char *command, char *resolved, size_t resolved_size);
static struct vfs_node *resolve_linux_command_node(const char *command, char *linux_path,
                                                   size_t linux_path_size,
                                                   char *host_path, size_t host_path_size);
static const char *command_basename(const char *path);

struct shell_builtin_doc {
    const char *name;
    const char *usage;
    const char *help;
};

static const struct shell_builtin_doc shell_builtin_docs[] = {
    { "help", "help [COMMAND]",
      "List commands or show documentation for a shell builtin, applet, or executable." },
    { "cd", "cd [DIR]",
      "Change the current directory. Without DIR, changes to the current user's home." },
    { "login", "login USER",
      "Authenticate and switch the current shell session to USER." },
    { "exec", "exec PATH [ARG...]",
      "Execute an ELF userspace program directly." },
    { "linux-run", "linux-run PATH [ARG...]",
      "Execute a Linux ELF binary through the Linux compatibility layer." },
    { "history", "history",
      "Print the shell command history." },
    { "env", "env",
      "Print shell environment variables." },
    { "set", "set KEY VALUE",
      "Set a shell environment variable." },
    { "sh", "sh SCRIPT [ARG...]",
      "Run a TSH script without requiring executable mode." },
    { "sudo", "sudo COMMAND [ARG...]",
      "Authenticate as root and run COMMAND with uid/gid 0, preserving argv." },
    { "sysinstall", "sysinstall",
      "Run the text installer. Requires root and a loaded install image." },
    { "driver", "driver list|stats",
      "Inspect kernel driver providers, including Linux Driver Runtime backends." },
    { "linuxdrv", "linuxdrv load MODULE|logs|modules|stats",
      "Inspect the Linux Driver Runtime module manager and bridge state." },
    { "net", "net trace",
      "Inspect native networking bridge paths." },
};

static void read_file_text(const char *path, char *out, size_t out_size, const char *fallback)
{
    if (!out || out_size == 0) {
        return;
    }
    struct vfs_node *node = vfs_lookup(path, process_current()->cwd);
    if (!node || !node->data) {
        strncpy(out, fallback, out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }
    size_t n = node->size < out_size - 1 ? (size_t)node->size : out_size - 1;
    memcpy(out, node->data, n);
    out[n] = '\0';
}

static void strip_first_newline(char *out)
{
    char *nl = strchr(out, '\n');
    if (nl) {
        *nl = '\0';
    }
}

static void display_cwd(char *out, size_t out_size)
{
    const struct user_record *u = user_current();
    struct process *proc = process_current();
    const char *cwd = proc ? proc->cwd : "/";
    size_t home_len = strlen(u->home);

    if (strcmp(cwd, u->home) == 0) {
        strncpy(out, "~", out_size - 1);
    } else if (home_len > 1 && strncmp(cwd, u->home, home_len) == 0 && cwd[home_len] == '/') {
        ksnprintf(out, out_size, "~%s", cwd + home_len);
    } else {
        strncpy(out, cwd, out_size - 1);
    }
    out[out_size - 1] = '\0';
}

static void prompt(void)
{
    char host[64];
    char cwd[VFS_PATH_MAX];
    const struct user_record *u = user_current();
    read_file_text("/etc/hostname", host, sizeof(host), "tiramisu");
    strip_first_newline(host);
    display_cwd(cwd, sizeof(cwd));
    console_set_color(u->uid == 0 ? CONSOLE_LIGHT_RED : CONSOLE_LIGHT_GREEN, CONSOLE_BLACK);
    kprintf("%s@%s", u->name, host);
    console_set_color(CONSOLE_LIGHT_GREY, CONSOLE_BLACK);
    kprintf(":%s%s ", cwd, u->uid == 0 ? "#" : "$");
}

static void add_history(const char *line)
{
    if (!line[0]) {
        return;
    }
    if (history_count < HISTORY_MAX) {
        strcpy(history[history_count++], line);
    } else {
        for (size_t i = 1; i < HISTORY_MAX; i++) {
            strcpy(history[i - 1], history[i]);
        }
        strcpy(history[HISTORY_MAX - 1], line);
    }
}

static void redraw_line(size_t old_len, size_t old_cursor,
                        const char *line, size_t len, size_t cursor)
{
    while (old_cursor < old_len) {
        console_cursor_right();
        old_cursor++;
    }
    for (size_t i = 0; i < old_len; i++) {
        console_putc('\b');
    }
    for (size_t i = 0; i < len; i++) {
        console_putc(line[i]);
    }
    for (size_t i = cursor; i < len; i++) {
        console_cursor_left();
    }
}

static void replace_line(char *line, size_t size, size_t *len, size_t *cursor,
                         const char *replacement)
{
    size_t old_len = *len;
    size_t old_cursor = *cursor;
    strncpy(line, replacement, size - 1);
    line[size - 1] = '\0';
    *len = strlen(line);
    *cursor = *len;
    redraw_line(old_len, old_cursor, line, *len, *cursor);
}

static void insert_text(char *line, size_t size, size_t *len, size_t *cursor,
                        const char *text)
{
    size_t add = strlen(text);
    if (add == 0 || *len + add >= size) {
        return;
    }
    size_t old_len = *len;
    size_t old_cursor = *cursor;
    memmove(line + *cursor + add, line + *cursor, *len - *cursor + 1);
    memcpy(line + *cursor, text, add);
    *len += add;
    *cursor += add;
    redraw_line(old_len, old_cursor, line, *len, *cursor);
}

static void selection_bounds(bool active, size_t anchor, size_t cursor,
                             size_t *start, size_t *end)
{
    if (!active) {
        *start = *end = cursor;
        return;
    }
    *start = anchor < cursor ? anchor : cursor;
    *end = anchor < cursor ? cursor : anchor;
}

static void delete_selection(char *line, size_t *len, size_t *cursor,
                             bool *sel_active, size_t sel_anchor)
{
    size_t start;
    size_t end;
    selection_bounds(*sel_active, sel_anchor, *cursor, &start, &end);
    if (start == end) {
        *sel_active = false;
        return;
    }
    size_t old_len = *len;
    size_t old_cursor = *cursor;
    memmove(line + start, line + end, *len - end + 1);
    *len -= end - start;
    *cursor = start;
    *sel_active = false;
    redraw_line(old_len, old_cursor, line, *len, *cursor);
}

struct completion_ctx {
    const char *prefix;
    size_t prefix_len;
    char common[VFS_PATH_MAX];
    char display[16][VFS_PATH_MAX];
    size_t count;
};

static void completion_consider(struct completion_ctx *ctx, const char *candidate)
{
    if (!candidate || strncmp(candidate, ctx->prefix, ctx->prefix_len) != 0) {
        return;
    }
    if (ctx->count == 0) {
        strncpy(ctx->common, candidate, sizeof(ctx->common) - 1);
        ctx->common[sizeof(ctx->common) - 1] = '\0';
    } else {
        size_t i = 0;
        while (ctx->common[i] && candidate[i] && ctx->common[i] == candidate[i]) {
            i++;
        }
        ctx->common[i] = '\0';
    }
    if (ctx->count < sizeof(ctx->display) / sizeof(ctx->display[0])) {
        strncpy(ctx->display[ctx->count], candidate, sizeof(ctx->display[0]) - 1);
        ctx->display[ctx->count][sizeof(ctx->display[0]) - 1] = '\0';
    }
    ctx->count++;
}

static bool completion_is_command_position(const char *line, size_t word_start)
{
    size_t i = word_start;
    while (i > 0 && (line[i - 1] == ' ' || line[i - 1] == '\t')) {
        i--;
    }
    if (i == 0) {
        return true;
    }
    if (line[i - 1] == '|') {
        return true;
    }
    if (i >= 2 && line[i - 1] == '&' && line[i - 2] == '&') {
        return true;
    }
    return false;
}

static void path_completion_emit(struct vfs_node *node, void *ctx_ptr)
{
    struct completion_ctx *ctx = ctx_ptr;
    char candidate[VFS_PATH_MAX];
    const char *slash = strrchr(ctx->prefix, '/');
    if (!slash) {
        ksnprintf(candidate, sizeof(candidate), "%s%s",
                  node->name, node->type == VFS_NODE_DIR ? "/" : "");
    } else if (slash == ctx->prefix) {
        ksnprintf(candidate, sizeof(candidate), "/%s%s",
                  node->name, node->type == VFS_NODE_DIR ? "/" : "");
    } else {
        size_t dir_len = (size_t)(slash - ctx->prefix);
        char dir[VFS_PATH_MAX];
        if (dir_len >= sizeof(dir)) {
            return;
        }
        memcpy(dir, ctx->prefix, dir_len);
        dir[dir_len] = '\0';
        ksnprintf(candidate, sizeof(candidate), "%s/%s%s",
                  dir, node->name, node->type == VFS_NODE_DIR ? "/" : "");
    }
    completion_consider(ctx, candidate);
}

static void command_completion_emit(struct vfs_node *node, void *ctx_ptr)
{
    struct completion_ctx *ctx = ctx_ptr;
    if (!node || node->type != VFS_NODE_FILE || !shell_can_execute_node(node)) {
        return;
    }
    completion_consider(ctx, node->name);
}

static const char *shell_path(void)
{
    const char *path = env_get_value("PATH");
    return path && path[0] ? path : DEFAULT_EXEC_PATH;
}

static void complete_path(struct completion_ctx *ctx)
{
    char dir_path[VFS_PATH_MAX];
    const char *slash = strrchr(ctx->prefix, '/');
    if (!slash) {
        strcpy(dir_path, ".");
    } else if (slash == ctx->prefix) {
        strcpy(dir_path, "/");
    } else {
        size_t n = (size_t)(slash - ctx->prefix);
        if (n >= sizeof(dir_path)) {
            return;
        }
        memcpy(dir_path, ctx->prefix, n);
        dir_path[n] = '\0';
    }
    struct vfs_node *dir = vfs_lookup(dir_path, process_current()->cwd);
    if (dir && dir->type == VFS_NODE_DIR) {
        vfs_list(dir, path_completion_emit, ctx);
    }
}

static void complete_command(struct completion_ctx *ctx)
{
    for (size_t i = 0; i < sizeof(shell_builtin_docs) / sizeof(shell_builtin_docs[0]); i++) {
        completion_consider(ctx, shell_builtin_docs[i].name);
    }
    const char *path = shell_path();
    char dir[VFS_PATH_MAX];
    while (*path) {
        size_t n = 0;
        while (path[n] && path[n] != ':' && n + 1 < sizeof(dir)) {
            dir[n] = path[n];
            n++;
        }
        dir[n] = '\0';
        struct vfs_node *node = vfs_lookup(dir[0] ? dir : ".", process_current()->cwd);
        if (node && node->type == VFS_NODE_DIR) {
            vfs_list(node, command_completion_emit, ctx);
        }
        path += n;
        if (*path == ':') {
            path++;
        }
    }
}

static void handle_tab(char *line, size_t size, size_t *len, size_t *cursor)
{
    size_t word_start = *cursor;
    while (word_start > 0 && line[word_start - 1] != ' ' && line[word_start - 1] != '\t' &&
           line[word_start - 1] != '|' && line[word_start - 1] != '&' &&
           line[word_start - 1] != '<' && line[word_start - 1] != '>') {
        word_start--;
    }

    char prefix[VFS_PATH_MAX];
    size_t prefix_len = *cursor - word_start;
    if (prefix_len >= sizeof(prefix)) {
        return;
    }
    memcpy(prefix, line + word_start, prefix_len);
    prefix[prefix_len] = '\0';

    struct completion_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.prefix = prefix;
    ctx.prefix_len = prefix_len;

    if (completion_is_command_position(line, word_start) && !strchr(prefix, '/')) {
        complete_command(&ctx);
    } else {
        complete_path(&ctx);
    }

    if (ctx.count == 0) {
        return;
    }
    if (strlen(ctx.common) > prefix_len) {
        insert_text(line, size, len, cursor, ctx.common + prefix_len);
        return;
    }

    console_putc('\n');
    size_t shown = ctx.count < sizeof(ctx.display) / sizeof(ctx.display[0])
                       ? ctx.count
                       : sizeof(ctx.display) / sizeof(ctx.display[0]);
    for (size_t i = 0; i < shown; i++) {
        kprintf("%s%s", ctx.display[i], (i + 1) % 4 == 0 ? "\n" : "  ");
    }
    if (ctx.count > shown) {
        kprintf("... ");
    }
    kprintf("\n");
    prompt();
    redraw_line(0, 0, line, *len, *cursor);
}

static void read_line(char *line, size_t size)
{
    size_t len = 0;
    size_t cursor = 0;
    int history_index = -1;
    char saved[LINE_MAX];
    bool sel_active = false;
    size_t sel_anchor = 0;
    saved[0] = '\0';
    line[0] = '\0';

    for (;;) {
        int ch = console_getchar();
        if (ch == '\n' || ch == '\r') {
            console_putc('\n');
            line[len] = '\0';
            add_history(line);
            return;
        }
        if (ch == KEY_CTRL_C) {
            keyboard_ack_interrupt();
            while (cursor < len) {
                console_cursor_right();
                cursor++;
            }
            kprintf("^C\n");
            line[0] = '\0';
            return;
        }
        if (ch == KEY_COPY) {
            size_t start;
            size_t end;
            selection_bounds(sel_active, sel_anchor, cursor, &start, &end);
            if (end > start && end - start < sizeof(shell_clipboard)) {
                memcpy(shell_clipboard, line + start, end - start);
                shell_clipboard[end - start] = '\0';
            }
            continue;
        }
        if (ch == KEY_PASTE) {
            if (sel_active) {
                delete_selection(line, &len, &cursor, &sel_active, sel_anchor);
            }
            insert_text(line, size, &len, &cursor, shell_clipboard);
            continue;
        }
        if (ch == '\t') {
            sel_active = false;
            handle_tab(line, size, &len, &cursor);
            continue;
        }
        if (ch == KEY_LEFT) {
            sel_active = false;
            if (cursor) {
                console_cursor_left();
                cursor--;
            }
            continue;
        }
        if (ch == KEY_RIGHT) {
            sel_active = false;
            if (cursor < len) {
                console_cursor_right();
                cursor++;
            }
            continue;
        }
        if (ch == KEY_SHIFT_LEFT) {
            if (!sel_active) {
                sel_active = true;
                sel_anchor = cursor;
            }
            if (cursor) {
                console_cursor_left();
                cursor--;
            }
            continue;
        }
        if (ch == KEY_SHIFT_RIGHT) {
            if (!sel_active) {
                sel_active = true;
                sel_anchor = cursor;
            }
            if (cursor < len) {
                console_cursor_right();
                cursor++;
            }
            continue;
        }
        if (ch == KEY_HOME) {
            sel_active = false;
            while (cursor) {
                console_cursor_left();
                cursor--;
            }
            continue;
        }
        if (ch == KEY_END) {
            sel_active = false;
            while (cursor < len) {
                console_cursor_right();
                cursor++;
            }
            continue;
        }
        if (ch == KEY_UP) {
            if (history_count == 0) {
                continue;
            }
            if (history_index < 0) {
                strcpy(saved, line);
                history_index = (int)history_count - 1;
            } else if (history_index > 0) {
                history_index--;
            }
            replace_line(line, size, &len, &cursor, history[history_index]);
            sel_active = false;
            continue;
        }
        if (ch == KEY_DOWN) {
            if (history_index < 0) {
                continue;
            }
            if ((size_t)history_index + 1 < history_count) {
                history_index++;
                replace_line(line, size, &len, &cursor, history[history_index]);
            } else {
                history_index = -1;
                replace_line(line, size, &len, &cursor, saved);
            }
            sel_active = false;
            continue;
        }
        if (ch == '\b' || ch == 127) {
            if (sel_active) {
                delete_selection(line, &len, &cursor, &sel_active, sel_anchor);
            } else if (cursor) {
                size_t old_len = len;
                size_t old_cursor = cursor;
                memmove(line + cursor - 1, line + cursor, len - cursor + 1);
                len--;
                cursor--;
                redraw_line(old_len, old_cursor, line, len, cursor);
            }
            continue;
        }
        if (ch == KEY_DELETE) {
            if (sel_active) {
                delete_selection(line, &len, &cursor, &sel_active, sel_anchor);
            } else if (cursor < len) {
                size_t old_len = len;
                size_t old_cursor = cursor;
                memmove(line + cursor, line + cursor + 1, len - cursor);
                len--;
                redraw_line(old_len, old_cursor, line, len, cursor);
            }
            continue;
        }
        if (ch >= 32 && ch <= 255 && len + 1 < size) {
            if (sel_active) {
                delete_selection(line, &len, &cursor, &sel_active, sel_anchor);
            }
            size_t old_len = len;
            size_t old_cursor = cursor;
            memmove(line + cursor + 1, line + cursor, len - cursor + 1);
            line[cursor] = (char)ch;
            len++;
            cursor++;
            redraw_line(old_len, old_cursor, line, len, cursor);
            history_index = -1;
        }
    }
}

static void read_password(char *line, size_t size)
{
    size_t len = 0;
    line[0] = '\0';
    for (;;) {
        int ch = console_getchar();
        if (ch == '\n' || ch == '\r') {
            console_putc('\n');
            line[len] = '\0';
            return;
        }
        if (ch == '\b' || ch == 127) {
            if (len) {
                len--;
            }
            continue;
        }
        if (ch >= 32 && ch <= 255 && len + 1 < size) {
            line[len++] = (char)ch;
        }
    }
}

static void read_plain_line(char *line, size_t size)
{
    size_t len = 0;
    line[0] = '\0';
    for (;;) {
        int ch = console_getchar();
        if (ch == '\n' || ch == '\r') {
            console_putc('\n');
            line[len] = '\0';
            return;
        }
        if (ch == KEY_CTRL_C) {
            keyboard_ack_interrupt();
            kprintf("^C\n");
            line[0] = '\0';
            return;
        }
        if (ch == '\b' || ch == 127) {
            if (len) {
                len--;
                console_putc('\b');
            }
            continue;
        }
        if (ch >= 32 && ch <= 255 && len + 1 < size) {
            line[len++] = (char)ch;
            console_putc((char)ch);
        }
    }
}

static void login_prompt(void)
{
    for (;;) {
        char user[LINE_MAX];
        kprintf("\nTiramisu login: ");
        read_plain_line(user, sizeof(user));
        if (!user[0]) {
            continue;
        }
        if (shell_authenticate_user(user, true) < 0) {
            continue;
        }
        if (shell_apply_user_session(user) < 0) {
            kprintf("login: cannot start session for %s\n", user);
            continue;
        }
        return;
    }
}

static size_t shell_operator_len(const char *p)
{
    if ((p[0] == '&' && p[1] == '&') ||
        (p[0] == '=' && p[1] == '=') ||
        (p[0] == '!' && p[1] == '=')) {
        return 2;
    }
    if (p[0] == '|' || p[0] == '<' || p[0] == '>') {
        return 1;
    }
    return 0;
}

static int split_args(char *line, char **argv)
{
    int argc = 0;
    int op_slot = 0;
    char *p = line;
    while (*p && argc < ARGV_MAX - 1) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p) {
            break;
        }

        size_t op_len = shell_operator_len(p);
        if (op_len) {
            int slot = op_slot++ % ARGV_MAX;
            memcpy(op_token_storage[slot], p, op_len);
            op_token_storage[slot][op_len] = '\0';
            argv[argc++] = op_token_storage[slot];
            p += op_len;
            continue;
        }

        argv[argc++] = p;
        char quote = 0;
        while (*p) {
            if (!quote && (*p == '\'' || *p == '"')) {
                quote = *p;
                memmove(p, p + 1, strlen(p));
                continue;
            }
            if (quote && *p == quote) {
                quote = 0;
                memmove(p, p + 1, strlen(p));
                continue;
            }
            if (!quote && (*p == ' ' || *p == '\t')) {
                *p++ = '\0';
                break;
            }
            op_len = !quote ? shell_operator_len(p) : 0;
            if (op_len) {
                char *op = p;
                int slot = op_slot++ % ARGV_MAX;
                memcpy(op_token_storage[slot], op, op_len);
                op_token_storage[slot][op_len] = '\0';
                if (argc < ARGV_MAX - 1) {
                    argv[argc++] = op_token_storage[slot];
                }
                *op = '\0';
                p = op + op_len;
                break;
            }
            p++;
        }
    }
    argv[argc] = NULL;
    return argc;
}

static void expand_tilde(const char *in, char *out, size_t out_size)
{
    if (out_size == 0) {
        return;
    }
    if (!in || in[0] != '~') {
        strncpy(out, in ? in : "", out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }

    if (in[1] == '\0' || in[1] == '/') {
        ksnprintf(out, out_size, "%s%s", user_current()->home, in + 1);
        return;
    }

    char name[USER_NAME_MAX + 1];
    size_t i = 1;
    size_t n = 0;
    while (in[i] && in[i] != '/' && n < USER_NAME_MAX) {
        name[n++] = in[i++];
    }
    name[n] = '\0';
    const struct user_record *u = user_find_name(name);
    if (u) {
        ksnprintf(out, out_size, "%s%s", u->home, in + i);
    } else {
        strncpy(out, in, out_size - 1);
        out[out_size - 1] = '\0';
    }
}

static bool wildcard_match(const char *pattern, const char *text)
{
    if (*pattern == '\0') {
        return *text == '\0';
    }
    if (*pattern == '*') {
        while (*pattern == '*') {
            pattern++;
        }
        if (*pattern == '\0') {
            return true;
        }
        for (const char *p = text; *p; p++) {
            if (wildcard_match(pattern, p)) {
                return true;
            }
        }
        return wildcard_match(pattern, text + strlen(text));
    }
    if (*text && *pattern == *text) {
        return wildcard_match(pattern + 1, text + 1);
    }
    return false;
}

struct glob_context {
    const char *pattern;
    const char *prefix;
    bool bare;
    char **argv;
    char (*storage)[VFS_PATH_MAX];
    int *argc;
    bool matched;
};

static void glob_emit(struct vfs_node *node, void *ctx_ptr)
{
    struct glob_context *ctx = ctx_ptr;
    if (*(ctx->argc) >= ARGV_MAX - 1 || !wildcard_match(ctx->pattern, node->name)) {
        return;
    }
    char *slot = ctx->storage[*(ctx->argc)];
    if (ctx->bare) {
        ksnprintf(slot, VFS_PATH_MAX, "%s", node->name);
    } else if (strcmp(ctx->prefix, "/") == 0) {
        ksnprintf(slot, VFS_PATH_MAX, "/%s", node->name);
    } else {
        ksnprintf(slot, VFS_PATH_MAX, "%s/%s", ctx->prefix, node->name);
    }
    ctx->argv[*(ctx->argc)] = slot;
    (*(ctx->argc))++;
    ctx->matched = true;
}

static void append_expanded_arg(char **argv, char storage[ARGV_MAX][VFS_PATH_MAX],
                                int *argc, const char *value)
{
    if (*argc >= ARGV_MAX - 1) {
        return;
    }
    strncpy(storage[*argc], value, VFS_PATH_MAX - 1);
    storage[*argc][VFS_PATH_MAX - 1] = '\0';
    argv[*argc] = storage[*argc];
    (*argc)++;
}

static void expand_glob_arg(const char *arg, char **argv,
                            char storage[ARGV_MAX][VFS_PATH_MAX], int *argc)
{
    if (!strchr(arg, '*')) {
        append_expanded_arg(argv, storage, argc, arg);
        return;
    }

    char dir_path[VFS_PATH_MAX];
    char prefix[VFS_PATH_MAX];
    char pattern[VFS_NAME_MAX + 1];
    const char *slash = strrchr(arg, '/');
    bool bare = slash == NULL;

    if (!slash) {
        strcpy(dir_path, ".");
        prefix[0] = '\0';
        strncpy(pattern, arg, sizeof(pattern) - 1);
    } else if (slash == arg) {
        strcpy(dir_path, "/");
        strcpy(prefix, "/");
        strncpy(pattern, slash + 1, sizeof(pattern) - 1);
    } else {
        size_t n = (size_t)(slash - arg);
        if (n >= sizeof(dir_path)) {
            append_expanded_arg(argv, storage, argc, arg);
            return;
        }
        memcpy(dir_path, arg, n);
        dir_path[n] = '\0';
        strcpy(prefix, dir_path);
        strncpy(pattern, slash + 1, sizeof(pattern) - 1);
    }
    pattern[sizeof(pattern) - 1] = '\0';
    if (pattern[0] == '\0' || strchr(dir_path, '*')) {
        append_expanded_arg(argv, storage, argc, arg);
        return;
    }

    struct vfs_node *dir = vfs_lookup(dir_path, process_current()->cwd);
    if (!dir || dir->type != VFS_NODE_DIR) {
        append_expanded_arg(argv, storage, argc, arg);
        return;
    }

    struct glob_context ctx = {
        .pattern = pattern,
        .prefix = prefix,
        .bare = bare,
        .argv = argv,
        .storage = storage,
        .argc = argc,
        .matched = false,
    };
    vfs_list(dir, glob_emit, &ctx);
    if (!ctx.matched) {
        append_expanded_arg(argv, storage, argc, arg);
    }
}

static int expand_args(int argc, char **argv, char **expanded,
                       char storage[ARGV_MAX][VFS_PATH_MAX])
{
    if (argc == 0) {
        expanded[0] = NULL;
        return 0;
    }
    expanded[0] = argv[0];
    int out_argc = 1;
    for (int i = 1; i < argc; i++) {
        char path[VFS_PATH_MAX];
        expand_tilde(argv[i], path, sizeof(path));
        expand_glob_arg(path, expanded, storage, &out_argc);
    }
    expanded[out_argc] = NULL;
    return out_argc;
}

static void help_command_emit(struct vfs_node *node, void *ctx)
{
    size_t *column = ctx;
    if (!node || node->type != VFS_NODE_FILE || !shell_can_execute_node(node)) {
        return;
    }
    kprintf("  %s", node->name);
    (*column)++;
    if ((*column % 6) == 0) {
        kprintf("\n");
    }
}

static void help_list_path_commands(void)
{
    const char *path = shell_path();
    char dir[VFS_PATH_MAX];
    size_t column = 0;

    while (*path) {
        size_t n = 0;
        while (path[n] && path[n] != ':' && n + 1 < sizeof(dir)) {
            dir[n] = path[n];
            n++;
        }
        dir[n] = '\0';
        struct vfs_node *node = vfs_lookup(dir[0] ? dir : ".", process_current()->cwd);
        if (node && node->type == VFS_NODE_DIR) {
            vfs_list(node, help_command_emit, &column);
        }
        path += n;
        if (*path == ':') {
            path++;
        }
    }
    if (column % 6 != 0) {
        kprintf("\n");
    }
}

static int cmd_help(int argc, char **argv)
{
    if (argc > 2) {
        kprintf("usage: help [COMMAND]\n");
        return 1;
    }
    if (argc == 2) {
        const char *name = command_basename(argv[1]);
        for (size_t i = 0; i < sizeof(shell_builtin_docs) / sizeof(shell_builtin_docs[0]); i++) {
            if (strcmp(name, shell_builtin_docs[i].name) == 0) {
                kprintf("Usage: %s\n\n%s\n", shell_builtin_docs[i].usage,
                        shell_builtin_docs[i].help);
                return 0;
            }
        }
        if (tnu_applet_help(name) == 0) {
            return 0;
        }
        char path[VFS_PATH_MAX];
        struct vfs_node *node = resolve_command_node(argv[1], path, sizeof(path));
        if (node && node->type == VFS_NODE_FILE) {
            kprintf("%s: executable at %s\n", name, path);
            kprintf("Try: %s --help\n", argv[1]);
            return 0;
        }
        kprintf("help: no help topic for %s\n", argv[1]);
        return 1;
    }
    kprintf("Tiramisu shell builtins:\n");
    for (size_t i = 0; i < sizeof(shell_builtin_docs) / sizeof(shell_builtin_docs[0]); i++) {
        kprintf("  %s", shell_builtin_docs[i].name);
        if ((i + 1) % 6 == 0) {
            kprintf("\n");
        }
    }
    if (sizeof(shell_builtin_docs) / sizeof(shell_builtin_docs[0]) % 6 != 0) {
        kprintf("\n");
    }
    kprintf("PATH executables:\n");
    help_list_path_commands();
    kprintf("Use 'help COMMAND' or 'COMMAND --help' for details.\n");
    return 0;
}

static bool shell_can_search_dir(struct vfs_node *node)
{
    struct process *proc = process_current();
    if (!proc || !node) {
        return false;
    }
    if (proc->uid == 0) {
        return true;
    }
    uint32_t shift = 0;
    if (proc->uid == node->uid) {
        shift = 6;
    } else if (proc->gid == node->gid) {
        shift = 3;
    }
    return ((node->mode >> shift) & 1u) == 1u;
}

static int cmd_cd(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : user_current()->home;
    struct vfs_node *node = vfs_lookup(path, process_current()->cwd);
    if (!node) {
        kprintf("cd: no such directory: %s\n", path);
        return 1;
    }
    if (node->type != VFS_NODE_DIR) {
        kprintf("cd: not a directory: %s\n", path);
        return 1;
    }
    if (!shell_can_search_dir(node)) {
        kprintf("cd: permission denied: %s\n", path);
        return 1;
    }
    if (syscall_dispatch(SYS_CHDIR, (uint64_t)path, 0, 0, 0, 0, 0) < 0) {
        kprintf("cd: failed: %s\n", path);
        return 1;
    }
    return 0;
}

static int shell_apply_user_session(const char *name)
{
    if (user_login(name) < 0) {
        return -1;
    }
    struct process *p = process_current();
    const struct user_record *u = user_current();
    if (!p || !u) {
        return -1;
    }
    p->uid = u->uid;
    p->gid = u->gid;
    strncpy(p->cwd, u->home, sizeof(p->cwd) - 1);
    p->cwd[sizeof(p->cwd) - 1] = '\0';
    return 0;
}

static int shell_authenticate_user(const char *name, bool interactive)
{
    if (!name || !name[0]) {
        return -1;
    }
    if (!user_find_name(name)) {
        if (interactive) {
            kprintf("login: unknown user: %s\n", name);
        }
        return -1;
    }

    char password[LINE_MAX];
    kprintf("Password: ");
    read_password(password, sizeof(password));

    /* Accounts without a password intentionally accept an empty line. */
    if (user_has_password(name)) {
        if (user_login_password(name, password) < 0) {
            if (interactive) {
                kprintf("login: authentication failed\n");
            }
            return -1;
        }
        return 0;
    }

    if (password[0] != '\0') {
        if (interactive) {
            kprintf("login: account has no password; press Enter on an empty password prompt\n");
        }
        return -1;
    }
    return shell_apply_user_session(name);
}

static int cmd_login(int argc, char **argv)
{
    if (argc != 2) {
        kprintf("usage: login USER\n");
        return 1;
    }
    if (shell_authenticate_user(argv[1], true) < 0) {
        return 1;
    }
    if (shell_apply_user_session(argv[1]) < 0) {
        kprintf("login: cannot switch session to %s\n", argv[1]);
        return 1;
    }
    kprintf("logged in as %s\n", user_current()->name);
    return 0;
}

static int cmd_exec(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("usage: exec PATH [ARG...]\n");
        return 1;
    }
    struct vfs_node *node = vfs_lookup(argv[1], process_current()->cwd);
    struct elf_image_info info;
    if (!node || elf64_validate(node->data, (size_t)node->size, &info) < 0) {
        kprintf("exec: not a valid x86_64 TNU ELF: %s\n", argv[1]);
        return 1;
    }
    long rc = syscall_dispatch(SYS_EXEC, (uint64_t)argv[1],
                               (uint64_t)(argc - 1), (uint64_t)(argv + 1),
                               0, 0, 0);
    if (rc < 0) {
        kprintf("exec: failed: %s\n", argv[1]);
        return 1;
    }
    return (int)rc;
}

static int cmd_linux_run(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("usage: linux-run PATH [ARG...]\n");
        return 1;
    }
    long rc = linux_run_binary(argv[1], argc - 1, argv + 1);
    if (rc == -2) {
        kprintf("linux-run: no such Linux binary: %s\n", argv[1]);
        return 127;
    }
    if (rc == -8) {
        kprintf("linux-run: unsupported or invalid Linux ELF: %s\n", argv[1]);
        return 126;
    }
    if (rc == -38) {
        kprintf("linux-run: Linux ABI feature not implemented yet\n");
        return 126;
    }
    if (rc < 0) {
        kprintf("linux-run: failed: %s (%ld)\n", argv[1], rc);
        return 126;
    }
    return (int)rc;
}

static int run_linux_transparent(const char *linux_path, int argc, char **argv)
{
    long rc = linux_run_binary(linux_path, argc, argv);
    if (rc == -2) {
        kprintf("%s: Linux command not found\n", argv[0]);
        return 127;
    }
    if (rc == -8) {
        kprintf("%s: unsupported or invalid Linux ELF\n", argv[0]);
        return 126;
    }
    if (rc == -38) {
        kprintf("%s: Linux ABI feature not implemented yet\n", argv[0]);
        return 126;
    }
    if (rc < 0) {
        kprintf("%s: Linux execution failed (%ld)\n", argv[0], rc);
        return 126;
    }
    if (rc == 139) {
        kprintf("%s: Linux process faulted before completing (SIGSEGV)\n", argv[0]);
        return 139;
    }
    if (rc != 0) {
        kprintf("%s: Linux process exited with status %ld\n", argv[0], rc);
    }
    return (int)rc;
}

static int cmd_history(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    for (size_t i = 0; i < history_count; i++) {
        kprintf("%2llu  %s\n", (uint64_t)i + 1, history[i]);
    }
    return 0;
}

static int cmd_env(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    for (size_t i = 0; i < env_count; i++) {
        kprintf("%s=%s\n", envs[i].key, envs[i].value);
    }
    return 0;
}

static int cmd_set(int argc, char **argv)
{
    if (argc != 3) {
        kprintf("usage: set KEY VALUE\n");
        return 1;
    }
    for (size_t i = 0; i < env_count; i++) {
        if (strcmp(envs[i].key, argv[1]) == 0) {
            strncpy(envs[i].value, argv[2], sizeof(envs[i].value) - 1);
            return 0;
        }
    }
    if (env_count >= ENV_MAX) {
        return 1;
    }
    strncpy(envs[env_count].key, argv[1], sizeof(envs[env_count].key) - 1);
    strncpy(envs[env_count].value, argv[2], sizeof(envs[env_count].value) - 1);
    env_count++;
    return 0;
}

static int cmd_sysinstall(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!process_current() || process_current()->uid != 0) {
        kprintf("sysinstall: permission denied; try sudo sysinstall\n");
        return 1;
    }
    char answer[LINE_MAX];
    const struct boot_info *boot = boot_info_get();
    const uint8_t *image = (const uint8_t *)boot->install_image.start;
    uint64_t image_size = boot->install_image.end > boot->install_image.start
                              ? boot->install_image.end - boot->install_image.start
                              : 0;

    kprintf("Tiramisu sysinstall\n");
    kprintf("Mode: raw boot image install\n");
    kprintf("Detected PCI devices: %llu\n", (uint64_t)pci_count());
    if (!image || image_size == 0) {
        kprintf("sysinstall: no install image was loaded by GRUB.\n");
        kprintf("sysinstall: rebuild the ISO; /boot/install.img must be present.\n");
        return 1;
    }
    size_t disk_count = block_device_count();
    if (disk_count == 0) {
        kprintf("sysinstall: no writable legacy ATA disk detected.\n");
        kprintf("sysinstall: AHCI/NVMe installation still needs those block drivers.\n");
        return 1;
    }

    kprintf("Install image: %llu KiB\n", (image_size + 1023) / 1024);
    kprintf("Writable disks:\n");
    for (size_t i = 0; i < disk_count; i++) {
        const struct block_device_info *info = block_device_get(i);
        if (!info || !info->writable) {
            continue;
        }
        kprintf("  %llu: /dev/%s  %s\n", (uint64_t)i, info->name, info->description);
    }

    kprintf("Disk target [0]: ");
    read_line(answer, sizeof(answer));
    const struct block_device_info *target = NULL;
    if (!answer[0]) {
        target = block_device_get(0);
    } else {
        char *end = NULL;
        long index = strtol(answer, &end, 10);
        if (end && *end == '\0') {
            target = index >= 0 ? block_device_get((size_t)index) : NULL;
        } else {
            target = block_device_find(answer);
        }
    }
    if (!target || !target->writable) {
        kprintf("sysinstall: invalid or unsupported disk target.\n");
        return 1;
    }

    char disk_path[32];
    ksnprintf(disk_path, sizeof(disk_path), "/dev/%s", target->name);
    kprintf("Selected target: %s (%s)\n", disk_path, target->description);
    kprintf("\nWARNING: this will overwrite the beginning of %s.\n", disk_path);
    kprintf("Type INSTALL to continue: ");
    read_line(answer, sizeof(answer));
    if (strcmp(answer, "INSTALL") != 0) {
        kprintf("sysinstall: cancelled.\n");
        return 1;
    }

    const size_t chunk_size = 64 * 1024;
    uint64_t written = 0;
    while (written < image_size) {
        size_t chunk = image_size - written > chunk_size
                           ? chunk_size
                           : (size_t)(image_size - written);
        if (block_write_lba28(target->name, (uint32_t)(written / 512), image + written, chunk) < 0) {
            kprintf("\nsysinstall: disk write failed at %llu KiB.\n", written / 1024);
            return 1;
        }
        written += chunk;
        if ((written % (1024 * 1024)) == 0 || written >= image_size) {
            kprintf("  wrote %llu / %llu KiB\n",
                    written / 1024, (image_size + 1023) / 1024);
        }
    }

    kprintf("sysinstall: boot image written to %s.\n", disk_path);
    kprintf("Remove the USB media and boot from the target disk.\n");
    kprintf("After first boot, run passwd to set the root password.\n");
    return 0;
}

struct command {
    const char *name;
    int (*fn)(int argc, char **argv);
};

static int run_command(int argc, char **argv, const char *stdin_data);
static int run_tokens(int argc, char **argv);
static int run_shell_script(struct vfs_node *node, int argc, char **argv,
                            bool require_exec, bool force_script);

static int cmd_sh(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("usage: sh SCRIPT [ARG...]\n");
        return 1;
    }
    struct vfs_node *node = vfs_lookup(argv[1], process_current()->cwd);
    if (!node || node->type != VFS_NODE_FILE) {
        kprintf("sh: cannot open %s\n", argv[1]);
        return 1;
    }
    int rc = run_shell_script(node, argc - 1, argv + 1, false, true);
    return rc < 0 ? 126 : rc;
}

static int cmd_sudo(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("usage: sudo COMMAND [ARG...]\n");
        return 1;
    }

    struct process *proc = process_current();
    const struct user_record *current = user_current();
    const struct user_record *root = user_find_name("root");
    if (!proc || !current || !root) {
        kprintf("sudo: user database unavailable\n");
        return 1;
    }

    if (proc->uid != 0) {
        char password[LINE_MAX];
        kprintf("[sudo] password for root: ");
        read_password(password, sizeof(password));
        if (!user_check_password("root", password)) {
            kprintf("sudo: authentication failed\n");
            return 1;
        }
    }

    char saved_user[USER_NAME_MAX + 1];
    char saved_cwd[VFS_PATH_MAX];
    uint32_t saved_uid = proc->uid;
    uint32_t saved_gid = proc->gid;
    strncpy(saved_user, current->name, sizeof(saved_user) - 1);
    saved_user[sizeof(saved_user) - 1] = '\0';
    strncpy(saved_cwd, proc->cwd, sizeof(saved_cwd) - 1);
    saved_cwd[sizeof(saved_cwd) - 1] = '\0';

    if (user_login("root") < 0) {
        kprintf("sudo: cannot switch to root\n");
        return 1;
    }
    proc->uid = root->uid;
    proc->gid = root->gid;

    int rc = run_command(argc - 1, argv + 1, NULL);

    user_login(saved_user);
    proc->uid = saved_uid;
    proc->gid = saved_gid;
    strncpy(proc->cwd, saved_cwd, sizeof(proc->cwd) - 1);
    proc->cwd[sizeof(proc->cwd) - 1] = '\0';
    return rc;
}

static int cmd_driver(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "list") == 0) {
        ldr_print_modules();
        return 0;
    }
    if (strcmp(argv[1], "stats") == 0) {
        ldr_print_stats();
        return 0;
    }
    kprintf("usage: driver list|stats\n");
    return 1;
}

static int cmd_linuxdrv(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "logs") == 0) {
        ldr_print_logs();
        return 0;
    }
    if (strcmp(argv[1], "modules") == 0) {
        ldr_print_modules();
        return 0;
    }
    if (strcmp(argv[1], "stats") == 0) {
        ldr_print_stats();
        return 0;
    }
    if (strcmp(argv[1], "load") == 0) {
        if (argc < 3) {
            kprintf("usage: linuxdrv load MODULE\n");
            return 1;
        }
        int rc = ldr_load_module(argv[2]);
        if (rc < 0) {
            kprintf("linuxdrv: load %s failed (%d)\n", argv[2], rc);
            return 1;
        }
        kprintf("linuxdrv: loaded %s\n", argv[2]);
        return 0;
    }
    kprintf("usage: linuxdrv load MODULE|logs|modules|stats\n");
    return 1;
}

static int cmd_net(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "trace") == 0) {
        ldr_print_net_trace();
        return 0;
    }
    kprintf("usage: net trace\n");
    return 1;
}

static const struct command commands[] = {
    { "help", cmd_help },       { "cd", cmd_cd },             { "login", cmd_login },
    { "exec", cmd_exec },       { "linux-run", cmd_linux_run },
    { "history", cmd_history }, { "env", cmd_env },
    { "set", cmd_set },         { "sh", cmd_sh },             { "sudo", cmd_sudo },
    { "sysinstall", cmd_sysinstall },
    { "driver", cmd_driver },   { "linuxdrv", cmd_linuxdrv }, { "net", cmd_net },
};

static int read_node_text(const char *path, char *buf, size_t size)
{
    if (!buf || size == 0) {
        return -1;
    }
    struct vfs_node *node = vfs_lookup(path, process_current()->cwd);
    if (!node || node->type == VFS_NODE_DIR) {
        buf[0] = '\0';
        return -1;
    }
    size_t n = node->size < size - 1 ? (size_t)node->size : size - 1;
    if (n && node->data) {
        memcpy(buf, node->data, n);
    }
    buf[n] = '\0';
    return (int)n;
}

static bool bin_entry_exists(const char *name)
{
    char path[VFS_PATH_MAX];
    ksnprintf(path, sizeof(path), "/bin/%s", name);
    if (vfs_lookup(path, "/")) {
        return true;
    }
    ksnprintf(path, sizeof(path), "/sbin/%s", name);
    return vfs_lookup(path, "/") != NULL;
}

static int parse_priority_value(const char *text, const char *key, int fallback)
{
    if (!text || !key) {
        return fallback;
    }
    size_t key_len = strlen(key);
    const char *p = text;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            p++;
        }
        if (strncmp(p, key, key_len) == 0 && p[key_len] == ':') {
            p += key_len + 1;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            int sign = 1;
            if (*p == '-') {
                sign = -1;
                p++;
            }
            int value = 0;
            bool any = false;
            while (*p >= '0' && *p <= '9') {
                any = true;
                if (value < 100000000) {
                    value = value * 10 + (*p - '0');
                }
                p++;
            }
            return any ? value * sign : fallback;
        }
        while (*p && *p != '\n') {
            p++;
        }
    }
    return fallback;
}

static void command_priority_read(int *linux_weight, int *tnu_weight)
{
    char buf[256];
    int linux_env_weight = 99999;
    int tnu = 1;
    if (read_node_text("/etc/priority", buf, sizeof(buf)) >= 0) {
        linux_env_weight = parse_priority_value(buf, "linux", linux_env_weight);
        tnu = parse_priority_value(buf, "tnu", tnu);
    }
    if (linux_weight) {
        *linux_weight = linux_env_weight;
    }
    if (tnu_weight) {
        *tnu_weight = tnu;
    }
}

static bool tnu_command_always_priority(const char *name)
{
    static const char *const names[] = {
        "sysfetch", "hostname", "login", "useradd", "userdel", "passwd",
        "init", "sh", "tsh", "uname", "tirux", "shutdown", "reboot",
        "sync", "keymap", "timezone", "layout", "nano",
    };
    const char *base = command_basename(name);
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(base, names[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool path_is_linux_root(const char *path)
{
    return path && strncmp(path, "/usr/linux/", 11) == 0;
}

static bool linux_busybox_applet_candidate(const char *name)
{
    static const char *const applets[] = {
        "ash", "awk", "basename", "cat", "chmod", "chown", "clear", "cp",
        "date", "dd", "df", "dirname", "dmesg", "du", "echo", "egrep",
        "env", "false", "fgrep", "find", "grep", "head", "hostname", "id",
        "ln", "ls", "mkdir", "mktemp", "mount", "mv", "pidof", "ping",
        "printf", "ps", "pwd", "rm", "rmdir", "sed", "sh", "sleep", "sort",
        "stat", "sync", "tail", "tar", "test", "touch", "tr", "true",
        "uname", "vi", "wget", "which", "whoami", "xargs",
    };
    const char *base = command_basename(name);
    for (size_t i = 0; i < sizeof(applets) / sizeof(applets[0]); i++) {
        if (strcmp(base, applets[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool shell_can_execute_node(struct vfs_node *node)
{
    struct process *proc = process_current();
    if (!proc || !node || node->type != VFS_NODE_FILE) {
        return false;
    }
    if (proc->uid == 0) {
        return (node->mode & 0111u) != 0;
    }
    uint32_t shift = 0;
    if (proc->uid == node->uid) {
        shift = 6;
    } else if (proc->gid == node->gid) {
        shift = 3;
    }
    return ((node->mode >> shift) & 1u) == 1u;
}

static struct vfs_node *resolve_command_node(const char *command, char *resolved, size_t resolved_size)
{
    if (!command || !command[0] || !resolved || resolved_size == 0) {
        return NULL;
    }
    if (strchr(command, '/')) {
        expand_tilde(command, resolved, resolved_size);
        return vfs_lookup(resolved, process_current()->cwd);
    }

    const char *path = shell_path();
    char dir[VFS_PATH_MAX];
    while (*path) {
        size_t n = 0;
        while (path[n] && path[n] != ':' && n + 1 < sizeof(dir)) {
            dir[n] = path[n];
            n++;
        }
        dir[n] = '\0';
        if (dir[0]) {
            ksnprintf(resolved, resolved_size, "%s/%s", dir, command);
        } else {
            ksnprintf(resolved, resolved_size, "%s", command);
        }
        struct vfs_node *node = vfs_lookup(resolved, process_current()->cwd);
        if (node) {
            return node;
        }
        path += n;
        if (*path == ':') {
            path++;
        }
    }
    return NULL;
}

static struct vfs_node *resolve_linux_command_node(const char *command, char *linux_path,
                                                   size_t linux_path_size,
                                                   char *host_path, size_t host_path_size)
{
    if (!command || !command[0] || !linux_path || !host_path ||
        linux_path_size == 0 || host_path_size == 0) {
        return NULL;
    }

    if (strchr(command, '/')) {
        if (path_is_linux_root(command)) {
            ksnprintf(host_path, host_path_size, "%s", command);
            ksnprintf(linux_path, linux_path_size, "%s", command + 10);
            return vfs_lookup(host_path, process_current()->cwd);
        }
        if (command[0] == '/') {
            ksnprintf(linux_path, linux_path_size, "%s", command);
            ksnprintf(host_path, host_path_size, "/usr/linux%s", command);
            return vfs_lookup(host_path, "/");
        }
        return NULL;
    }

    const char *path = DEFAULT_LINUX_EXEC_PATH;
    char dir[VFS_PATH_MAX];
    while (*path) {
        size_t n = 0;
        while (path[n] && path[n] != ':' && n + 1 < sizeof(dir)) {
            dir[n] = path[n];
            n++;
        }
        dir[n] = '\0';
        if (dir[0]) {
            ksnprintf(linux_path, linux_path_size, "%s/%s", dir, command);
            ksnprintf(host_path, host_path_size, "/usr/linux%s/%s", dir, command);
        } else {
            ksnprintf(linux_path, linux_path_size, "/%s", command);
            ksnprintf(host_path, host_path_size, "/usr/linux/%s", command);
        }
        struct vfs_node *node = vfs_lookup(host_path, "/");
        if (node) {
            return node;
        }
        path += n;
        if (*path == ':') {
            path++;
        }
    }

    /* Alpine/BusyBox rootfs images often expose common commands as symlinks
     * to /bin/busybox. TFS does not preserve symlinks yet, and the rootfs
     * packaging step may delete absolute symlinks, so fall back to the BusyBox
     * dispatcher while preserving argv[0] as the requested applet name. */
    struct vfs_node *busybox = linux_busybox_applet_candidate(command) ?
        vfs_lookup("/usr/linux/bin/busybox", "/") : NULL;
    if (busybox && busybox->type == VFS_NODE_FILE) {
        ksnprintf(linux_path, linux_path_size, "/bin/busybox");
        ksnprintf(host_path, host_path_size, "/usr/linux/bin/busybox");
        return busybox;
    }

    return NULL;
}

static const char *command_basename(const char *path)
{
    const char *last = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') {
            last = p + 1;
        }
    }
    return last;
}

static const char *env_get_value(const char *key)
{
    for (size_t i = 0; i < env_count; i++) {
        if (strcmp(envs[i].key, key) == 0) {
            return envs[i].value;
        }
    }
    return NULL;
}

static int env_set_value(const char *key, const char *value)
{
    if (!key || !key[0] || !value) {
        return 1;
    }
    for (const char *p = key; *p; p++) {
        bool ok = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                  (*p >= '0' && *p <= '9') || *p == '_';
        if (!ok || (p == key && *p >= '0' && *p <= '9')) {
            return 1;
        }
    }
    for (size_t i = 0; i < env_count; i++) {
        if (strcmp(envs[i].key, key) == 0) {
            strncpy(envs[i].value, value, sizeof(envs[i].value) - 1);
            envs[i].value[sizeof(envs[i].value) - 1] = '\0';
            return 0;
        }
    }
    if (env_count >= ENV_MAX) {
        return 1;
    }
    strncpy(envs[env_count].key, key, sizeof(envs[env_count].key) - 1);
    envs[env_count].key[sizeof(envs[env_count].key) - 1] = '\0';
    strncpy(envs[env_count].value, value, sizeof(envs[env_count].value) - 1);
    envs[env_count].value[sizeof(envs[env_count].value) - 1] = '\0';
    env_count++;
    return 0;
}

static void script_append(char *out, size_t out_size, size_t *pos, const char *text)
{
    while (text && *text && *pos + 1 < out_size) {
        out[(*pos)++] = *text++;
    }
    out[*pos] = '\0';
}

static void script_expand_line(const char *in, char *out, size_t out_size,
                               int argc, char **argv, int last_status)
{
    size_t pos = 0;
    out[0] = '\0';
    for (size_t i = 0; in[i] && pos + 1 < out_size; i++) {
        if (in[i] != '$') {
            out[pos++] = in[i];
            out[pos] = '\0';
            continue;
        }
        char next = in[++i];
        if (next >= '0' && next <= '9') {
            int index = next - '0';
            if (index < argc) {
                script_append(out, out_size, &pos, argv[index]);
            }
        } else if (next == '@') {
            for (int a = 1; a < argc; a++) {
                if (a > 1) {
                    script_append(out, out_size, &pos, " ");
                }
                script_append(out, out_size, &pos, argv[a]);
            }
        } else if (next == '?') {
            char value[16];
            ksnprintf(value, sizeof(value), "%d", last_status);
            script_append(out, out_size, &pos, value);
        } else if ((next >= 'A' && next <= 'Z') || (next >= 'a' && next <= 'z') || next == '_') {
            char key[32];
            size_t k = 0;
            key[k++] = next;
            while (in[i + 1] &&
                   ((in[i + 1] >= 'A' && in[i + 1] <= 'Z') ||
                    (in[i + 1] >= 'a' && in[i + 1] <= 'z') ||
                    (in[i + 1] >= '0' && in[i + 1] <= '9') ||
                    in[i + 1] == '_') &&
                   k + 1 < sizeof(key)) {
                key[k++] = in[++i];
            }
            key[k] = '\0';
            const char *value = env_get_value(key);
            if (!value && strcmp(key, "USER") == 0) {
                value = user_current()->name;
            } else if (!value && strcmp(key, "HOME") == 0) {
                value = user_current()->home;
            }
            script_append(out, out_size, &pos, value ? value : "");
        } else {
            out[pos++] = '$';
            if (next) {
                out[pos++] = next;
            } else {
                break;
            }
            out[pos] = '\0';
        }
    }
    out[pos] = '\0';
}

static bool has_suffix(const char *s, const char *suffix)
{
    size_t s_len = strlen(s);
    size_t suffix_len = strlen(suffix);
    return s_len >= suffix_len && strcmp(s + s_len - suffix_len, suffix) == 0;
}

static bool shell_name_matches(const char *name)
{
    const char *base = command_basename(name);
    return strcmp(base, "sh") == 0 || strcmp(base, "tsh") == 0;
}

static bool script_shell_shebang(const struct vfs_node *node)
{
    if (!node || !node->data || node->size < 2) {
        return false;
    }
    const char *data = (const char *)node->data;
    if (data[0] != '#' || data[1] != '!') {
        return false;
    }
    size_t i = 2;
    while (i < node->size && (data[i] == ' ' || data[i] == '\t')) {
        i++;
    }
    char interp[64];
    char arg[64];
    size_t n = 0;
    while (i < node->size && data[i] != '\n' && data[i] != ' ' && data[i] != '\t' &&
           n + 1 < sizeof(interp)) {
        interp[n++] = data[i++];
    }
    interp[n] = '\0';
    while (i < node->size && (data[i] == ' ' || data[i] == '\t')) {
        i++;
    }
    n = 0;
    while (i < node->size && data[i] != '\n' && data[i] != ' ' && data[i] != '\t' &&
           n + 1 < sizeof(arg)) {
        arg[n++] = data[i++];
    }
    arg[n] = '\0';

    if (shell_name_matches(interp)) {
        return true;
    }
    return strcmp(command_basename(interp), "env") == 0 && shell_name_matches(arg);
}

static bool script_has_shebang(const struct vfs_node *node)
{
    return node && node->data && node->size >= 2 &&
           ((const char *)node->data)[0] == '#' && ((const char *)node->data)[1] == '!';
}

static bool script_looks_textual(const struct vfs_node *node)
{
    if (!node || node->type != VFS_NODE_FILE || !node->data) {
        return false;
    }
    size_t n = node->size < 256 ? (size_t)node->size : 256;
    const unsigned char *data = node->data;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = data[i];
        if (c == 0) {
            return false;
        }
        if (c < 32 && c != '\n' && c != '\r' && c != '\t' && c != '\b') {
            return false;
        }
    }
    return true;
}

static bool script_should_run(struct vfs_node *node, const char *invoked,
                              bool force_script, bool *skip_first_line)
{
    *skip_first_line = false;
    if (force_script) {
        return node && node->type == VFS_NODE_FILE;
    }
    if (script_shell_shebang(node)) {
        *skip_first_line = true;
        return true;
    }
    if (script_has_shebang(node)) {
        return false;
    }
    return has_suffix(invoked, ".sh") || has_suffix(invoked, ".tsh") ||
           has_suffix(node->name, ".sh") || has_suffix(node->name, ".tsh");
}

static char *script_trim(char *line)
{
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t' ||
                       line[len - 1] == '\r')) {
        line[--len] = '\0';
    }
    return line;
}

static bool script_read_line(const char *data, size_t size, size_t *offset,
                             char *out, size_t out_size)
{
    if (*offset >= size) {
        return false;
    }
    size_t len = 0;
    while (*offset < size && data[*offset] != '\n' && len + 1 < out_size) {
        out[len++] = data[(*offset)++];
    }
    while (*offset < size && data[*offset] != '\n') {
        (*offset)++;
    }
    if (*offset < size && data[*offset] == '\n') {
        (*offset)++;
    }
    out[len] = '\0';
    return true;
}

static bool script_strip_open_brace(char *line)
{
    char *trimmed = script_trim(line);
    (void)trimmed;
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t')) {
        line[--len] = '\0';
    }
    if (len == 0 || line[len - 1] != '{') {
        return false;
    }
    line[len - 1] = '\0';
    script_trim(line);
    return true;
}

static int script_collect_block(const char *data, size_t size, size_t *offset,
                                char *out, size_t out_size)
{
    char line[LINE_MAX];
    int depth = 1;
    size_t used = 0;
    out[0] = '\0';

    while (script_read_line(data, size, offset, line, sizeof(line))) {
        char control[LINE_MAX];
        strncpy(control, line, sizeof(control) - 1);
        control[sizeof(control) - 1] = '\0';
        char *trimmed = script_trim(control);

        if (strcmp(trimmed, "}") == 0 || strcmp(trimmed, "fi") == 0 ||
            strcmp(trimmed, "done") == 0) {
            depth--;
            if (depth == 0) {
                return 0;
            }
        }

        if (used + strlen(line) + 2 >= out_size) {
            return -1;
        }
        script_append(out, out_size, &used, line);
        script_append(out, out_size, &used, "\n");

        strncpy(control, line, sizeof(control) - 1);
        control[sizeof(control) - 1] = '\0';
        trimmed = script_trim(control);
        if (strlen(trimmed) > 0 && trimmed[strlen(trimmed) - 1] == '{') {
            depth++;
        }
    }
    return -1;
}

static bool script_assignment(char *line)
{
    char *eq = strchr(line, '=');
    if (!eq || eq == line || eq[1] == '=') {
        return false;
    }
    for (char *p = line; p < eq; p++) {
        bool ok = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                  (*p >= '0' && *p <= '9') || *p == '_';
        if (!ok || (p == line && *p >= '0' && *p <= '9')) {
            return false;
        }
    }
    *eq = '\0';
    return env_set_value(line, eq + 1) == 0;
}

static int script_condition_status(char *condition)
{
    char *trimmed = script_trim(condition);
    if (strncmp(trimmed, "exists ", 7) == 0) {
        char path[VFS_PATH_MAX];
        expand_tilde(script_trim(trimmed + 7), path, sizeof(path));
        return vfs_lookup(path, process_current()->cwd) ? 0 : 1;
    }
    char *tokens[ARGV_MAX];
    int token_count = split_args(trimmed, tokens);
    return run_tokens(token_count, tokens);
}

static int run_shell_script_data(const char *data, size_t size, int argc, char **argv,
                                 bool skip_first_line, int depth)
{
    if (depth >= SCRIPT_MAX_DEPTH) {
        kprintf("tsh: script nesting limit reached\n");
        return 2;
    }
    char raw[LINE_MAX];
    char control[LINE_MAX];
    char expanded[LINE_MAX];
    char *tokens[ARGV_MAX];
    char *block = script_block_storage[depth];
    size_t offset = 0;
    int status = 0;
    bool first_line = skip_first_line;

    while (script_read_line(data, size, &offset, raw, sizeof(raw))) {
        if (first_line) {
            first_line = false;
            continue;
        }
        char *p = script_trim(raw);
        if (*p == '\0' || *p == '#') {
            continue;
        }

        strncpy(control, p, sizeof(control) - 1);
        control[sizeof(control) - 1] = '\0';
        if (script_assignment(control)) {
            status = 0;
            continue;
        }

        if (strncmp(p, "exit", 4) == 0 && (p[4] == '\0' || p[4] == ' ' || p[4] == '\t')) {
            char *arg = p + 4;
            while (*arg == ' ' || *arg == '\t') {
                arg++;
            }
            return *arg ? atoi(arg) : status;
        }

        if (strncmp(p, "if ", 3) == 0) {
            strncpy(control, p + 3, sizeof(control) - 1);
            control[sizeof(control) - 1] = '\0';
            if (!script_strip_open_brace(control)) {
                kprintf("tsh: if requires a braced block\n");
                return 2;
            }
            if (script_collect_block(data, size, &offset, block, SCRIPT_BLOCK_MAX) < 0) {
                kprintf("tsh: unterminated if block\n");
                return 2;
            }
            script_expand_line(control, expanded, sizeof(expanded), argc, argv, status);
            if (script_condition_status(expanded) == 0) {
                status = run_shell_script_data(block, strlen(block), argc, argv, false, depth + 1);
            } else {
                status = 0;
            }
            continue;
        }

        if (strncmp(p, "for ", 4) == 0) {
            strncpy(control, p, sizeof(control) - 1);
            control[sizeof(control) - 1] = '\0';
            if (!script_strip_open_brace(control)) {
                kprintf("tsh: for requires a braced block\n");
                return 2;
            }
            if (script_collect_block(data, size, &offset, block, SCRIPT_BLOCK_MAX) < 0) {
                kprintf("tsh: unterminated for block\n");
                return 2;
            }
            script_expand_line(control, expanded, sizeof(expanded), argc, argv, status);
            int token_count = split_args(expanded, tokens);
            if (token_count < 4 || strcmp(tokens[0], "for") != 0 || strcmp(tokens[2], "in") != 0) {
                kprintf("tsh: usage: for NAME in WORDS { ... }\n");
                return 2;
            }
            char **values = script_value_argv[depth];
            char (*value_storage)[VFS_PATH_MAX] = script_value_storage[depth];
            int value_count = 0;
            values[0] = NULL;
            for (int i = 3; i < token_count; i++) {
                char path[VFS_PATH_MAX];
                expand_tilde(tokens[i], path, sizeof(path));
                expand_glob_arg(path, values, value_storage, &value_count);
            }
            for (int i = 0; i < value_count; i++) {
                env_set_value(tokens[1], values[i]);
                status = run_shell_script_data(block, strlen(block), argc, argv, false, depth + 1);
            }
            continue;
        }

        script_expand_line(p, expanded, sizeof(expanded), argc, argv, status);
        int token_count = split_args(expanded, tokens);
        status = run_tokens(token_count, tokens);
    }
    return status;
}

static int run_shell_script(struct vfs_node *node, int argc, char **argv,
                            bool require_exec, bool force_script)
{
    bool skip_first_line = false;
    if (!script_should_run(node, argv[0], force_script, &skip_first_line)) {
        return -1;
    }
    if (require_exec && !shell_can_execute_node(node)) {
        kprintf("%s: permission denied\n", argv[0]);
        return 126;
    }
    if (!node->data) {
        return 0;
    }
    int script_argc = argc < ARGV_MAX - 1 ? argc : ARGV_MAX - 1;
    for (int i = 0; i < script_argc; i++) {
        strncpy(script_arg_storage[i], argv[i], sizeof(script_arg_storage[i]) - 1);
        script_arg_storage[i][sizeof(script_arg_storage[i]) - 1] = '\0';
        script_arg_argv[i] = script_arg_storage[i];
    }
    script_arg_argv[script_argc] = NULL;
    return run_shell_script_data((const char *)node->data, (size_t)node->size,
                                 script_argc, script_arg_argv, skip_first_line, 0);
}

static int run_command(int argc, char **argv, const char *stdin_data)
{
    if (argc == 0) {
        return 0;
    }
    if (argc == 3 && (strcmp(argv[1], "==") == 0 || strcmp(argv[1], "!=") == 0)) {
        bool equal = strcmp(argv[0], argv[2]) == 0;
        return (strcmp(argv[1], "==") == 0 ? equal : !equal) ? 0 : 1;
    }
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        if (strcmp(argv[0], commands[i].name) == 0) {
            return commands[i].fn(argc, argv);
        }
    }

    const char *cmd_name = command_basename(argv[0]);
    bool applet_available = tnu_applet_is_command(cmd_name) &&
                            (bin_entry_exists(cmd_name) || strcmp(cmd_name, "layout") == 0);
    bool force_tnu = tnu_command_always_priority(cmd_name);
    char linux_path[VFS_PATH_MAX];
    char linux_host_path[VFS_PATH_MAX];
    struct vfs_node *linux_node = (force_tnu || strchr(argv[0], '/')) ? NULL :
        resolve_linux_command_node(argv[0], linux_path, sizeof(linux_path),
                                   linux_host_path, sizeof(linux_host_path));
    int linux_weight = 0;
    int tnu_weight = 1;
    command_priority_read(&linux_weight, &tnu_weight);

    if (!force_tnu && linux_node && linux_weight > tnu_weight) {
        if (!shell_can_execute_node(linux_node)) {
            kprintf("%s: permission denied\n", argv[0]);
            return 126;
        }
        return run_linux_transparent(linux_path, argc, argv);
    }

    if (applet_available) {
        return tnu_applet_run(argc, argv, stdin_data);
    }

    char path[VFS_PATH_MAX];
    struct vfs_node *node = resolve_command_node(argv[0], path, sizeof(path));
    if (node) {
        if (!shell_can_execute_node(node)) {
            kprintf("%s: permission denied\n", argv[0]);
            return 126;
        }
        if (path_is_linux_root(path)) {
            const char *inner = path + 10;
            return run_linux_transparent(inner, argc, argv);
        }
        int script_rc = run_shell_script(node, argc, argv, true, false);
        if (script_rc >= 0) {
            return script_rc;
        }
        //kprintf("tsh: executing %s in userspace...\n", path);
        long rc = syscall_dispatch(SYS_EXEC, (uint64_t)path,
                                   (uint64_t)argc, (uint64_t)argv,
                                   0, 0, 0);
        if (rc < 0) {
            if (script_looks_textual(node)) {
                int fallback_rc = run_shell_script(node, argc, argv, true, true);
                if (fallback_rc >= 0) {
                    return fallback_rc;
                }
            }
            kprintf("tsh: exec failed: %s (returned %ld)\n", path, rc);
            return 126;
        }
        return (int)rc;
    }
    kprintf("%s: command not found\n", argv[0]);
    return 127;
}

static int run_stage(char **tokens, int start, int end, const char *stdin_data,
                     bool capture, char *output, size_t output_size)
{
    char *argv[ARGV_MAX];
    char *expanded[ARGV_MAX];
    int argc = 0;
    const char *input_path = NULL;
    const char *output_path = NULL;

    for (int i = start; i < end && argc < ARGV_MAX - 1; i++) {
        if (strcmp(tokens[i], "<") == 0 || strcmp(tokens[i], ">") == 0) {
            bool out = strcmp(tokens[i], ">") == 0;
            if (i + 1 >= end) {
                kprintf("tsh: missing filename after %s\n", tokens[i]);
                return 2;
            }
            if (out) {
                output_path = tokens[++i];
            } else {
                input_path = tokens[++i];
            }
            continue;
        }
        argv[argc++] = tokens[i];
    }
    argv[argc] = NULL;
    argc = expand_args(argc, argv, expanded, stage_expanded_storage);

    const char *input = stdin_data;
    char input_path_expanded[VFS_PATH_MAX];
    char output_path_expanded[VFS_PATH_MAX];
    if (input_path) {
        expand_tilde(input_path, input_path_expanded, sizeof(input_path_expanded));
        if (read_node_text(input_path_expanded, stage_input_buf, sizeof(stage_input_buf)) < 0) {
            kprintf("tsh: cannot read %s\n", input_path_expanded);
            return 1;
        }
        input = stage_input_buf;
    }
    if (output_path) {
        expand_tilde(output_path, output_path_expanded, sizeof(output_path_expanded));
        output_path = output_path_expanded;
    }

    bool do_capture = capture || output_path;
    if (do_capture) {
        console_capture_begin(stage_capture_buf, sizeof(stage_capture_buf));
    }
    int rc = run_command(argc, expanded, input);
    size_t captured = 0;
    if (do_capture) {
        captured = console_capture_end();
        if (output_path) {
            int fd = (int)syscall_dispatch(SYS_OPEN, (uint64_t)output_path,
                                           VFS_O_CREAT | VFS_O_RDWR | VFS_O_TRUNC,
                                           0644, 0, 0, 0);
            long written = 0;
            if (fd >= 0 && captured > 0) {
                written = syscall_dispatch(SYS_WRITE, (uint64_t)fd,
                                           (uint64_t)stage_capture_buf, captured, 0, 0, 0);
            }
            if (fd >= 0) {
                syscall_dispatch(SYS_CLOSE, (uint64_t)fd, 0, 0, 0, 0, 0);
            }
            if (fd < 0 || (captured > 0 && written != (long)captured)) {
                kprintf("tsh: cannot write %s\n", output_path);
                return 1;
            }
        }
        if (capture && output && output_size) {
            size_t n = captured < output_size - 1 ? captured : output_size - 1;
            memcpy(output, stage_capture_buf, n);
            output[n] = '\0';
        }
    }
    return rc;
}

static int run_pipeline(char **tokens, int start, int end)
{
    const char *input = NULL;
    bool use_a = true;
    int rc = 0;
    int seg_start = start;

    while (seg_start < end) {
        int seg_end = seg_start;
        while (seg_end < end && strcmp(tokens[seg_end], "|") != 0) {
            seg_end++;
        }
        bool last = seg_end == end;
        char *out = use_a ? pipe_buf_a : pipe_buf_b;
        rc = run_stage(tokens, seg_start, seg_end, input, !last, out, 4096);
        if (!last) {
            input = out;
            use_a = !use_a;
        }
        seg_start = seg_end + 1;
    }
    return rc;
}

static int run_tokens(int argc, char **argv)
{
    int start = 0;
    int rc = 0;
    for (int i = 0; i <= argc; i++) {
        if (i == argc || strcmp(argv[i], "&&") == 0) {
            if (i == start) {
                return 2;
            }
            rc = run_pipeline(argv, start, i);
            if (rc != 0) {
                return rc;
            }
            start = i + 1;
        }
    }
    return rc;
}

static void init_env(void)
{
    env_count = 0;
    strcpy(envs[env_count].key, "PATH");
    strcpy(envs[env_count++].value, DEFAULT_EXEC_PATH);
    strcpy(envs[env_count].key, "SHELL");
    strcpy(envs[env_count++].value, "/bin/tsh");
    strcpy(envs[env_count].key, "TERM");
    strcpy(envs[env_count++].value, "tnu-vga");
}

static void init_keymap(void)
{
    char keymap[32];
    read_file_text("/etc/keymap", keymap, sizeof(keymap), "us");
    strip_first_newline(keymap);
    if (keymap[0] && keyboard_set_layout(keymap) < 0) {
        keyboard_set_layout("us");
    }
}

void tsh_run(void)
{
    init_env();
    init_keymap();
    login_prompt();
    char motd[512];
    read_file_text("/etc/motd", motd, sizeof(motd), "Welcome to Tiramisu.");
    kprintf("\n%s\n\n", motd);

    for (;;) {
        char line[LINE_MAX];
        char *argv[ARGV_MAX];
        prompt();
        read_line(line, sizeof(line));
        int argc = split_args(line, argv);
        run_tokens(argc, argv);
    }
}
