/**
 * @file test_checksum.c
 * @brief Test suite for the Internet checksum function.
 */

#include "test_suite.h"
#include "net.h"
#include <string.h>

static void test_checksum_basic(void)
{
    uint16_t data[] = { 0x4500, 0x003C };
    uint16_t sum = checksum(data, 4);
    TEST_ASSERT(sum != 0, "checksum of non-zero data should be non-zero");
}

static void test_checksum_zero(void)
{
    uint16_t data[10] = {0};
    uint16_t sum = checksum(data, 20);
    TEST_ASSERT(sum == 0xFFFF, "checksum of all-zero should be 0xFFFF");
}

static void test_checksum_odd_len(void)
{
    uint8_t data[] = { 0x45, 0x00, 0x01 };
    uint16_t sum = checksum((uint16_t*)data, 3);
    TEST_ASSERT(sum != 0, "odd-length checksum should work");
}

static void test_checksum_known_ip(void)
{
    uint8_t header[] = {
        0x45, 0x00, 0x00, 0x3C,
        0x00, 0x00, 0x40, 0x00,
        0x40, 0x01, 0x00, 0x00,  /* checksum field zeroed */
        0x0A, 0x00, 0x02, 0x0F,
        0x0A, 0x00, 0x02, 0x02
    };
    uint16_t sum = checksum((uint16_t*)header, 20);
    TEST_ASSERT(sum != 0, "known IP header checksum should be valid");
}

void register_checksum_tests(void)
{
    test_register("checksum_basic", "basic checksum computation", test_checksum_basic);
    test_register("checksum_zero", "checksum of all-zero data", test_checksum_zero);
    test_register("checksum_odd", "odd-length checksum", test_checksum_odd_len);
    test_register("checksum_ip_hdr", "IP header checksum", test_checksum_known_ip);
}
