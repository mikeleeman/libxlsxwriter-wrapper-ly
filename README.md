# mikeleeman/libxlsxwriter-wrapper-ly

A fully static `xlsx_writer` binary (libxlsxwriter + zlib + glibc all linked
in — zero runtime dependencies) plus a Composer plugin that downloads the
right prebuilt binary for the host platform automatically.

## Install

```bash
composer require mikeleeman/libxlsxwriter-wrapper-ly
```

This package is a **Composer plugin** (not a plain script hook), which is
what lets the binary download run automatically when you `require` it —
plain `"scripts"` entries in a dependency's own `composer.json` are
ignored by Composer unless that dependency is the root project, so this
had to be a real plugin. Composer 2.2+ will ask you to confirm you trust
it the first time:

```
mikeleeman/libxlsxwriter-wrapper-ly contains a Composer plugin which is
currently not in your allow-plugins config. See https://getcomposer.org/allow-plugins
Do you trust "mikeleeman/libxlsxwriter-wrapper-ly" to execute code and
run it? (writes "allow-plugins" to composer.json) [y,n,d,?]
```

Answer `y` interactively, or — for CI / non-interactive installs — add
this to your **own project's** `composer.json` up front:

```json
{
    "config": {
        "allow-plugins": {
            "mikeleeman/libxlsxwriter-wrapper-ly": true
        }
    }
}
```

(or run `composer config allow-plugins.mikeleeman/libxlsxwriter-wrapper-ly true`).

Once trusted, `composer install`/`composer update` will detect your
OS/arch, download `xlsx_writer-<platform>` from this repo's GitHub
Releases matching `extra.xlsx-writer-binary-version` in this package's
`composer.json`, verify its SHA-256 against the published `.sha256`
file, and install it to `vendor/mikeleeman/libxlsxwriter-wrapper-ly/bin/xlsx_writer`
— which Composer's `"bin"` entry also exposes at `vendor/bin/xlsx_writer`.

If the download fails partway (network hiccup) or you change machines,
just re-run `composer update mikeleeman/libxlsxwriter-wrapper-ly` to
retry it.

Supported platforms today: `linux-x86_64`. `linux-arm64`,
`darwin-x86_64`, and `darwin-arm64` are recognized by the installer and
will warn cleanly (not crash) rather than fail silently if no matching
release asset exists yet — see `.github/workflows/release-binaries.yml`
to add those builds.

## Using it (e.g. from Laravel)

Point your binary runner at the installed path — typically:

```php
base_path('vendor/bin/xlsx_writer')
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
   installs to pick up a new binary (it's decoupled from this package's
   own Composer/Packagist version on purpose — a pure-PHP fix to
   `Installer.php`/`Plugin.php` doesn't force a binary rebuild, and a
   binary-only fix doesn't force a Packagist release).
2. `git tag vX.Y.Z && git push --tags` — this is the *binary* release tag,
   matched by `on: push: tags: "v*"` in the workflow and by whatever value
   `extra.xlsx-writer-binary-version` points at.
3. CI builds, smoke-tests, checksums, and attaches the binaries to the
   GitHub Release for that tag automatically.
4. Tag/push a matching commit if you also want a new Packagist version —
   Packagist reads the package version from git tags, independent of the
   binary-version tags above (they can be the same tag or different ones;
   just keep `extra.xlsx-writer-binary-version` pointed at whichever
   release actually has binaries attached).
