/*
 * bootd - Tiramisu Boot Daemon
 * Service manager for TNU
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

#ifndef SIGCHLD
#define SIGCHLD 17
#endif
#ifndef SIGTERM
#define SIGTERM 15
#endif
#ifndef SIGINT
#define SIGINT 2
#endif
#ifndef WNOHANG
#define WNOHANG 1
#endif

/* Provide missing functions */
extern char *strtok(char *str, const char *delim);
extern int usleep(unsigned long usec);
extern void _exit(int status);

#define MAX_SERVICES 16
#define CONFIG_FILE "/etc/bootd.conf"
#define PID_FILE "/var/run/bootd.pid"

typedef enum {
    SERVICE_STOPPED,
    SERVICE_STARTING,
    SERVICE_RUNNING,
    SERVICE_STOPPING,
    SERVICE_FAILED
} service_state_t;

typedef struct {
    char name[32];
    char command[256];
    char args[256];
    int auto_start;
    int restart_on_failure;
    int pid;
    service_state_t state;
} service_t;

static service_t services[MAX_SERVICES];
static int service_count = 0;
static volatile int running = 1;

void load_config(void)
{
    FILE *fp = fopen(CONFIG_FILE, "r");
    if (!fp) {
        /* Default services */
        strcpy(services[0].name, "iwlwifi");
        strcpy(services[0].command, "/sbin/iwlwifi");
        strcpy(services[0].args, "");
        services[0].auto_start = 1;
        services[0].restart_on_failure = 1;
        services[0].pid = -1;
        services[0].state = SERVICE_STOPPED;
        service_count = 1;
        return;
    }

    char line[512];
    while (fgets(line, sizeof(line), fp) && service_count < MAX_SERVICES) {
        /* Skip comments and empty lines */
        if (line[0] == '#' || line[0] == '\n') continue;

        service_t *s = &services[service_count];
        memset(s, 0, sizeof(*s));
        
        /* Parse: name:command:args:auto_start:restart */
        char *p = line;
        char *field = p;
        int field_num = 0;
        
        while (*p && field_num < 5) {
            if (*p == ':' || *p == '\n') {
                *p = '\0';
                switch (field_num) {
                case 0: strncpy(s->name, field, 31); break;
                case 1: strncpy(s->command, field, 255); break;
                case 2: strncpy(s->args, field, 255); break;
                case 3: s->auto_start = atoi(field); break;
                case 4: s->restart_on_failure = atoi(field); break;
                }
                field = p + 1;
                field_num++;
            }
            p++;
        }
        
        if (field_num >= 3) {
            s->pid = -1;
            s->state = SERVICE_STOPPED;
            service_count++;
        }
    }
    
    fclose(fp);
}

int start_service(service_t *s)
{
    if (s->state == SERVICE_RUNNING || s->state == SERVICE_STARTING) {
        return 0;
    }

    s->state = SERVICE_STARTING;
    
    pid_t pid = fork();
    if (pid < 0) {
        s->state = SERVICE_FAILED;
        return -1;
    }
    
    if (pid == 0) {
        /* Child process */
        /* Redirect stdout/stderr to log file */
        char logfile[64];
        snprintf(logfile, sizeof(logfile), "/var/log/%s.log", s->name);
        int fd = open(logfile, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        
        /* Execute service */
        char *argv[16];
        int argc = 0;
        argv[argc++] = s->command;
        
        /* Parse args */
        char args_copy[256];
        strncpy(args_copy, s->args, sizeof(args_copy) - 1);
        args_copy[sizeof(args_copy) - 1] = '\0';
        
        char *tok = strtok(args_copy, " ");
        while (tok && argc < 15) {
            argv[argc++] = tok;
            tok = strtok(NULL, " ");
        }
        argv[argc] = NULL;
        
        execv(s->command, argv);
        _exit(1);
    }
    
    /* Parent */
    s->pid = pid;
    s->state = SERVICE_RUNNING;
    printf("bootd: started service '%s' (pid %d)\n", s->name, pid);
    return 0;
}

void stop_service(service_t *s)
{
    if (s->state != SERVICE_RUNNING && s->state != SERVICE_STARTING) {
        return;
    }
    
    s->state = SERVICE_STOPPING;
    if (s->pid > 0) {
        kill(s->pid, SIGTERM);
        /* Wait up to 5 seconds for graceful shutdown */
        int status;
        int waited = 0;
        while (waited < 5000) {
            pid_t result = waitpid(s->pid, &status, WNOHANG);
            if (result == s->pid) {
                break;
            }
            usleep(10000); /* 10ms */
            waited += 10;
        }
        /* Force kill if still running */
        if (s->state != SERVICE_STOPPED) {
            kill(s->pid, SIGKILL);
        }
    }
    s->pid = -1;
    s->state = SERVICE_STOPPED;
}

void check_services(void)
{
    int status;
    pid_t pid;
    
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < service_count; i++) {
            if (services[i].pid == pid) {
                services[i].pid = -1;
                if (services[i].restart_on_failure && 
                    services[i].state != SERVICE_STOPPING) {
                    printf("bootd: service '%s' exited, restarting...\n", 
                           services[i].name);
                    services[i].state = SERVICE_STOPPED;
                    start_service(&services[i]);
                } else {
                    services[i].state = SERVICE_STOPPED;
                    printf("bootd: service '%s' stopped\n", services[i].name);
                }
                break;
            }
        }
    }
}

void signal_handler(int sig)
{
    if (sig == SIGTERM || sig == SIGINT) {
        running = 0;
    }
}

int main(int argc, char **argv)
{
    /* Check if already running */
    int pid_fd = open(PID_FILE, O_RDONLY);
    if (pid_fd >= 0) {
        char buf[16];
        int n = read(pid_fd, buf, sizeof(buf) - 1);
        close(pid_fd);
        if (n > 0) {
            buf[n] = '\0';
            int pid = atoi(buf);
            if (pid > 0 && kill(pid, 0) == 0) {
                fprintf(stderr, "bootd: already running (pid %d)\n", pid);
                return 1;
            }
        }
    }
    
    /* Write PID file */
    pid_fd = open(PID_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (pid_fd >= 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d\n", getpid());
        write(pid_fd, buf, strlen(buf));
        close(pid_fd);
    }
    
    /* Setup signal handlers */
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);
    signal(SIGCHLD, SIG_IGN); /* Auto-reap children */
    
    /* Load configuration */
    load_config();
    
    printf("bootd: starting %d services...\n", service_count);
    
    /* Start auto-start services */
    for (int i = 0; i < service_count; i++) {
        if (services[i].auto_start) {
            start_service(&services[i]);
        }
    }
    
    /* Main loop */
    while (running) {
        check_services();
        usleep(100000); /* 100ms */
    }
    
    /* Stop all services */
    printf("bootd: stopping services...\n");
    for (int i = 0; i < service_count; i++) {
        stop_service(&services[i]);
    }
    
    unlink(PID_FILE);
    return 0;
}
