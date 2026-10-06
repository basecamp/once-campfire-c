# Exact reference Debian source pins and build options. External media tools
# stay isolated from the application and from host system executables.
ARG DEBIAN_RELEASE=trixie
ARG LIBVIPS_VERSION=8.16.1-1+deb13u1
ARG LIBVIPS_DSC_SHA256=60205e00d061b9d8072938e04899f2ca2fdac0513068e561d33f7c87fae1ae2e
ARG FFMPEG_VERSION=7:7.1.5-0+deb13u1
ARG FFMPEG_DSC_SHA256=9ed2ed34cbe7f056eeebbe9045c5e2d15e41b5b053fe7c8ba6979a0b6fb081ce


# Toolchain and Debian -dev packages for the external libvips/FFmpeg tools, with deb-src enabled so
# `apt-get source` can fetch the exact Debian sources (the .dsc checksums pin them; dpkg-source
# verifies the tarballs against the .dsc).
FROM docker.io/library/debian:${DEBIAN_RELEASE} AS media-base
RUN sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/debian.sources && \
    apt-get update -qq && \
    apt-get install --no-install-recommends -y \
      build-essential dpkg-dev meson ninja-build nasm \
      libglib2.0-dev libexpat1-dev zlib1g-dev libjpeg62-turbo-dev libspng-dev libpng-dev \
      libwebp-dev libtiff-dev libheif-dev libexif-dev liblcms2-dev libcgif-dev libimagequant-dev \
      libhwy-dev libdav1d-dev libbz2-dev liblzma-dev
WORKDIR /usr/src


# libvips with only the loaders and savers above, built into the library (no modules), with
# Debian's build flags (debian/rules: meson, buildtype plain, hardening=+all).
FROM media-base AS vips
ARG LIBVIPS_VERSION
ARG LIBVIPS_DSC_SHA256
RUN apt-get source -qq vips=${LIBVIPS_VERSION} && \
    echo "${LIBVIPS_DSC_SHA256}  vips_${LIBVIPS_VERSION}.dsc" | sha256sum -c - && \
    cd vips-${LIBVIPS_VERSION%-*} && \
    eval "$(DEB_BUILD_MAINT_OPTIONS=hardening=+all dpkg-buildflags --export=sh)" && \
    meson setup build --buildtype=plain --wrap-mode=nodownload --prefix=/opt/vips --libdir=lib \
      --auto-features=disabled -Dmodules=disabled -Dintrospection=disabled -Dcplusplus=false \
      -Ddeprecated=false -Dexamples=false \
      -Djpeg=enabled -Dspng=enabled -Dpng=enabled -Dwebp=enabled -Dtiff=enabled -Dheif=enabled \
      -Dexif=enabled -Dlcms=enabled -Dcgif=enabled -Dimagequant=enabled -Dhighway=enabled \
      -Dzlib=enabled && \
    meson install -C build --strip


# ffmpeg and ffprobe (shared libavcodec/libavformat/...) from Debian's source with Debian's
# toolchain and version string (debian/rules), so the vectors' version check still applies.
FROM media-base AS ffmpeg
ARG FFMPEG_VERSION
ARG FFMPEG_DSC_SHA256
RUN apt-get source -qq ffmpeg=${FFMPEG_VERSION} && \
    upstream=${FFMPEG_VERSION#*:} && upstream=${upstream%-*} && revision=${FFMPEG_VERSION##*-} && \
    echo "${FFMPEG_DSC_SHA256}  ffmpeg_${FFMPEG_VERSION#*:}.dsc" | sha256sum -c - && \
    cd ffmpeg-${upstream} && \
    ./configure --prefix=/opt/ffmpeg --extra-version="${revision}" --toolchain=hardened \
      --enable-shared --disable-static --disable-doc --disable-ffplay --disable-avdevice \
      --disable-autodetect --disable-network --disable-hwaccels --disable-devices \
      --enable-libdav1d --enable-zlib --enable-bzlib --enable-lzma \
      --disable-encoders --enable-encoder=mjpeg \
      --disable-muxers --enable-muxer=image2 \
      --disable-protocols --enable-protocol=file,pipe \
      --disable-filters \
      --enable-filter=buffer,buffersink,abuffer,abuffersink,format,aformat,null,anull,scale,aresample \
      --enable-filter=select,loop,trim,transpose,hflip,vflip,rotate,crop && \
    make -j4 && \
    make install && \
    strip --strip-unneeded /opt/ffmpeg/lib/*.so.* /opt/ffmpeg/bin/*



FROM media-base AS adapter
COPY cf-vips.c /usr/src/cf-vips.c
COPY --from=vips /opt/vips /opt/vips
RUN PKG_CONFIG_PATH=/opt/vips/lib/pkgconfig cc -std=c11 -O2 -Wall -Wextra -Werror \
    /usr/src/cf-vips.c -o /usr/local/bin/cf-vips $(PKG_CONFIG_PATH=/opt/vips/lib/pkgconfig pkg-config --cflags --libs vips)

FROM media-base AS pinned-tools
COPY --from=adapter /usr/local/bin/cf-vips /usr/local/bin/cf-vips
COPY --from=vips /opt/vips /opt/vips
COPY --from=ffmpeg /opt/ffmpeg /opt/ffmpeg
COPY --from=vips /usr/src/vips_8.16.1-1+deb13u1.dsc /usr/local/share/campfire-media/vips.dsc
COPY --from=vips /usr/src/vips-8.16.1/debian/copyright /usr/local/share/campfire-media/vips.copyright
COPY --from=ffmpeg /usr/src/ffmpeg_7.1.5-0+deb13u1.dsc /usr/local/share/campfire-media/ffmpeg.dsc
COPY --from=ffmpeg /usr/src/ffmpeg-7.1.5/LICENSE.md /usr/local/share/campfire-media/ffmpeg.LICENSE.md
ENV LD_LIBRARY_PATH=/opt/vips/lib:/opt/ffmpeg/lib
ENV PATH=/opt/vips/bin:/opt/ffmpeg/bin:$PATH
