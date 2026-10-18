#ifndef PLATFORM_CTYPE_H
#define PLATFORM_CTYPE_H
static inline int isdigit(int value) { return value >= '0' && value <= '9'; }
static inline int isspace(int value) { return value == ' ' || (value >= 9 && value <= 13); }
static inline int isxdigit(int value) { return isdigit(value) || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F'); }
static inline int tolower(int value) { return value >= 'A' && value <= 'Z' ? value + 32 : value; }
#endif
