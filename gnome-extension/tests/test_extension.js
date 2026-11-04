// Runs extension.js against the fake shell in stubs/ (see run-tests.sh). GLib is real.

import System from 'system';

import {CLIPBOARD, PRIMARY, makeWindow, resetState, state} from './stubs/state.js';
import GLib from 'gi://GLib';
import Gio from './stubs/Gio.js';

import ClipItToExtension, {hasPlainText, hasText, imageType, isPopupMain, isPopupWindow, isSensitive, popupPlacement, sourceAppName} from './extension.js';

const NO_AUTO_START = 1;
let passed = 0;
let failed = 0;

function test(name, fn) {
    resetState();
    try {
        fn();
        passed++;
        print(`PASS ${name}`);
    } catch (e) {
        failed++;
        printerr(`FAIL ${name}: ${e.message}\n${e.stack}`);
    }
}

function assertEq(actual, expected, what = '') {
    const a = JSON.stringify(actual);
    const e = JSON.stringify(expected);
    if (a !== e)
        throw new Error(`${what} expected ${e}, got ${a}`);
}

function assert(condition, what = 'assertion failed') {
    if (!condition)
        throw new Error(what);
}

function enabled() {
    const ext = new ClipItToExtension({uuid: 'clipitto@nimiz.github.io'});
    ext.enable();
    return ext;
}

const TEXT = ['text/plain;charset=utf-8', 'text/plain', 'UTF8_STRING'];
const popup = () => makeWindow({wmClass: 'io.github.nimiz.ClipItTo'});

test('MIME helpers', () => {
    assert(hasText(['text/html']));
    assert(hasText(['UTF8_STRING']));
    assert(!hasText(['image/png']));
    assert(hasPlainText(['UTF8_STRING']) && hasPlainText(['text/plain;charset=utf-8']));
    assert(!hasPlainText(['text/html', 'image/png']));
    assertEq(imageType(['text/html', 'image/jpeg', 'image/png']), 'image/png');
    assertEq(imageType(['image/webp', 'image/svg+xml']), null); // only formats the daemon decodes
    assertEq(imageType(TEXT), null);
    assert(isSensitive(['text/plain', 'x-kde-passwordManagerHint']));
    assert(!isSensitive(TEXT));
});

test('popup window matching', () => {
    assert(isPopupWindow(popup()), 'Wayland app_id');
    assert(isPopupWindow(makeWindow({instance: 'clipitto-daemon', wmClass: 'Clipitto'})), 'X11 WM_CLASS');
    assert(isPopupWindow(makeWindow({app: {get_id: () => 'io.github.nimiz.ClipItTo.desktop'}})), 'tracked app');
    assert(!isPopupWindow(makeWindow({wmClass: 'org.gnome.Nautilus'})));
});

test('clipboard text is sent to the daemon without auto-starting it', () => {
    const ext = enabled();
    state.selection.setOwner(CLIPBOARD, TEXT, 'hello');
    assertEq(state.dbusCalls.length, 1, 'calls');
    const call = state.dbusCalls[0];
    assertEq([call.name, call.path, call.method],
        ['io.github.nimiz.ClipItTo', '/io/github/nimiz/ClipItTo', 'Ingest']);
    assertEq(call.params.deepUnpack(), ['hello', TEXT], 'params');
    assertEq(call.flags, NO_AUTO_START, 'flags');
    ext.disable();
});

test('a copy with a formatted version is sent as IngestRich', () => {
    const ext = enabled();
    const types = [...TEXT, 'text/html'];
    state.html[CLIPBOARD] = new TextEncoder().encode('<b>bold</b> word');
    state.selection.setOwner(CLIPBOARD, types, 'bold word');
    assertEq(state.dbusCalls.length, 1, 'calls');
    assertEq(state.dbusCalls[0].method, 'IngestRich');
    assertEq(state.dbusCalls[0].params.deepUnpack(), ['bold word', '<b>bold</b> word', types]);
    assertEq(state.dbusCalls[0].flags, NO_AUTO_START, 'flags');

    // An older daemon has no IngestRich: the plain text is sent instead.
    state.dbusCalls[0].callback({
        call_finish: () => {
            const e = new Error('No such method');
            e.matches = (_domain, code) => code === Gio.DBusError.UNKNOWN_METHOD;
            throw e;
        },
    }, null);
    assertEq(state.dbusCalls[1].method, 'Ingest');
    assertEq(state.dbusCalls[1].params.deepUnpack(), ['bold word', types]);

    // Offered but unreadable: plain. Not offered: plain, and nothing is read. Primary: plain.
    state.html = {};
    state.selection.setOwner(CLIPBOARD, types, 'no html');
    assertEq(state.dbusCalls[2].method, 'Ingest');
    state.contentReads = [];
    state.selection.setOwner(CLIPBOARD, TEXT, 'plain');
    assertEq(state.contentReads, [], 'content reads');
    assertEq(state.dbusCalls[3].method, 'Ingest');
    ext.disable();
});

// Answers a recorded D-Bus call as the daemon would (or fails it, with `reply` null).
function answer(call, reply) {
    call.callback({
        call_finish: () => {
            if (reply === null)
                throw new Error('No such method');
            return reply;
        },
    }, null);
}

test('secrets are read only when the daemon keeps passwords', () => {
    const ext = enabled();
    const marked = [...TEXT, 'x-kde-passwordManagerHint'];
    state.display.focus_window = makeWindow({app: {get_name: () => 'KeePassXC'}});

    // Asked first; the secret isn't read before the answer, nor after a "no".
    state.selection.setOwner(CLIPBOARD, marked, 'hunter2');
    assertEq(state.textReads, 0, 'text reads before the answer');
    assertEq(state.dbusCalls.map(c => c.method), ['WantsPasswords']);
    assertEq(state.dbusCalls[0].flags, NO_AUTO_START, 'flags');
    answer(state.dbusCalls[0], new GLib.Variant('(b)', [false]));
    assertEq(state.textReads, 0, 'text reads after no');

    // An older daemon (no such method) counts as "no".
    state.selection.setOwner(CLIPBOARD, marked, 'hunter2');
    answer(state.dbusCalls[1], null);
    assertEq(state.textReads, 0, 'text reads after an error');

    // "Yes": sent with its markers and the app it came from.
    state.selection.setOwner(CLIPBOARD, marked, 'hunter2');
    answer(state.dbusCalls[2], new GLib.Variant('(b)', [true]));
    assertEq(state.textReads, 1, 'text reads after yes');
    const ingest = state.dbusCalls[3];
    assertEq(ingest.method, 'Ingest');
    assertEq(ingest.params.deepUnpack(), ['hunter2', [...marked, 'x-clipitto-source:KeePassXC']]);

    // A source the copying app offers itself is dropped.
    state.selection.setOwner(CLIPBOARD, [...marked, 'x-clipitto-source:Bank'], 'hunter2');
    answer(state.dbusCalls[4], new GLib.Variant('(b)', [true]));
    assertEq(state.dbusCalls[5].params.deepUnpack()[1], [...marked, 'x-clipitto-source:KeePassXC']);

    // Marked copies from the primary selection are never sent.
    state.settings.values['capture-primary'] = true;
    state.selection.setOwner(PRIMARY, marked, 'hunter2');
    assertEq(state.dbusCalls.length, 6, 'calls at the end');
    ext.disable();
});

test('sourceAppName', () => {
    assertEq(sourceAppName(null), '');
    assertEq(sourceAppName(makeWindow({app: {get_name: () => ' Bitwarden '}})), 'Bitwarden');
    assertEq(sourceAppName(makeWindow({app: null, wmClass: 'firefox'})), 'firefox');
});

test('a copied picture is sent as IngestImage', () => {
    const ext = enabled();
    const png = new Uint8Array([0x89, 0x50, 0x4e, 0x47, 1, 2, 3]);
    const types = ['text/html', 'image/png'];
    state.selection.setOwner(CLIPBOARD, types, png); // a browser's "Copy Image"
    assertEq(state.textReads, 0, 'not read as text');
    assertEq(state.contentReads, ['image/png']);
    assertEq(state.dbusCalls.length, 1, 'calls');
    const call = state.dbusCalls[0];
    assertEq(call.method, 'IngestImage');
    const [data, mimeType, mimeTypes] = call.params.deepUnpack();
    assertEq([Array.from(data), mimeType, mimeTypes], [Array.from(png), 'image/png', types]);
    assertEq(call.flags, NO_AUTO_START, 'flags');

    // Plain text wins when a copy offers both (e.g. cells from a spreadsheet).
    state.selection.setOwner(CLIPBOARD, [...TEXT, 'image/png'], 'cells');
    assertEq(state.dbusCalls[1].method, 'Ingest');
    // A marked secret is never read, picture or not.
    state.selection.setOwner(CLIPBOARD, ['image/png', 'x-kde-passwordManagerHint'], png);
    assertEq(state.contentReads.length, 1, 'content reads');
    // No pictures from the primary selection.
    state.settings.values['capture-primary'] = true;
    state.selection.setOwner(PRIMARY, ['image/png'], png);
    assertEq(state.dbusCalls.length, 2, 'calls at the end');
    ext.disable();
});

test('SetClipboardContent puts a picture on the clipboard', () => {
    const ext = enabled();
    state.service.SetClipboardContent(new Uint8Array([1, 2, 3]), 'image/png');
    assertEq(state.clipboardSet, [{type: CLIPBOARD, mimeType: 'image/png', data: [1, 2, 3]}]);
    state.service.SetClipboardContent(new Uint8Array([1]), 'text/html'); // only PNG pictures
    assertEq(state.clipboardSet.length, 1);
    assert(state.serviceXml.includes('<method name="SetClipboardContent">'));
    ext.disable();
});

test('non-text, empty and oversized selections are skipped', () => {
    const ext = enabled();
    state.selection.setOwner(CLIPBOARD, ['application/octet-stream'], null);
    assertEq(state.textReads, 0, 'binary read');
    state.selection.setOwner(CLIPBOARD, TEXT, '');
    state.selection.setOwner(CLIPBOARD, TEXT, 'x'.repeat(1024 * 1024 + 1));
    assertEq(state.dbusCalls.length, 0, 'calls');
    ext.disable();
});

test('primary selection only when enabled', () => {
    const ext = enabled();
    state.selection.setOwner(PRIMARY, TEXT, 'selected');
    assertEq(state.dbusCalls.length, 0, 'disabled');
    state.settings.values['capture-primary'] = true;
    state.selection.setOwner(PRIMARY, TEXT, 'selected');
    assertEq(state.dbusCalls.length, 1, 'enabled');
    ext.disable();
});

test('enable exports the service and owns the name; disable undoes everything', () => {
    const ext = enabled();
    assertEq(state.exported, ['/io/github/nimiz/ClipItTo/Shell']);
    assertEq([...state.ownedNames.values()], ['io.github.nimiz.ClipItTo.Shell']);
    assert(state.serviceXml.includes('<method name="ActivateWindow"/>'));
    ext.disable();
    assertEq(state.exported, [], 'exported after disable');
    assertEq(state.ownedNames.size, 0, 'names after disable');
    assertEq(state.selection.handlerCount(), 0, 'selection handlers');
    assertEq(state.settings.handlerCount(), 0, 'settings handlers');
    state.selection.setOwner(CLIPBOARD, TEXT, 'after disable');
    assertEq(state.dbusCalls.length, 0, 'no capture after disable');
});

test('ActivateWindow centers an existing popup on the pointer monitor and focuses it', () => {
    const ext = enabled();
    const win = popup();
    state.windows = [makeWindow({wmClass: 'org.gnome.Terminal'}), win];
    state.service.ActivateWindow();
    assertEq(state.activated.length, 1);
    assert(state.activated[0] === win, 'activated the popup');
    // Monitor 1 work area: x 1920, y 32, 1920x1048; popup 560x480.
    assertEq([state.moved[0].x, state.moved[0].y], [1920 + 680, 32 + 189]);
    ext.disable();
});

test('ActivateWindow waits for a popup that is not mapped yet', () => {
    const ext = enabled();
    state.service.ActivateWindow();
    assertEq(state.activated.length, 0, 'nothing yet');

    state.display.emit('window-created', makeWindow({wmClass: 'org.gnome.Terminal'}));
    const win = popup();
    state.display.emit('window-created', win);
    assertEq(state.activated.length, 0, 'not before it is shown');
    win.emit('shown');
    assert(state.activated[0] === win, 'activated once shown');
    assertEq(state.display.handlerCount(), 1, 'stopped waiting (only the dialog watcher is left)');
    assertEq(win.handlerCount(), 1, 'shown handler removed (position-changed stays, to remember "last")');
    ext.disable();
});

test('disable cancels a pending activation', () => {
    const ext = enabled();
    state.service.ActivateWindow();
    assertEq(state.display.handlerCount(), 2); // the dialog watcher and the pending activation
    ext.disable();
    assertEq(state.display.handlerCount(), 0);
});

test('SetClipboard sets the clipboard selection', () => {
    const ext = enabled();
    state.service.SetClipboard('from history');
    assertEq(state.clipboardSet, [{type: CLIPBOARD, text: 'from history'}]);
    ext.disable();
});

test('Paste types the requested shortcut', () => {
    const ext = enabled();
    state.service.Paste('ctrl+v');
    assertEq(state.keyEvents, [[0xffe3, 1], [0x76, 1], [0x76, 0], [0xffe3, 0]], 'ctrl+v');
    state.keyEvents.length = 0;
    state.service.Paste('shift+insert');
    assertEq(state.keyEvents, [[0xffe1, 1], [0xff63, 1], [0xff63, 0], [0xffe1, 0]], 'shift+insert');
    ext.disable();
});

test('toggle shortcut follows the setting and auto-starts the daemon', () => {
    const ext = enabled();
    assertEq(state.keybindings.size, 0, 'none by default');
    state.settings.set('toggle-shortcut', ['<Control><Alt>v']);
    assert(state.keybindings.has('toggle-shortcut'), 'added');
    state.keybindings.get('toggle-shortcut')();
    const call = state.dbusCalls[0];
    assertEq([call.method, call.params.deepUnpack(), call.flags], ['Toggle', [''], 0]);
    state.settings.set('toggle-shortcut', []);
    assertEq(state.keybindings.size, 0, 'removed when cleared');
    state.settings.set('toggle-shortcut', ['<Super>v']);
    ext.disable();
    assertEq(state.keybindings.size, 0, 'removed on disable');
});

const AREA = {x: 1920, y: 32, width: 1920, height: 1048};
const SIZE = {width: 560, height: 480};

test('popupPlacement mirrors the X11 positions', () => {
    const at = (position, pointer = {x: 2920, y: 300}, last = null) => {
        const p = popupPlacement(position, AREA, SIZE, pointer, last);
        return [p.x, p.y];
    };
    // 5% margins: 96 x 52 (floor of 52.4).
    assertEq(at('top-left'), [1920 + 96, 32 + 52]);
    assertEq(at('top-right'), [1920 + 1920 - 96 - 560, 32 + 52]);
    assertEq(at('bottom-left'), [1920 + 96, 32 + 1048 - 52 - 480]);
    assertEq(at('bottom-right'), [1920 + 1920 - 96 - 560, 32 + 1048 - 52 - 480]);
    assertEq(at('center'), [1920 + 680, 32 + 189]);
    // Near the pointer: centered on it horizontally, 24 px above it; kept on the screen.
    assertEq(at('cursor'), [2920 - 280, 276]);
    assertEq(at('cursor', {x: 1925, y: 40}), [1920, 32]);
    assertEq(at('cursor', {x: 3830, y: 1070}), [1920 + 1920 - 560, 32 + 1048 - 480]);
    // "last" with a place, without one (the pointer), and an unknown value (the pointer).
    assertEq(at('last', {x: 0, y: 0}, {x: 2000, y: 100}), [2000, 100]);
    assertEq(at('last'), at('cursor'));
    assertEq(at('nonsense'), at('cursor'));
});

test('ActivateWindowAt places the popup where the setting says', () => {
    const ext = enabled();
    assert(state.serviceXml.includes('<method name="ActivateWindowAt">'));
    const win = popup();
    state.windows = [win];
    state.service.ActivateWindowAt('top-left');
    assertEq([state.moved[0].x, state.moved[0].y], [1920 + 96, 32 + 52]);
    state.service.ActivateWindowAt('cursor');
    assertEq([state.moved[1].x, state.moved[1].y], [2920 - 280, 276]);
    assertEq(state.activated.length, 2);
    ext.disable();
});

test('"last" returns to where the popup was dragged to', () => {
    const ext = enabled();
    const first = popup();
    state.windows = [first];
    state.service.ActivateWindowAt('last'); // nothing remembered: near the pointer
    assertEq([state.moved[0].x, state.moved[0].y], [2920 - 280, 276]);
    first.frame = {x: 2400, y: 500, width: 560, height: 480}; // dragged by its name
    first.emit('position-changed');
    const second = popup(); // the next time it opens it is a new window
    state.windows = [second];
    state.service.ActivateWindowAt('last');
    assertEq([state.moved[1].x, state.moved[1].y], [2400, 500]);
    ext.disable();
});

test('the popup is found among ClipItTo windows, not a dialog of its own', () => {
    const main = popup();
    const dialog = makeWindow({wmClass: 'io.github.nimiz.ClipItTo', transientFor: main});
    assert(isPopupMain(main));
    assert(!isPopupMain(dialog), 'a dialog has the popup as its parent');
    const ext = enabled();
    state.windows = [dialog, main];
    state.service.ActivateWindowAt('center');
    assert(state.activated[0] === main, 'the popup, not the dialog');
    ext.disable();
});

test('Settings opens centered over the popup, on its monitor', () => {
    const ext = enabled();
    const main = makeWindow({wmClass: 'io.github.nimiz.ClipItTo', frame: {x: 2000, y: 100, width: 560, height: 480}});
    const dialog = makeWindow({wmClass: 'io.github.nimiz.ClipItTo', transientFor: main,
        frame: {x: 0, y: 0, width: 700, height: 600}});
    state.display.emit('window-created', dialog);
    assertEq(state.moved.length, 0, 'not before it is shown');
    dialog.emit('shown');
    // centered on the popup: x 2000 + (560-700)/2, y 100 + (480-600)/2
    assertEq([state.moved[0].x, state.moved[0].y], [1930, 40]);
    assertEq(dialog.handlerCount(), 0, 'shown handler removed');

    // Kept inside the monitor's work area (x from 1920, y from 32).
    const edge = makeWindow({wmClass: 'io.github.nimiz.ClipItTo', frame: {x: 1920, y: 32, width: 300, height: 200}});
    const wide = makeWindow({wmClass: 'io.github.nimiz.ClipItTo', transientFor: edge,
        frame: {x: 0, y: 0, width: 700, height: 600}});
    state.display.emit('window-created', wide);
    wide.emit('shown');
    assertEq([state.moved[1].x, state.moved[1].y], [1920, 32]);

    // A window that is not ClipItTo's, or has no parent, is left alone.
    const other = makeWindow({wmClass: 'org.gnome.Terminal', transientFor: main});
    state.display.emit('window-created', other);
    assertEq(other.handlerCount(), 0);
    const toplevel = popup();
    state.display.emit('window-created', toplevel);
    assertEq(toplevel.handlerCount(), 0);
    ext.disable();
});

print(`\n${passed} passed, ${failed} failed`);
if (failed > 0)
    System.exit(1);
