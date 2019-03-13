#include <stdio.h>
#include <stdbool.h>
#include "test.h"

int main() {
#ifdef UNIT_TEST
	run_tests();
#endif
}

TEST_CASE(test1) {
	TEST_EQUAL(1, 0);
}

TEST_CASE(test2) {
	TEST_TRUE(false);
}
