# mikeleeman/libxlsxwriter-wrapper-ly

A fully static `xlsx_writer` binary (libxlsxwriter + zlib linked in, zero
runtime dependencies — not even glibc) plus a Composer install script that
fetches the right prebuilt binary for the host platform from this repo's
GitHub Releases.

## Install

```bash
composer require mikeleeman/libxlsxwriter-wrapper-ly
```

On `composer install`/`update`, `src/Installer.php` runs automatically,
detects the host OS/arch, downloads `xlsx_writer-<platform>` from the
GitHub Release matching `extra.xlsx-writer-binary-version` in
`composer.json` (currently `v1.0.0`), verifies its SHA-256 against the
published `.sha256` file, and installs it to `bin/xlsx_writer`. Composer's
`"bin"` entry then makes it available at `vendor/bin/xlsx_writer`.

If the automatic download fails (flaky network during someone else's
`composer install`) or you switch machines/architectures, re-run it
manually:

```bash
composer xlsx-writer:install
```

Supported platforms today: `linux-x86_64`. `linux-arm64`, `darwin-x86_64`,
and `darwin-arm64` are recognized by the installer but need a CI job added
(see `.github/workflows/release-binaries.yml`) before a release actually
ships those assets — until then, the installer will warn and tell you to
build from source instead of failing silently.

## Using it (e.g. from Laravel)

Point your binary runner at the installed path:

```php
new XlsxBinaryRunner(
    binaryPath: base_path('vendor/bin/xlsx_writer'),
);
```

## Verifying a binary yourself

Every release asset has a matching `.sha256` file:

```bash
curl -LO https://github.com/mikeleeman/libxlsxwriter-wrapper-ly/releases/download/v1.0.0/xlsx_writer-linux-x86_64
curl -LO https://github.com/mikeleeman/libxlsxwriter-wrapper-ly/releases/download/v1.0.0/xlsx_writer-linux-x86_64.sha256
sha256sum -c xlsx_writer-linux-x86_64.sha256
```

## Building from source

See `native/README.md`. The source (`native/xlsx_writer.c`) and the exact
libxlsxwriter version it's built against (`native/libxlsxwriter`, a pinned
git submodule) are committed to this repo — only the compiled binaries are
distributed via GitHub Releases rather than git, since they're
platform-specific build artifacts, not source.

## Releasing a new version

1. Bump `extra.xlsx-writer-binary-version` in `composer.json` if you want
   installs to pick up a new binary (it doesn't have to match the
   package's own Composer version — they're decoupled on purpose, so a
   pure-PHP fix to `Installer.php` doesn't force a binary rebuild, and
   vice versa).
2. `git tag vX.Y.Z && git push --tags` — this is the *binary* release tag,
   matched by `on: push: tags: "v*"` in the workflow and by whatever value
   `extra.xlsx-writer-binary-version` points at.
3. CI builds, smoke-tests, checksums, and attaches the binaries to the
   GitHub Release for that tag automatically.
4. Tag/push a matching Composer version if you also want a Packagist
   release (`composer.json` doesn't declare its own `"version"` — Packagist
   reads it from the git tag at publish time).
