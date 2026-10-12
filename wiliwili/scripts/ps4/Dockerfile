FROM xfangfang/pacbrew:latest AS builder

MAINTAINER xfangfang <xfangfang@126.com>

RUN apt-get update && \
    apt-get install -y --no-install-recommends xz-utils lzma fakeroot file \
      autoconf automake libtool yasm makepkg python3 python3-setuptools && \
    adduser --gecos '' --disabled-password builder

COPY musl/PKGBUILD /src/musl/PKGBUILD
COPY musl/dns.patch /src/musl/dns.patch
COPY SDL2/PKGBUILD /src/SDL2/PKGBUILD
COPY SDL2/sdl2.patch /src/SDL2/sdl2.patch
COPY mbedtls/PKGBUILD /src/mbedtls/PKGBUILD
COPY libcurl/PKGBUILD /src/libcurl/PKGBUILD
COPY ffmpeg/PKGBUILD /src/ffmpeg/PKGBUILD
COPY libmpv/PKGBUILD /src/libmpv/PKGBUILD
COPY libmpv/mpv.patch /src/libmpv/mpv.patch

RUN mkdir -p /pkgs && \
    chown -R builder:builder /src && \
    rm -rf /src/*/*.pkg.tar.xz

RUN su - builder -c "cd /src/musl && makepkg" && \
    pacman -U --noconfirm /src/musl/*.pkg.tar.gz
RUN su - builder -c "cd /src/SDL2 && makepkg" && \
    pacman -U --noconfirm /src/SDL2/*.pkg.tar.gz
RUN su - builder -c "cd /src/mbedtls && makepkg" && \
    pacman -U --noconfirm /src/mbedtls/*.pkg.tar.gz
RUN su - builder -c "cd /src/libcurl && makepkg" && \
    pacman -U --noconfirm /src/libcurl/*.pkg.tar.gz
RUN su - builder -c "cd /src/ffmpeg && makepkg" && \
    pacman -U --noconfirm /src/ffmpeg/*.pkg.tar.gz
RUN su - builder -c "cd /src/libmpv && makepkg" && \
    pacman -U --noconfirm /src/libmpv/*.pkg.tar.gz

RUN cp /src/*/*.pkg.tar.gz /pkgs

FROM xfangfang/pacbrew:latest

COPY --from=builder /pkgs /pkgs

RUN pacman -U --noconfirm /pkgs/*.pkg.tar.gz && \
    rm -rf /pkgs && \
    yes | pacman -Scc

ENV PATH="/opt/pacbrew/ps4/openorbis/bin:/opt/pacbrew/ps4/openorbis/usr/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
ENV OPENORBIS="/opt/pacbrew/ps4/openorbis"