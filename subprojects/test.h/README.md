test.h
======

A very simple, light weight, header only C unit test framework.

# Features

* Easy to use, no dependencies, no setup needed.
* Keep test cases close to the code they test.
* Automatic registration of the test cases.

# Usage
## Setup

Just include the header

```c
#include "test.h"
```

## Defining test cases

```c
TEST_CASE(test_case_name) {
	// Your code here
	// ...
	TEST_EQUAL(1, 0); // Fail
}
```

## Run the test cases

Build your program with `-DUNIT_TEST`, then run your program with `./program --unittest`.

# Hooks

If you define a function `test_h_unittest_setup`, it will be called before any test cases are run.
