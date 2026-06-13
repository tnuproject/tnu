#ifndef TNU_APPLETS_H
#define TNU_APPLETS_H

#include <tnu/types.h>

bool tnu_applet_is_command(const char *name);
const char *tnu_applet_list(void);
int tnu_applet_help(const char *name);
int tnu_applet_run(int argc, char **argv, const char *stdin_data);

#endif
