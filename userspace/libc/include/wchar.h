#ifndef TNU_WCHAR_H
#define TNU_WCHAR_H

#include <stddef.h>

typedef int wchar_t;
typedef int wint_t;

int wctomb(char *s, wchar_t wc);
int wcwidth(wchar_t wc);

#endif
