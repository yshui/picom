#include <stdio.h>
#include "test.h"

int main() {
#ifdef UNIT_TEST
	run_tests();
#endif
}

TEST_CASE(test1) {
	printf("test1\n");
	SHOULD_EQUAL(1, 0);
}
