/*
 * bootd - Tiramisu session service manager
 *
 * Services start only after login. Configuration lives in /etc/bootd.conf and
 * runtime state/control lives in /var/run.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>

extern char *strtok(char *str, const char *delim);
extern int usleep(unsigned long usec);
extern void _exit(int status);

#define BOOTD_CONFIG_FILE "/etc/bootd.conf"
#define BOOTD_PID_FILE "/var/run/bootd.pid"
#define BOOTD_CMD_FILE "/var/run/bootd.cmd"
#define BOOTD_STATE_FILE "/var/run/bootd.state"
#define BOOTD_LOG_DIR "/var/log"

#define MAX_SERVICES 32
#define MAX_LINE 512

typedef enum {
    BOOTD_STOPPED = 0,
    BOOTD_RUNNING = 1,
    BOOTD_FAILED = 2
} bootd_state_t;

typedef struct {
    char name[32];
    char command[256];
    char args[256];
    int enabled;
    int restart_on_failure;
    int pid;
    int ever_started;
    bootd_state_t state;
} bootd_service_t;

static bootd_service_t services[MAX_SERVICES];
static int service_count = 0;

static int write_all_fd(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n <= 0) {
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

static void strip_newline(char *s)
{
    if (!s) {
        return;
    }
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == '\r' || s[i] == '\n') {
            s[i] = '\0';
            return;
        }
    }
}

static const char *bootd_state_name(bootd_state_t state)
{
    switch (state) {
    case BOOTD_RUNNING:
        return "running";
    case BOOTD_FAILED:
        return "failed";
    case BOOTD_STOPPED:
    default:
        return "stopped";
    }
}

static void bootd_prepare_paths(void)
{
    mkdir("/var", 0755);
    mkdir("/var/run", 0755);
    mkdir("/var/log", 0755);
    mkdir("/etc", 0755);
}

static int bootd_find_service(const char *name)
{
    for (int i = 0; i < service_count; i++) {
        if (strcmp(services[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static void bootd_set_default_config(void)
{
    memset(services, 0, sizeof(services));
    strcpy(services[0].name, "iwlwifi");
    strcpy(services[0].command, "/bin/wifi");
    strcpy(services[0].args, "start wlan0");
    services[0].enabled = 1;
    services[0].restart_on_failure = 1;
    services[0].pid = -1;
    services[0].state = BOOTD_STOPPED;
    service_count = 1;
}

static int bootd_parse_service_line(char *line, bootd_service_t *svc)
{
    char *fields[5];
    int field_count = 0;
    char *cursor = line;

    while (field_count < 5) {
        fields[field_count++] = cursor;
        cursor = strchr(cursor, ':');
        if (!cursor) {
            break;
        }
        *cursor = '\0';
        cursor++;
    }
    if (field_count < 5) {
        return -1;
    }

    memset(svc, 0, sizeof(*svc));
    strncpy(svc->name, fields[0], sizeof(svc->name) - 1);
    strncpy(svc->command, fields[1], sizeof(svc->command) - 1);
    strncpy(svc->args, fields[2], sizeof(svc->args) - 1);
    svc->enabled = atoi(fields[3]) ? 1 : 0;
    svc->restart_on_failure = atoi(fields[4]) ? 1 : 0;
    svc->pid = -1;
    svc->state = BOOTD_STOPPED;
    return svc->name[0] && svc->command[0] ? 0 : -1;
}

static void bootd_load_config(void)
{
    FILE *fp = fopen(BOOTD_CONFIG_FILE, "r");
    char line[MAX_LINE];

    service_count = 0;
    if (!fp) {
        bootd_set_default_config();
        return;
    }

    while (fgets(line, sizeof(line), fp) && service_count < MAX_SERVICES) {
        bootd_service_t svc;
        strip_newline(line);
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        if (bootd_parse_service_line(line, &svc) == 0) {
            services[service_count++] = svc;
        }
    }
    fclose(fp);

    if (service_count == 0) {
        bootd_set_default_config();
    }
}

static int bootd_save_config(void)
{
    int fd = open(BOOTD_CONFIG_FILE, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        return -1;
    }

    if (write_all_fd(fd, "# name:command:args:enabled:restart\n", 36) < 0) {
        close(fd);
        return -1;
    }

    for (int i = 0; i < service_count; i++) {
        char line[MAX_LINE];
        int n = snprintf(line, sizeof(line), "%s:%s:%s:%d:%d\n",
                         services[i].name,
                         services[i].command,
                         services[i].args,
                         services[i].enabled,
                         services[i].restart_on_failure);
        if (n < 0 || n >= (int)sizeof(line) ||
            write_all_fd(fd, line, (size_t)n) < 0) {
            close(fd);
            return -1;
        }
    }

    close(fd);
    return 0;
}

static int bootd_write_pid(void)
{
    char buf[32];
    int fd = open(BOOTD_PID_FILE, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        return -1;
    }
    int n = snprintf(buf, sizeof(buf), "%d\n", getpid());
    if (n < 0 || write_all_fd(fd, buf, (size_t)n) < 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static int bootd_write_state(void)
{
    int fd = open(BOOTD_STATE_FILE, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        return -1;
    }

    for (int i = 0; i < service_count; i++) {
        char line[MAX_LINE];
        int n = snprintf(line, sizeof(line),
                         "%s %s pid=%d enabled=%d restart=%d started=%d\n",
                         services[i].name,
                         bootd_state_name(services[i].state),
                         services[i].pid,
                         services[i].enabled,
                         services[i].restart_on_failure,
                         services[i].ever_started);
        if (n < 0 || n >= (int)sizeof(line) ||
            write_all_fd(fd, line, (size_t)n) < 0) {
            close(fd);
            return -1;
        }
    }

    close(fd);
    return 0;
}

static int bootd_exec_service(bootd_service_t *svc)
{
    char *argv[32];
    char args_copy[256];
    int argc = 0;

    argv[argc++] = svc->command;
    strncpy(args_copy, svc->args, sizeof(args_copy) - 1);
    args_copy[sizeof(args_copy) - 1] = '\0';

    char *tok = strtok(args_copy, " ");
    while (tok && argc < (int)(sizeof(argv) / sizeof(argv[0])) - 1) {
        argv[argc++] = tok;
        tok = strtok(NULL, " ");
    }
    argv[argc] = NULL;

    execv(svc->command, argv);
    return -1;
}

static int bootd_start_service(bootd_service_t *svc)
{
    if (!svc || svc->state == BOOTD_RUNNING) {
        return 0;
    }

    pid_t pid = fork();
    if (pid < 0) {
        svc->state = BOOTD_FAILED;
        svc->pid = -1;
        return -1;
    }

    if (pid == 0) {
        char log_path[128];
        snprintf(log_path, sizeof(log_path), "%s/%s.log", BOOTD_LOG_DIR, svc->name);
        int log_fd = open(log_path, O_CREAT | O_APPEND | O_WRONLY, 0644);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        }
        bootd_exec_service(svc);
        _exit(127);
    }

    svc->pid = pid;
    svc->state = BOOTD_RUNNING;
    svc->ever_started = 1;
    printf("bootd: started %s (pid %d)\n", svc->name, pid);
    bootd_write_state();
    return 0;
}

static void bootd_reap_services(void)
{
    for (int i = 0; i < service_count; i++) {
        int status = 0;
        pid_t pid = services[i].pid;
        if (pid <= 0) {
            continue;
        }
        pid_t rc = waitpid(pid, &status, WNOHANG);
        if (rc != pid) {
            continue;
        }
        services[i].pid = -1;
        services[i].state = (status == 0) ? BOOTD_STOPPED : BOOTD_FAILED;
        printf("bootd: %s exited with status %d\n", services[i].name, status);
        if (services[i].enabled && services[i].restart_on_failure && status != 0) {
            bootd_start_service(&services[i]);
        }
    }

    bootd_write_state();
}

static int bootd_read_command(char *buf, size_t size)
{
    int fd = open(BOOTD_CMD_FILE, O_RDONLY, 0);
    if (fd < 0) {
        return -1;
    }
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    unlink(BOOTD_CMD_FILE);
    if (n <= 0) {
        return -1;
    }
    buf[n] = '\0';
    strip_newline(buf);
    return 0;
}

static int bootd_send_command(int argc, char **argv)
{
    int fd = open(BOOTD_CMD_FILE, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        printf("bootd: daemon control channel unavailable\n");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        if (i > 1 && write_all_fd(fd, " ", 1) < 0) {
            close(fd);
            return 1;
        }
        if (write_all_fd(fd, argv[i], strlen(argv[i])) < 0) {
            close(fd);
            return 1;
        }
    }
    if (write_all_fd(fd, "\n", 1) < 0) {
        close(fd);
        return 1;
    }

    close(fd);
    printf("bootd: queued command\n");
    return 0;
}

static void bootd_print_service_line(const bootd_service_t *svc)
{
    printf("%s\t%s\tenabled=%d\trestart=%d\tpid=%d\tcmd=%s %s\n",
           svc->name,
           bootd_state_name(svc->state),
           svc->enabled,
           svc->restart_on_failure,
           svc->pid,
           svc->command,
           svc->args);
}

static int bootd_print_config(void)
{
    bootd_load_config();
    for (int i = 0; i < service_count; i++) {
        bootd_print_service_line(&services[i]);
    }
    return 0;
}

static int bootd_set_enabled(const char *name, int enabled)
{
    bootd_load_config();
    int idx = bootd_find_service(name);
    if (idx < 0) {
        printf("bootd: unknown service: %s\n", name);
        return 1;
    }
    services[idx].enabled = enabled ? 1 : 0;
    if (bootd_save_config() < 0) {
        printf("bootd: failed to update %s\n", BOOTD_CONFIG_FILE);
        return 1;
    }
    printf("bootd: %s %s\n", enabled ? "enabled" : "disabled", name);
    return 0;
}

static int bootd_show_status(void)
{
    char buf[2048];
    int fd = open(BOOTD_STATE_FILE, O_RDONLY, 0);
    if (fd < 0) {
        printf("bootd: no runtime state yet\n");
        return 1;
    }
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        printf("bootd: no runtime state yet\n");
        return 1;
    }
    buf[n] = '\0';
    printf("%s", buf);
    return 0;
}

static int bootd_handle_runtime_command(const char *cmdline)
{
    char copy[MAX_LINE];
    char *argv[4];
    int argc = 0;

    strncpy(copy, cmdline, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';

    char *tok = strtok(copy, " ");
    while (tok && argc < (int)(sizeof(argv) / sizeof(argv[0]))) {
        argv[argc++] = tok;
        tok = strtok(NULL, " ");
    }

    if (argc == 0) {
        return 0;
    }
    if (strcmp(argv[0], "start") == 0 && argc >= 2) {
        int idx = bootd_find_service(argv[1]);
        if (idx < 0) {
            printf("bootd: unknown service: %s\n", argv[1]);
            return 1;
        }
        return bootd_start_service(&services[idx]) < 0 ? 1 : 0;
    }
    if ((strcmp(argv[0], "stop") == 0 || strcmp(argv[0], "kill") == 0) && argc >= 2) {
        int idx = bootd_find_service(argv[1]);
        int status = 0;
        if (idx < 0) {
            printf("bootd: unknown service: %s\n", argv[1]);
            return 1;
        }
        if (services[idx].pid <= 0 || services[idx].state != BOOTD_RUNNING) {
            services[idx].pid = -1;
            services[idx].state = BOOTD_STOPPED;
            return 0;
        }
        pid_t target = services[idx].pid;
        if (kill(target, strcmp(argv[0], "kill") == 0 ? 9 : 15) < 0) {
            printf("bootd: failed to stop %s\n", services[idx].name);
            return 1;
        }
        for (int tries = 0; tries < 50; tries++) {
            if (waitpid(target, &status, WNOHANG) == target) {
                break;
            }
            usleep(10000);
        }
        services[idx].pid = -1;
        services[idx].state = BOOTD_STOPPED;
        printf("bootd: stopped %s\n", services[idx].name);
        return 0;
    }
    if (strcmp(argv[0], "restart") == 0 && argc >= 2) {
        int idx = bootd_find_service(argv[1]);
        int status = 0;
        if (idx < 0) {
            printf("bootd: unknown service: %s\n", argv[1]);
            return 1;
        }
        if (services[idx].pid > 0 && services[idx].state == BOOTD_RUNNING) {
            pid_t target = services[idx].pid;
            if (kill(target, 15) < 0) {
                printf("bootd: failed to stop %s for restart\n", services[idx].name);
                return 1;
            }
            for (int tries = 0; tries < 50; tries++) {
                if (waitpid(target, &status, WNOHANG) == target) {
                    break;
                }
                usleep(10000);
            }
            services[idx].pid = -1;
            services[idx].state = BOOTD_STOPPED;
        }
        return bootd_start_service(&services[idx]) < 0 ? 1 : 0;
    }
    if (strcmp(argv[0], "status") == 0) {
        return bootd_write_state();
    }

    printf("bootd: unknown daemon command: %s\n", argv[0]);
    return 1;
}

static int bootd_run_daemon(void)
{
    char cmdline[MAX_LINE];

    bootd_prepare_paths();
    bootd_load_config();
    bootd_write_pid();
    bootd_write_state();

    printf("bootd: session daemon ready with %d service(s)\n", service_count);
    for (int i = 0; i < service_count; i++) {
        if (services[i].enabled) {
            bootd_start_service(&services[i]);
        }
    }

    for (;;) {
        bootd_reap_services();
        if (bootd_read_command(cmdline, sizeof(cmdline)) == 0) {
            bootd_handle_runtime_command(cmdline);
            bootd_write_state();
        }
        usleep(100000);
    }
}

static void bootd_usage(void)
{
    printf("usage: bootd\n");
    printf("       bootd list\n");
    printf("       bootd status\n");
    printf("       bootd start NAME\n");
    printf("       bootd stop NAME\n");
    printf("       bootd restart NAME\n");
    printf("       bootd kill NAME\n");
    printf("       bootd enable NAME\n");
    printf("       bootd disable NAME\n");
}

int main(int argc, char **argv)
{
    if (argc <= 1) {
        return bootd_run_daemon();
    }

    if (strcmp(argv[1], "list") == 0) {
        return bootd_print_config();
    }
    if (strcmp(argv[1], "status") == 0) {
        return bootd_show_status();
    }
    if (strcmp(argv[1], "enable") == 0 && argc >= 3) {
        return bootd_set_enabled(argv[2], 1);
    }
    if (strcmp(argv[1], "disable") == 0 && argc >= 3) {
        return bootd_set_enabled(argv[2], 0);
    }
    if ((strcmp(argv[1], "start") == 0 || strcmp(argv[1], "stop") == 0 ||
         strcmp(argv[1], "restart") == 0 || strcmp(argv[1], "kill") == 0) && argc >= 3) {
        return bootd_send_command(argc, argv);
    }

    bootd_usage();
    return 1;
}
