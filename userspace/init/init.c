#include <tnu/libc.h>

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    println("bootd: starting /bin/tsh");
    int pid = spawn("/bin/tsh");
    if (pid >= 0) {
        wait(pid);
    }
    println("bootd: shell exited");
    return 0;
}
