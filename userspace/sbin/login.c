#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <tnu/syscall.h>

long tnu_syscall(long n, long a0, long a1, long a2, long a3, long a4, long a5);

static void strip_newline(char *s)
{
    if (!s) return;
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == '\n' || s[i] == '\r') {
            s[i] = '\0';
            return;
        }
    }
}

static int login_session(const char *user, const char *password)
{
    return (int)tnu_syscall(SYS_LOGIN, (long)user, (long)password, 0, 0, 0, 0);
}

int main(int argc, char **argv)
{
    char user[64];
    char password[128];

    if (argc >= 2) {
        strncpy(user, argv[1], sizeof(user) - 1);
        user[sizeof(user) - 1] = '\0';
    } else {
        printf("Tiramisu login: ");
        fflush(stdout);
        if (!fgets(user, sizeof(user), stdin)) {
            return 1;
        }
        strip_newline(user);
    }

    printf("Password: ");
    fflush(stdout);
    if (!fgets(password, sizeof(password), stdin)) {
        return 1;
    }
    strip_newline(password);

    if (login_session(user, password) < 0) {
        printf("login: authentication failed\n");
        return 1;
    }

    char *sh_argv[] = { "tsh", NULL };
    execv("/bin/tsh", sh_argv);
    execv("/sbin/tsh", sh_argv);
    printf("login: authenticated, but cannot exec shell\n");
    return 1;
}
