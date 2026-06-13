#include <ctype.h>

int isdigit(int c)
{
    return c >= '0' && c <= '9';
}

int isupper(int c)
{
    return c >= 'A' && c <= 'Z';
}

int islower(int c)
{
    return c >= 'a' && c <= 'z';
}

int isalpha(int c)
{
    return isupper(c) || islower(c);
}

int isalnum(int c)
{
    return isalpha(c) || isdigit(c);
}

int isblank(int c)
{
    return c == ' ' || c == '\t';
}

int ispunct(int c)
{
    return c >= 0x21 && c <= 0x7e && !isalnum(c) && !isspace(c);
}

int isxdigit(int c)
{
    return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int isspace(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

int toupper(int c)
{
    return islower(c) ? c - 'a' + 'A' : c;
}

int tolower(int c)
{
    return isupper(c) ? c - 'A' + 'a' : c;
}
