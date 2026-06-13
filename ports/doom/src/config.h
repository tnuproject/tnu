/* TNU build config for doomgeneric */
#ifndef CONFIG_H
#define CONFIG_H

#define PACKAGE_STRING "DOOM TNU"
#define PACKAGE_VERSION "TNU"

/* Disable SDL and sound */
#undef  ORIGCODE
#undef  FEATURE_SOUND
#define NO_SOUND 1

/* These are defined in doomgeneric already */

#endif
