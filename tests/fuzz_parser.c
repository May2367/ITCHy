#define _POSIX_C_SOURCE 200809L

#include "parser.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char path[] = "/tmp/itchy_fuzz_XXXXXX";
    ParserRunOptions options = {0};
    Config config = {0};
    gzFile out;
    gzFile in;
    int fd;
    unsigned write_size;

    fd = mkstemp(path);
    if (fd < 0) {
        return 0;
    }

    out = gzdopen(fd, "wb");
    if (out == NULL) {
        close(fd);
        unlink(path);
        return 0;
    }

    write_size = size > 1048576u ? 1048576u : (unsigned)size;
    if (write_size > 0) {
        gzwrite(out, data, write_size);
    }
    gzclose(out);

    strcpy(config.ticker, "AAPL");
    options.max_messages = 4096;

    in = gzopen(path, "rb");
    if (in != NULL) {
        parser_run_stream(in, &config, &options);
        gzclose(in);
    }
    unlink(path);
    return 0;
}
