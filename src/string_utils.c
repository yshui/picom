#include <string.h>

#include "compiler.h"
#include "string_utils.h"
#include "utils.h"
/**
 * Allocate the space and copy a string.
 */
char *mstrcpy(const char *src) {
  auto str = ccalloc(strlen(src) + 1, char);

  strcpy(str, src);

  return str;
}

/**
 * Allocate the space and copy a string.
 */
char *mstrncpy(const char *src, unsigned len) {
  auto str = ccalloc(len + 1, char);

  strncpy(str, src, len);
  str[len] = '\0';

  return str;
}

/**
 * Allocate the space and join two strings.
 */
char *mstrjoin(const char *src1, const char *src2) {
  auto str = ccalloc(strlen(src1)+strlen(src2)+1, char);

  strcpy(str, src1);
  strcat(str, src2);

  return str;
}

/**
 * Allocate the space and join two strings;
 */
char *
mstrjoin3(const char *src1, const char *src2, const char *src3) {
  auto str = ccalloc(strlen(src1)+strlen(src2)+strlen(src3)+1, char);

  strcpy(str, src1);
  strcat(str, src2);
  strcat(str, src3);

  return str;
}

/**
 * Concatenate a string on heap with another string.
 */
void mstrextend(char **psrc1, const char *src2) {
  *psrc1 = crealloc(*psrc1, (*psrc1 ? strlen(*psrc1) : 0)+strlen(src2)+1);

  strcat(*psrc1, src2);
}
