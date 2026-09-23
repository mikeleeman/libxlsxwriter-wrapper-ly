# native/

Source of truth for the binary this package installs. `xlsx_writer.c` is
built statically against libxlsxwriter (vendored as a pinned git submodule
under `native/libxlsxwriter/`, see `.gitmodules`) and static zlib, producing
a single self-contained executable with zero runtime dependencies.

Binaries are NOT committed here — they're built by CI on tag push and
attached to the matching GitHub Release (see `.github/workflows/release-binaries.yml`).
`src/Installer.php` downloads the right one for the host platform during
`composer install`.

## Building locally

```bash
git submodule update --init native/libxlsxwriter
apt-get install -y build-essential zlib1g-dev
make -C native/libxlsxwriter -j"$(nproc)"
gcc -O2 -static \
  -I native/libxlsxwriter/include \
  -o xlsx_writer-linux-x86_64 \
  native/xlsx_writer.c \
  native/libxlsxwriter/lib/libxlsxwriter.a \
  /usr/lib/x86_64-linux-gnu/libz.a
strip xlsx_writer-linux-x86_64
```

Verify it's actually static (no runtime deps to worry about on the target host):

```bash
ldd xlsx_writer-linux-x86_64   # expect: "not a dynamic executable"
```
