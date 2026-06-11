#include <tnu/libc.h>

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    println("TNU sysinstall");
    println("disk selection -> partitioning -> TFS format -> bootloader -> base copy");
    println("TNU live system configurator");
    println("The base system seeds only root; create additional users after first boot.");
    return 0;
}
