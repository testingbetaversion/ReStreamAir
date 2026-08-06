# ReStreamAir — the Swift panel, which is the fully-featured binary on every
# platform. Multi-stage: build with the Swift toolchain, ship on a slim runtime
# that carries only the two shared libraries Foundation actually needs.
#
#   docker build -t restreamair .
#   docker run -d --name restreamair -p 8787:8787 -v restreamair-data:/data restreamair
#
# Everything mutable (state.json, runtime/, logs/, logo-cache.json) is resolved
# relative to the working directory, so /data is the only volume that matters.
# Back that up and you have backed up the install.

FROM swift:6.0 AS build
WORKDIR /src

# Copy the manifest and sources. `swift build` — not a bare swiftc — because the
# Swift sources import the C core, which only SwiftPM compiles and links.
COPY Package.swift ./
COPY core ./core
COPY *.swift ./
RUN swift build -c release --product restreamair

# Prove the crypto and the C-vs-Swift parity check pass in the same environment
# that produced the binary, rather than trusting that they did somewhere else.
RUN .build/release/restreamair selftest

FROM swift:6.0-slim
# libcurl4 and libxml2 are what Foundation pulls in at runtime; ca-certificates
# is what lets the panel fetch an https:// manifest at all.
RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates curl libcurl4 libxml2 \
    && rm -rf /var/lib/apt/lists/*

# An unprivileged user: nothing here needs root, and the panel binds 8787 rather
# than a privileged port.
RUN useradd --system --create-home --uid 10001 restreamair
WORKDIR /data
COPY --from=build /src/.build/release/restreamair /usr/local/bin/restreamair
COPY public /app/public
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
