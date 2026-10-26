// Runs extension.js against the fake shell in stubs/ (see run-tests.sh). GLib is real.

import System from 'system';

import {CLIPBOARD, PRIMARY, makeWindow, resetState, state} from './stubs/state.js';
import GLib from 'gi://GLib';

import ClipItToExtension, {hasPlainText, hasText, imageType, isPopupWindow, isSensitive, sourceAppName} from './extension.js';

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
    assertEq(state.display.handlerCount(), 0, 'stopped waiting');
    assertEq(win.handlerCount(), 0, 'shown handler removed');
    ext.disable();
});

test('disable cancels a pending activation', () => {
    const ext = enabled();
    state.service.ActivateWindow();
    assertEq(state.display.handlerCount(), 1);
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

print(`\n${passed} passed, ${failed} failed`);
if (failed > 0)
    System.exit(1);
