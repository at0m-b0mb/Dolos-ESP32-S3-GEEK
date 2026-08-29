/* Tiny host test harness for the Aegis engines - no dependencies. */
#ifndef AEGIS_TEST_H
#define AEGIS_TEST_H
#include <stdio.h>
#include <string.h>
static int g_checks = 0, g_fails = 0;
#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { g_fails++; \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)
#define SUITE(name) printf("== %s ==\n", name)
#define TEST_MAIN_BEGIN int main(void) {
#define TEST_MAIN_END \
    printf("\n%d checks, %d failures\n", g_checks, g_fails); \
    return g_fails ? 1 : 0; }
#endif
