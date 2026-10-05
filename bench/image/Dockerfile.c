# syntax=docker/dockerfile:1
# Production-image shape for the C port, matching how the pinned harness runs
# every other app: one process, its own public listener, the Rails storage
# layout bind-mounted at /rails/storage/{db,files}, and HTTP_PORT/TARGET_PORT
# in the environment (bench/run --network host).
#
# Build context: the small directory bench/run-c assembles under
# bench/.work/image/ (campfire, tests/fixtures/assets, entrypoint.sh) so the
# repository root -- which holds tmp/ and build/ -- is never sent to the
# daemon:
#
#   docker build -f bench/image/Dockerfile.c -t campfire-c:bench bench/.work/image
#
# The binary is built on the host with `make bench` (the Makefile links every
# vendor library statically; only libc/libm are dynamic), so BASE_IMAGE must
# provide a glibc at least as new as the build host's.  Pin BASE_IMAGE by
# digest when recording measurements.

ARG BASE_IMAGE=fedora:latest
FROM ${BASE_IMAGE}

WORKDIR /app
COPY campfire /app/campfire
COPY tests/fixtures/assets /app/tests/fixtures/assets
COPY entrypoint.sh /app/entrypoint.sh
RUN chmod 0755 /app/campfire /app/entrypoint.sh

# The harness's HTTP_PORT is the public listener; TARGET_PORT is unused because
# this is a single-process server (the reference's front/app split does not
# exist here).  EXPOSE is documentation only: the harness uses --network host.
EXPOSE 80
ENTRYPOINT ["/app/entrypoint.sh"]
