<?php

namespace Ly\XlsxWriterWrapper;

use Composer\Composer;
use Composer\EventDispatcher\EventSubscriberInterface;
use Composer\IO\IOInterface;
use Composer\Plugin\PluginInterface;
use Composer\Script\Event;
use Composer\Script\ScriptEvents;

/**
 * A plain "scripts" entry in composer.json only auto-runs when THIS
 * package is the root project being built — it is silently ignored by
 * Composer when the package is required as a dependency elsewhere.
 * Making this a real Composer plugin (implementing PluginInterface +
 * EventSubscriberInterface) is what makes the binary download actually
 * fire during `composer require`/`composer update` in a consuming app.
 *
 * Consequence: consuming projects must explicitly trust this plugin —
 * Composer 2.2+ will either prompt interactively ("do you trust...?")
 * or, non-interactively (CI), require
 *   "config": { "allow-plugins": { "mikeleeman/libxlsxwriter-wrapper-ly": true } }
 * in THEIR OWN composer.json. See the top-level README for the exact
 * steps — this is expected Composer behavior for any plugin, not a bug.
 */
class Plugin implements PluginInterface, EventSubscriberInterface
{
    public function activate(Composer $composer, IOInterface $io): void
    {
        // Nothing to do at activation time; the real work happens in
        // the subscribed post-install/post-update events below.
    }

    public function deactivate(Composer $composer, IOInterface $io): void
    {
    }

    public function uninstall(Composer $composer, IOInterface $io): void
    {
        $bin = dirname(__DIR__) . '/bin/xlsx_writer';
        if (is_file($bin)) {
            @unlink($bin);
        }
    }

    public static function getSubscribedEvents(): array
    {
        return [
            ScriptEvents::POST_INSTALL_CMD => 'onInstallOrUpdate',
            ScriptEvents::POST_UPDATE_CMD => 'onInstallOrUpdate',
        ];
    }

    public static function onInstallOrUpdate(Event $event): void
    {
        Installer::install($event);
    }
}
