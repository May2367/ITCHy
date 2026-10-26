#ifndef ITCHY_UTILS_H
#define ITCHY_UTILS_H

#include <stddef.h>
#include <stdint.h>

uint16_t read_uint16_be(const unsigned char *buf);
uint32_t read_uint32_be(const unsigned char *buf);
uint64_t read_uint48_be(const unsigned char *buf);
uint64_t read_uint64_be(const unsigned char *buf);
void copy_itch_string(char *dest, size_t dest_size, const unsigned char *src, size_t src_len);
int ticker_matches(const char *filter, const char *stock);
void format_price(char *dest, size_t dest_size, uint32_t price);

#endif
