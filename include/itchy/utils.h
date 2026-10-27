#ifndef ITCHY_UTILS_H
#define ITCHY_UTILS_H

#include <stddef.h>
#include <stdint.h>

static inline uint16_t read_uint16_be(const unsigned char *buf)
{
    return (uint16_t)(((uint16_t)buf[0] << 8) | (uint16_t)buf[1]);
}

static inline uint32_t read_uint32_be(const unsigned char *buf)
{
    return ((uint32_t)buf[0] << 24) |
           ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8) |
           (uint32_t)buf[3];
}

static inline uint64_t read_uint48_be(const unsigned char *buf)
{
    return ((uint64_t)buf[0] << 40) |
           ((uint64_t)buf[1] << 32) |
           ((uint64_t)buf[2] << 24) |
           ((uint64_t)buf[3] << 16) |
           ((uint64_t)buf[4] << 8) |
           (uint64_t)buf[5];
}

static inline uint64_t read_uint64_be(const unsigned char *buf)
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

void copy_itch_string(char *dest, size_t dest_size, const unsigned char *src, size_t src_len);
int ticker_matches(const char *filter, const char *stock);
void format_price(char *dest, size_t dest_size, uint32_t price);

#endif
