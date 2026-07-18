#!/bin/sh
# Package a self-contained Linux release: the binary, plus the versioned shared
# libraries whose sonames drift across distros (libxml2 / ICU / lzma), plus a
# launcher that points LD_LIBRARY_PATH at them. glibc, libcurl and OpenSSL are
# standard and taken from the host, so they are NOT bundled.
#
# Why this exists: the binary is glibc-dynamic and links libxml2.so.2 +
# libicu*.so.NN. Newer Ubuntu (e.g. 26.04) ships libxml2.so.16 and a newer ICU,
# so a bare download dies with "libxml2.so.2: cannot open shared object file".
# Bundling the exact sonames the binary was built against fixes that.
#
# Run INSIDE the build container (where the libs exist and match the binary).
# Arg 1: stage directory name. Arg 2 (optional): binary name (default restreamair-linux).
set -e
STAGE="$1"
BIN="${2:-restreamair-linux}"
mkdir -p "$STAGE/libs"
# Normalise the staged binary name so the launcher (which execs
# ./restreamair-linux) works for every arch — the tarball name carries the arch.
cp "$BIN" "$STAGE/restreamair-linux"

# Direct deps of the binary.
for l in $(ldd "$BIN" | grep -oE '/[^ ]+' | grep -E 'libxml2|libicu|liblzma'); do
  cp -L "$l" "$STAGE/libs/"
done

# Transitive deps of the bundled libs (e.g. libicuuc -> libicudata), looped
# until the set stops growing.
changed=1
while [ "$changed" = 1 ]; do
  changed=0
  for lib in "$STAGE"/libs/*.so*; do
    for l in $(ldd "$lib" 2>/dev/null | grep -oE '/[^ ]+' | grep -E 'libxml2|libicu|liblzma'); do
      base=$(basename "$l")
      if [ ! -e "$STAGE/libs/$base" ]; then cp -L "$l" "$STAGE/libs/"; changed=1; fi
    done
  done
done

cat > "$STAGE/restreamair" <<'EOF'
#!/bin/sh
# Launcher: resolves the bundled libs (libxml2 / ICU / lzma, whose sonames
# differ across Ubuntu/Debian releases) so the binary runs on any reasonably
# recent glibc Linux without installing anything. Run THIS, not restreamair-linux.
DIR="$(cd "$(dirname "$0")" && pwd)"
export LD_LIBRARY_PATH="$DIR/libs:$LD_LIBRARY_PATH"
exec "$DIR/restreamair-linux" "$@"
EOF
chmod +x "$STAGE/restreamair"

echo "bundled $(ls "$STAGE/libs" | wc -l) libs into $STAGE:"
ls -1 "$STAGE/libs"
