#pragma once

#include <uthash.h>

#define HASH_ITER2(head, el)                                                             \
	for (__typeof__(head) el = (head), __tmp = el != NULL ? el->hh.next : NULL;      \
	     el != NULL; el = __tmp, __tmp = el != NULL ? el->hh.next : NULL)
