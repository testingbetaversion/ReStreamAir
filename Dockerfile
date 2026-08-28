# ReStreamAir — the panel. Multi-stage: build with cmake and the two dev
# packages, ship on a slim runtime carrying only their shared libraries.
#
#   docker build -t restreamair .
#   docker run -d --name restreamair -p 8787:8787 -v restreamair-data:/data restreamair
#
# Everything mutable (state.json, logs/, logo-cache.json) is resolved relative
# to the working directory, so /data is the only volume that matters. Back that
# up and you have backed up the install.

FROM ubuntu:24.04 AS build
WORKDIR /src
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       cmake build-essential libcurl4-openssl-dev libxml2-dev \
    && rm -rf /var/lib/apt/lists/*

COPY CMakeLists.txt ./
COPY core ./core
COPY apps ./apps
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"$(nproc)"

# Prove the known-answer vectors pass in the same environment that produced the
# binary, rather than trusting that they did somewhere else.
RUN ./build/restream_selftest

FROM ubuntu:24.04
# libcurl4 fetches manifests and segments, libxml2 parses an MPD, and
# ca-certificates is what lets either touch an https:// origin at all.
RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates curl libcurl4 libxml2 \
    && rm -rf /var/lib/apt/lists/*

# An unprivileged user: nothing here needs root, and the panel binds 8787 rather
# than a privileged port.
RUN useradd --system --create-home --uid 10001 restreamair
WORKDIR /data
COPY --from=build /src/build/restreamair-server /usr/local/bin/restreamair
COPY public /app/public
COPY README.md SCRIPTING.md API.md /app/
# The panel serves ./public relative to the working directory; symlink rather
# than copy into /data so a mounted volume doesn't shadow the UI.
RUN ln -s /app/public /data/public && chown -R restreamair:restreamair /data /app
USER restreamair

EXPOSE 8787
VOLUME ["/data"]

# /ping is unauthenticated precisely so a probe can use it without a credential.
HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
    CMD curl -fsS "http://127.0.0.1:${PORT:-8787}/ping" || exit 1

ENTRYPOINT ["/usr/local/bin/restreamair"]
CMD ["serve", "--bind", "0.0.0.0"]
