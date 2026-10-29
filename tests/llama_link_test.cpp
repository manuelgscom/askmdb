#include "doctest.h"

#include "llama.h"

// Checks that llama.cpp builds and links into askmdb_tests.
TEST_SUITE("infer") {
TEST_CASE("llama: llama.cpp links and the backend initialises") {
    llama_backend_init();
    CHECK(llama_max_devices() >= 1);
    llama_backend_free();
}
}  // TEST_SUITE
