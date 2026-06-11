#include <tnu/libc.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        println("usage: userdel NAME");
        return 1;
    }
    print("userdel: kernel-shell command removes in-memory user ");
    println(argv[1]);
    return 0;
}
