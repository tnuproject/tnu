#include <tnu/libc.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        println("usage: login USER");
        return 1;
    }
    print("login: kernel-shell command handles session switch for ");
    println(argv[1]);
    return 0;
}
