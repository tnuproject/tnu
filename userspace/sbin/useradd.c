#include <tnu/libc.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        println("usage: useradd NAME");
        return 1;
    }
    print("useradd: kernel-shell command creates in-memory user ");
    println(argv[1]);
    return 0;
}
