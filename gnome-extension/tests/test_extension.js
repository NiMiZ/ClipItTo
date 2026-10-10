// Runs extension.js against the fake shell in stubs/ (see run-tests.sh). GLib is real.

import System from 'system';

import {CLIPBOARD, PRIMARY, makeWindow, resetState, state} from './stubs/state.js';
import ClipItToExtension, {hasText, isPopupWindow, isSensitive} from './extension.js';

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

test('secrets are never read', () => {
    const ext = enabled();
    state.selection.setOwner(CLIPBOARD, [...TEXT, 'x-kde-passwordManagerHint'], 'hunter2');
    assertEq(state.textReads, 0, 'text reads');
    assertEq(state.dbusCalls.length, 0, 'calls');
    ext.disable();
});

test('non-text, empty and oversized selections are skipped', () => {
    const ext = enabled();
    state.selection.setOwner(CLIPBOARD, ['image/png'], null);
    assertEq(state.textReads, 0, 'image read');
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
