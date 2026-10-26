FROM debian:trixie-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        gcc \
        libc6-dev \
        make \
        uthash-dev \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN make clean build test

ENTRYPOINT ["./itchy"]
CMD ["synthetic", "--benchmark"]
