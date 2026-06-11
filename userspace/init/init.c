#include <tnu/libc.h>

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    println("launchd: starting /bin/tsh");
    int pid = spawn("/bin/tsh");
    if (pid >= 0) {
        wait(pid);
    }
    println("launchd: shell exited");
    return 0;
}
