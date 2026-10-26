#include "utils.h"

#include <stdio.h>
#include <string.h>

uint16_t read_uint16_be(const unsigned char *buf)
{
    return (uint16_t)(((uint16_t)buf[0] << 8) | (uint16_t)buf[1]);
}

uint32_t read_uint32_be(const unsigned char *buf)
{
    return ((uint32_t)buf[0] << 24) |
           ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8) |
           (uint32_t)buf[3];
}

uint64_t read_uint48_be(const unsigned char *buf)
{
    return ((uint64_t)buf[0] << 40) |
           ((uint64_t)buf[1] << 32) |
           ((uint64_t)buf[2] << 24) |
           ((uint64_t)buf[3] << 16) |
           ((uint64_t)buf[4] << 8) |
           (uint64_t)buf[5];
}

uint64_t read_uint64_be(const unsigned char *buf)
{
    return ((uint64_t)buf[0] << 56) |
           ((uint64_t)buf[1] << 48) |
           ((uint64_t)buf[2] << 40) |
           ((uint64_t)buf[3] << 32) |
           ((uint64_t)buf[4] << 24) |
           ((uint64_t)buf[5] << 16) |
           ((uint64_t)buf[6] << 8) |
           (uint64_t)buf[7];
}

void copy_itch_string(char *dest, size_t dest_size, const unsigned char *src, size_t src_len)
{
    size_t end = src_len;
    size_t n;

    if (dest_size == 0) {
        return;
    }

    while (end > 0 && src[end - 1] == ' ') {
        end--;
    }

    n = end < dest_size - 1 ? end : dest_size - 1;
    memcpy(dest, src, n);
    dest[n] = '\0';
}

int ticker_matches(const char *filter, const char *stock)
{
    if (filter == NULL || filter[0] == '\0') {
        return 1;
    }
    return strcmp(filter, stock) == 0;
}

void format_price(char *dest, size_t dest_size, uint32_t price)
{
    snprintf(dest, dest_size, "%u.%04u", price / 10000u, price % 10000u);
}
