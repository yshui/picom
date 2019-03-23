#include <stdio.h>
#include <stdbool.h>
#include "test.h"

int main(int argc, char *const *argv) {
	printf("Hello World!\n");
}

TEST_CASE(test1) {
	TEST_EQUAL(1, 0);
}

TEST_CASE(test2) {
	TEST_TRUE(false);
}
