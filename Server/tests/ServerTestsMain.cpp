#include "TestHarness.h"

int main(int argc, char **argv) {
    return test::runAll(argc > 1 ? argv[1] : "");
}

