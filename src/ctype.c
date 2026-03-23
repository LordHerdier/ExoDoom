#include "ctype.h"

int isalpha(int c) { return ((unsigned)c | 32) - 'a' < 26; }
int isdigit(int c) { return (unsigned)c - '0' < 10; }
int isspace(int c)  { return c == ' ' || (unsigned)c - '\t' < 5; }
int toupper(int c)  { return ((unsigned)c - 'a' < 26) ? c - 32 : c; }
int tolower(int c)  { return ((unsigned)c - 'A' < 26) ? c + 32 : c; }
