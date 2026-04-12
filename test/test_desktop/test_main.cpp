#include <unity.h>

void setUp(void) {
    // set stuff up here
}

void tearDown(void) {
    // clean stuff up here
}

void test_basic_arithmetic(void) {
    TEST_ASSERT_EQUAL(4, 2 + 2);
}

void test_string_comparison(void) {
    TEST_ASSERT_EQUAL_STRING("PlatformIO", "PlatformIO");
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_basic_arithmetic);
    RUN_TEST(test_string_comparison);
    UNITY_END();

    return 0;
}
