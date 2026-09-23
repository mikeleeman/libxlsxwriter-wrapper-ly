<?php

namespace Ly\XlsxWriterWrapper;

use Composer\Script\Event;

/**
 * Downloads the xlsx_writer binary matching the host platform from this
 * package's GitHub Releases, verifies its checksum, and installs it to
 * bin/xlsx_writer (the path composer.json's "bin" entry points at).
 *
 * Runs automatically on `composer install` / `composer update` (wired
 * via post-install-cmd / post-update-cmd). Can also be run manually:
 *
 *   composer xlsx-writer:install
 *
 * which is the thing to reach for if the automatic run failed (e.g. a
 * network hiccup during someone else's `composer install`), or after
 * moving to a different machine/architecture.
 */
class Installer
{
    private const REPO = 'mikeleeman/libxlsxwriter-wrapper-ly';

    public static function install(Event $event): void
    {
        $io = $event->getIO();

        // NOT $event->getComposer()->getPackage()->getExtra() — that
        // returns the ROOT project's package (the consuming app), not
        // this library's own package, so it was silently falling
        // through to the 'v1.0.0' default below no matter what
        // xlsx-writer-binary-version was actually set to here. Reading
        // our own composer.json straight off disk instead: dirname(__DIR__)
        // from src/Installer.php is this package's own root (where
        // Composer always places a copy of composer.json for the
        // installed package), so there's no ambiguity about which
        // package's metadata this is.
        $ownComposerJson = json_decode(file_get_contents(dirname(__DIR__) . '/composer.json'), true) ?? [];
        $extra = $ownComposerJson['extra'] ?? [];
        $version = getenv('XLSX_WRITER_BINARY_VERSION')
            ?: ($extra['xlsx-writer-binary-version'] ?? 'v1.0.0');

        $platform = self::detectPlatform();
        if ($platform === null) {
            $io->writeError(
                '<warning>xlsx-writer: no prebuilt binary for ' . PHP_OS_FAMILY . '/' . php_uname('m') . '. ' .
                'Build xlsx_writer.c yourself against libxlsxwriter (see README) and either place it at ' .
                'bin/xlsx_writer in this package, or point your app at it directly via XLSX_WRITER_BINARY_PATH.</warning>'
            );
            return;
        }

        $binDir = dirname(__DIR__) . '/bin';
        if (!is_dir($binDir) && !mkdir($binDir, 0755, true) && !is_dir($binDir)) {
            $io->writeError("<error>xlsx-writer: could not create {$binDir}</error>");
            exit(1);
        }
        // NOT bin/xlsx_writer — that name is the committed wrapper
        // script (see bin/xlsx_writer's own docblock for why). Writing
        // the downloaded native binary to a different filename means we
        // never overwrite the wrapper, and Composer's "bin" linking step
        // (which runs BEFORE this plugin event fires, and only succeeds
        // if the file already exists) always finds something at
        // bin/xlsx_writer regardless of whether this download has run yet.
        $binPath = $binDir . '/xlsx_writer-native';

        $assetName = "xlsx_writer-{$platform}";
        $baseUrl = 'https://github.com/' . self::REPO . "/releases/download/{$version}/";

        $io->write("<info>xlsx-writer: fetching {$assetName} ({$version})</info>");

        $binary = self::fetch($io, $baseUrl . $assetName);
        $checksumFile = self::fetch($io, $baseUrl . $assetName . '.sha256');

        // sha256sum-format files read "<hash>  <filename>"; also accept a bare hash.
        $expected = strtok(trim($checksumFile), " \t");
        $actual = hash('sha256', $binary);

        if (!is_string($expected) || !hash_equals(strtolower($expected), strtolower($actual))) {
            $io->writeError(
                "<error>xlsx-writer: checksum mismatch for {$assetName}\n" .
                "  expected: {$expected}\n" .
                "  actual:   {$actual}\n" .
                "Refusing to install a binary that doesn't match its published checksum.</error>"
            );
            exit(1);
        }

        file_put_contents($binPath, $binary);
        chmod($binPath, 0755);

        $io->write("<info>xlsx-writer: installed to {$binPath}</info>");
    }

    private static function detectPlatform(): ?string
    {
        // Written as if/elseif rather than match() — match() is PHP 8.0+
        // only, and Composer itself may run under an older PHP than the
        // app it's installing into (this caused a ParseError on a
        // staging box where the system `php` used to run Composer was
        // 7.4). Composer plugins need to tolerate whatever PHP Composer
        // itself happens to be running under.
        $os = PHP_OS_FAMILY;          // 'Linux', 'Darwin', 'Windows', ...
        $arch = php_uname('m');       // 'x86_64', 'aarch64', 'arm64', ...

        if ($os === 'Linux' && $arch === 'x86_64') {
            return 'linux-x86_64';
        }
        if ($os === 'Linux' && in_array($arch, ['aarch64', 'arm64'], true)) {
            return 'linux-arm64';
        }
        if ($os === 'Darwin' && $arch === 'x86_64') {
            return 'darwin-x86_64';
        }
        if ($os === 'Darwin' && $arch === 'arm64') {
            return 'darwin-arm64';
        }
        return null;
    }

    private static function fetch($io, string $url): string
    {
        if (function_exists('curl_init')) {
            $ch = curl_init($url);
            curl_setopt_array($ch, [
                CURLOPT_RETURNTRANSFER => true,
                CURLOPT_FOLLOWLOCATION => true,
                CURLOPT_TIMEOUT => 60,
                CURLOPT_USERAGENT => 'libxlsxwriter-wrapper-ly-installer',
            ]);
            $data = curl_exec($ch);
            $code = curl_getinfo($ch, CURLINFO_HTTP_CODE);
            $err = curl_error($ch);
            curl_close($ch);

            if ($data === false || $code >= 400) {
                $io->writeError("<error>xlsx-writer: failed to download {$url} (HTTP {$code}) {$err}</error>");
                exit(1);
            }
            return $data;
        }

        // Fallback if curl isn't available — requires allow_url_fopen.
        $context = stream_context_create(['http' => ['timeout' => 60, 'follow_location' => 1]]);
        $data = @file_get_contents($url, false, $context);
        if ($data === false) {
            $io->writeError("<error>xlsx-writer: failed to download {$url} (curl extension unavailable and allow_url_fopen failed)</error>");
            exit(1);
        }
        return $data;
    }
}
