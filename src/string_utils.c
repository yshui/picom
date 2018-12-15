#include <string.h>

#include "string_utils.h"
#include "utils.h"
/**
 * Allocate the space and copy a string.
 */
char *mstrcpy(const char *src) {
  char *str = cmalloc(strlen(src) + 1, char);

  strcpy(str, src);

  return str;
}

/**
 * Allocate the space and copy a string.
 */
char *mstrncpy(const char *src, unsigned len) {
  char *str = cmalloc(len + 1, char);

  strncpy(str, src, len);
  str[len] = '\0';

  return str;
}

/**
 * Allocate the space and join two strings.
 */
char *mstrjoin(const char *src1, const char *src2) {
  char *str = cmalloc(strlen(src1) + strlen(src2) + 1, char);

  strcpy(str, src1);
  strcat(str, src2);

  return str;
}

/**
 * Allocate the space and join two strings;
 */
char *
mstrjoin3(const char *src1, const char *src2, const char *src3) {
  char *str = cmalloc(strlen(src1) + strlen(src2)
        + strlen(src3) + 1, char);

  strcpy(str, src1);
  strcat(str, src2);
  strcat(str, src3);

  return str;
}

/**
 * Concatenate a string on heap with another string.
 */
void mstrextend(char **psrc1, const char *src2) {
  *psrc1 = crealloc(*psrc1, (*psrc1 ? strlen(*psrc1): 0) + strlen(src2) + 1,
      char);

  strcat(*psrc1, src2);
}
