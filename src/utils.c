#include "utils.h"

#include <stdio.h>
#include <string.h>

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
