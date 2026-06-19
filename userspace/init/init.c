#include <tnu/libc.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    println("init: starting login");
    int pid = spawn("/bin/login");
    if (pid < 0) {
        pid = spawn("/sbin/login");
    }
    if (pid >= 0) {
        wait(pid);
    }
    println("init: login exited");
    return 0;
}
