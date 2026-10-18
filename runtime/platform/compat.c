#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
char *strcat(char *target, const char *source) {
  strcpy(target + strlen(target), source); return target;
}
long strtol(const char *text, char **end, int base) {
  const char *original = text;
  while (*text == ' ' || (*text >= 9 && *text <= 13)) text++;
  int negative = *text == '-';
  if (*text == '+' || *text == '-') text++;
  if ((base == 0 || base == 16) && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    unsigned char value = (unsigned char)text[2];
    if ((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F')) { base = 16; text += 2; }
  }
  if (!base) base = *text == '0' ? 8 : 10;
  if (base < 2 || base > 36) { if (end) *end = (char *)original; return 0; }
  unsigned long maximum = negative ? (unsigned long)LONG_MAX + 1 : (unsigned long)LONG_MAX;
  unsigned long result = 0;
  int digits = 0, overflow = 0;
  for (;;) {
    unsigned char value = (unsigned char)*text;
    unsigned digit = value >= '0' && value <= '9' ? value - '0' :
      value >= 'a' && value <= 'z' ? value - 'a' + 10 :
      value >= 'A' && value <= 'Z' ? value - 'A' + 10 : 36;
    if (digit >= (unsigned)base) break;
    if (result > (maximum - digit) / (unsigned)base) overflow = 1;
    else if (!overflow) result = result * (unsigned)base + digit;
    digits = 1; text++;
  }
  if (end) *end = (char *)(digits ? text : original);
  if (overflow) return negative ? LONG_MIN : LONG_MAX;
  if (negative && result == (unsigned long)LONG_MAX + 1) return LONG_MIN;
  return negative ? -(long)result : (long)result;
}
static void swap_bytes(unsigned char *left, unsigned char *right, size_t size) {
  while (size--) { unsigned char value = *left; *left++ = *right; *right++ = value; }
}
static void sift(unsigned char *base, size_t count, size_t root, size_t size,
                 int (*compare)(const void *, const void *)) {
  while (root < count / 2) {
    size_t child = root * 2 + 1;
    if (child + 1 < count && compare(base + child * size, base + (child + 1) * size) < 0) child++;
    if (compare(base + root * size, base + child * size) >= 0) return;
    swap_bytes(base + root * size, base + child * size, size); root = child;
  }
}
void qsort(void *pointer, size_t count, size_t size, int (*compare)(const void *, const void *)) {
  if (count < 2 || !size || count > SIZE_MAX / size) return;
  unsigned char *base = pointer;
  for (size_t root = count / 2; root; root--) sift(base, count, root - 1, size, compare);
  while (--count) { swap_bytes(base, base + count * size, size); sift(base, count, 0, size, compare); }
}

unsigned long strtoul(const char *text, char **end, int base) {
  const char *start=text;
  while(*text==' ' || (*text>='\t' && *text<='\r'))text++;
  int negative=*text=='-';
  if(*text=='+' || *text=='-')text++;
  if((base==0 || base==16) && text[0]=='0' && (text[1]=='x' || text[1]=='X') &&
     ((text[2]>='0' && text[2]<='9') || (text[2]>='a' && text[2]<='f') || (text[2]>='A' && text[2]<='F'))) {base=16;text+=2;}
  if(!base)base=*text=='0'?8:10;
  if(base<2 || base>36){if(end)*end=(char *)start;return 0;}
  unsigned long value=0,maximum=~0UL;int any=0,overflow=0;
  for(;;text++) {
    unsigned digit=*text>='0' && *text<='9'?(unsigned)(*text-'0'):*text>='a' && *text<='z'?(unsigned)(*text-'a'+10):*text>='A' && *text<='Z'?(unsigned)(*text-'A'+10):36;
    if(digit>=(unsigned)base)break;
    any=1;if(value>(maximum-digit)/(unsigned)base)overflow=1;
    if(!overflow)value=value*(unsigned)base+digit;
  }
  if(end)*end=(char *)(any?text:start);
  return overflow?maximum:negative?0UL-value:value;
}
