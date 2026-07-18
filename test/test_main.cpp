// test_main.cpp — entry point for the host test binary. All test cases live in
// the other test_*.cpp files and self-register via WL_TEST.
#include "wl_test.h"

int main() { return wltest::run_all(); }
