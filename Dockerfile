# syntax=docker/dockerfile:1.6

FROM alpine:latest AS builder

RUN apk add --no-cache \
    git \
    patch \
    meson \
    ninja \
    gcc \
    g++ \
    linux-headers \
    pigz \
    ffmpeg-dev \
    mbedtls-dev \
    libsrtp-dev \
    libsrt-dev \
    librist-dev \
    cjson-dev

WORKDIR /src
COPY . .

RUN meson setup build \
    -DWITH_AV=true \
    -DDEBUG=3 \
    && ninja -C build install

FROM alpine:latest

RUN apk add --no-cache \
    libstdc++ \
    libgcc \
    ffmpeg-libs \
    mbedtls \
    libsrtp \
    libsrt \
    librist \
    cjson

COPY --from=builder /usr/local/ /usr/local/

VOLUME ["/var/lib/mistserver"]
EXPOSE 4242 8080 1935 5554 4200 8889/udp 18203/udp

LABEL org.opencontainers.image.authors="frameworks@livepeer.com"
LABEL org.opencontainers.image.source="https://github.com/Livepeer-FrameWorks/mistserver"

ENTRYPOINT ["MistController"]
CMD ["-c", "/etc/mistserver.conf"]
HEALTHCHECK CMD ["MistUtilHealth"]
