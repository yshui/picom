all: normal unittest

# A normal binary, compiled without unit tests
normal: test_test1.c test_test2.c ../../test.h
	gcc -g test_test1.c test_test2.c -I../../ -o $@
# A binary to run unit tests
unittest: test_test1.c test_test2.c ../../test.h
	gcc -g test_test1.c test_test2.c -DUNIT_TEST -I../../ -o $@
