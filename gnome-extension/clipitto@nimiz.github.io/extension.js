// ClipItTo companion extension.
//
// GNOME's Wayland session offers regular apps no way to watch the clipboard, take focus
// or type keys into other windows. This extension runs inside the shell and does those
// things for the ClipItTo daemon:
//   - reports every clipboard change to the daemon (io.github.nimiz.ClipItTo.Ingest, or
//     IngestImage for a copied picture); a copy marked by a password manager only when the
//     daemon keeps password copies (WantsPasswords), with the app it came from;
//   - exports io.github.nimiz.ClipItTo.Shell so the daemon can focus its popup (placing it
//     where the user's popup_position says: a Wayland client cannot place its own window),
//     set the clipboard and paste;
//   - centers ClipItTo's dialogs (Settings) over the popup instead of leaving them wherever
//     the shell would put them, e.g. across two monitors;
//   - optionally binds a shortcut that toggles the popup.

import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';
import St from 'gi://St';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';

const DAEMON_NAME = 'io.github.nimiz.ClipItTo';
const DAEMON_PATH = '/io/github/nimiz/ClipItTo';
const DAEMON_IFACE = 'io.github.nimiz.ClipItTo';

const SHELL_NAME = 'io.github.nimiz.ClipItTo.Shell';
const SHELL_PATH = '/io/github/nimiz/ClipItTo/Shell';
const SHELL_IFACE_XML = `
<node>
  <interface name="io.github.nimiz.ClipItTo.Shell">
    <method name="ActivateWindow"/>
    <method name="ActivateWindowAt">
      <arg type="s" direction="in"/>
    </method>
    <method name="SetClipboard">
      <arg name="text" type="s" direction="in"/>
    </method>
    <method name="SetClipboardContent">
      <arg name="data" type="ay" direction="in"/>
      <arg name="mime_type" type="s" direction="in"/>
    </method>
    <method name="Paste">
      <arg name="keys" type="s" direction="in"/>
    </method>
    <method name="FocusedApp">
      <arg name="ids" type="as" direction="out"/>
    </method>
  </interface>
</node>`;

// The popup's Wayland app_id (QGuiApplication::setDesktopFileName) and X11 WM_CLASS values.
const APP_ID = 'io.github.nimiz.ClipItTo';
const X11_CLASSES = ['clipitto', 'clipitto-daemon'];

// Larger texts are not worth a D-Bus round trip; the daemon applies its own, configurable cap.
const MAX_TEXT_LENGTH = 1024 * 1024;
// Pictures: the daemon applies its own limit (max_image_bytes, 50 MB by default) and accepts
// only these formats.
const MAX_IMAGE_BYTES = 64 * 1024 * 1024;
const IMAGE_TYPES = ['image/png', 'image/bmp', 'image/jpeg', 'image/gif'];
const SENSITIVE_MIME_TYPES = [
    'x-kde-passwordmanagerhint',
    'application/x-kde-passwordmanagerhint',
    'application/x-nspasteboard-concealed-type',
];
const X11_TEXT_TARGETS = ['UTF8_STRING', 'STRING', 'TEXT', 'COMPOUND_TEXT'];
// Names the app a password copy came from, for its default note (see ClipFilter::sourceApp).
const SOURCE_PREFIX = 'x-clipitto-source:';

// How long ActivateWindow waits for a popup that is not mapped yet.
const ACTIVATION_WAIT_MS = 1000;

export function isSensitive(mimeTypes) {
    return mimeTypes.some(type => SENSITIVE_MIME_TYPES.includes(type.toLowerCase()));
}

export function hasText(mimeTypes) {
    return mimeTypes.some(type => type.startsWith('text/') || X11_TEXT_TARGETS.includes(type));
}

// Plain text, as opposed to e.g. only text/html (what a browser offers for a copied picture).
export function hasPlainText(mimeTypes) {
    return mimeTypes.some(type => type.toLowerCase().startsWith('text/plain') || X11_TEXT_TARGETS.includes(type));
}

// The picture type to fetch (PNG preferred), or null when there is none the daemon accepts.
export function imageType(mimeTypes) {
    const lower = mimeTypes.map(type => type.toLowerCase());
    return IMAGE_TYPES.find(type => lower.includes(type)) ?? null;
}

// The app's name for a window ("KeePassXC"), or its WM_CLASS; '' when unknown.
export function sourceAppName(win) {
    if (!win)
        return '';
    const name = Shell.WindowTracker.get_default().get_window_app(win)?.get_name?.() ?? win.get_wm_class?.() ?? '';
    return name.trim().slice(0, 64);
}

// What names the program of a window, for the daemon's per-app paste keys: its WM_CLASS (class and instance;
// on Wayland the app id) and the desktop id of its app. Never the title. Empty when there is no window.
export function focusedAppIds(win) {
    if (!win)
        return [];
    const ids = [win.get_wm_class?.(), win.get_wm_class_instance?.(),
        Shell.WindowTracker.get_default().get_window_app(win)?.get_id?.()?.replace(/\.desktop$/, '')]
        .map(id => (id ?? '').trim().slice(0, 128));
    return [...new Set(ids.filter(id => id !== ''))];
}

// Where the popup goes (its top-left, in screen coordinates), mirroring PopupWindow::placeOnScreen
// for X11: `area` is the work area of the monitor, `size` the popup's {width, height}, `pointer`
// {x, y}, `last` {x, y} or null. Unknown values and "last" without a last place fall back to
// the pointer; "center" is horizontally centered in the upper third.
export function popupPlacement(position, area, size, pointer, last) {
    const marginX = Math.floor(area.width * 5 / 100);
    const marginY = Math.floor(area.height * 5 / 100);
    const keep = (x, y) => ({
        x: Math.max(area.x, Math.min(x, area.x + area.width - size.width)),
        y: Math.max(area.y, Math.min(y, area.y + area.height - size.height)),
    });
    switch (position) {
    case 'last':
        if (last)
            return keep(last.x, last.y);
        break;
    case 'center':
        return keep(area.x + Math.round((area.width - size.width) / 2),
            area.y + Math.round((area.height - size.height) / 3));
    case 'top-left':
        return keep(area.x + marginX, area.y + marginY);
    case 'top-right':
        return keep(area.x + area.width - marginX - size.width, area.y + marginY);
    case 'bottom-left':
        return keep(area.x + marginX, area.y + area.height - marginY - size.height);
    case 'bottom-right':
        return keep(area.x + area.width - marginX - size.width, area.y + area.height - marginY - size.height);
    default:
        break;
    }
    return keep(pointer.x - Math.round(size.width / 2), pointer.y - 24);
}

// The popup itself, not a dialog of ClipItTo's (those have the popup as their parent).
export function isPopupMain(win) {
    return isPopupWindow(win) && !win.get_transient_for?.();
}

export function isPopupWindow(win) {
    const wmClass = (win.get_wm_class() ?? '').toLowerCase();
    const instance = (win.get_wm_class_instance() ?? '').toLowerCase();
    if (wmClass === APP_ID.toLowerCase() || X11_CLASSES.includes(wmClass) || X11_CLASSES.includes(instance))
        return true;
    const app = Shell.WindowTracker.get_default().get_window_app(win);
    return app?.get_id() === `${APP_ID}.desktop`;
}

export default class ClipItToExtension extends Extension {
    enable() {
        this._settings = this.getSettings();

        this._selection = global.display.get_selection();
        this._ownerChangedId = this._selection.connect('owner-changed',
            (_selection, type) => this._onOwnerChanged(type));

        this._dbus = Gio.DBusExportedObject.wrapJSObject(SHELL_IFACE_XML, {
            ActivateWindow: () => this._activatePopup('center'),
            ActivateWindowAt: position => this._activatePopup(position),
            SetClipboard: text => St.Clipboard.get_default().set_text(St.ClipboardType.CLIPBOARD, text),
            SetClipboardContent: (data, mimeType) => {
                if (mimeType === 'image/png') // the daemon only ever sends PNG pictures
                    St.Clipboard.get_default().set_content(St.ClipboardType.CLIPBOARD, mimeType, new GLib.Bytes(data));
            },
            Paste: keys => this._paste(keys),
            FocusedApp: () => new GLib.Variant('(as)', [this._focusedApp()]),
        });
        this._dbus.export(Gio.DBus.session, SHELL_PATH);
        // Owning the name is how the daemon learns the extension is running.
        this._nameId = Gio.bus_own_name_on_connection(Gio.DBus.session, SHELL_NAME,
            Gio.BusNameOwnerFlags.NONE, null, null);

        this._shortcutChangedId = this._settings.connect('changed::toggle-shortcut',
            () => this._updateKeybinding());
        this._updateKeybinding();

        this._dialogWatchId = global.display.connect('window-created',
            (_display, created) => this._onWindowCreated(created));
    }

    disable() {
        this._cancelPendingActivation();
        global.display.disconnect(this._dialogWatchId);
        this._removeKeybinding();
        this._settings.disconnect(this._shortcutChangedId);
        this._selection.disconnect(this._ownerChangedId);
        Gio.bus_unown_name(this._nameId);
        this._dbus.unexport();

        this._virtualKeyboard = null;
        this._lastFocused = null;
        this._dbus = null;
        this._selection = null;
        this._settings = null;
    }

    _onOwnerChanged(type) {
        let stType;
        if (type === Meta.SelectionType.SELECTION_CLIPBOARD)
            stType = St.ClipboardType.CLIPBOARD;
        else if (type === Meta.SelectionType.SELECTION_PRIMARY && this._settings.get_boolean('capture-primary'))
            stType = St.ClipboardType.PRIMARY;
        else
            return;

        // Check the MIME types first so the content of a secret is never read unless the daemon
        // keeps password copies. Plain text first, then a picture (clipboard only), then any
        // other text.
        const mimeTypes = this._selection.get_mimetypes(type);
        if (isSensitive(mimeTypes)) {
            if (stType === St.ClipboardType.CLIPBOARD && hasText(mimeTypes))
                this._sendSecret(stType, mimeTypes);
            return;
        }
        const picture = imageType(mimeTypes);
        if (!hasPlainText(mimeTypes) && picture && stType === St.ClipboardType.CLIPBOARD) {
            this._sendImage(stType, picture, mimeTypes);
            return;
        }
        if (!hasText(mimeTypes))
            return;

        St.Clipboard.get_default().get_text(stType, (_clipboard, text) => {
            if (!text || text.length > MAX_TEXT_LENGTH)
                return;
            // The formatted version too, for the clipboard (not the primary selection) when the copy offers one;
            // the daemon keeps it only if it has formatting and keep_formatting is on.
            if (stType === St.ClipboardType.CLIPBOARD && mimeTypes.includes('text/html')) {
                St.Clipboard.get_default().get_content(stType, 'text/html', (_c, bytes) => {
                    const size = bytes?.get_size() ?? 0;
                    // The clipboard may have changed since the text was read: only if it still offers the same
                    // kinds of data (and none is a password manager's marker) is this HTML that copy's.
                    const now = this._selection?.get_mimetypes(type) ?? [];
                    const same = now.length === mimeTypes.length && now.every((t, i) => t === mimeTypes[i]);
                    if (same && !isSensitive(now) && size > 0 && size <= MAX_TEXT_LENGTH) {
                        const html = new TextDecoder().decode(bytes.toArray());
                        this._callDaemon('IngestRich', new GLib.Variant('(ssas)', [text, html, mimeTypes]),
                            Gio.DBusCallFlags.NO_AUTO_START, () => this._ingestText(text, mimeTypes));
                        return;
                    }
                    this._ingestText(text, mimeTypes);
                });
                return;
            }
            this._ingestText(text, mimeTypes);
        });
    }

    // NO_AUTO_START: a stopped daemon stays stopped; the copy simply isn't recorded.
    _ingestText(text, mimeTypes) {
        this._callDaemon('Ingest', new GLib.Variant('(sas)', [text, mimeTypes]), Gio.DBusCallFlags.NO_AUTO_START);
    }

    // Asks the daemon first: without a master password it keeps no password copies, and then
    // the secret is never read. An older daemon without WantsPasswords counts as "no".
    _sendSecret(stType, mimeTypes) {
        const source = sourceAppName(global.display.focus_window);
        Gio.DBus.session.call(DAEMON_NAME, DAEMON_PATH, DAEMON_IFACE, 'WantsPasswords', null,
            new GLib.VariantType('(b)'), Gio.DBusCallFlags.NO_AUTO_START, -1, null, (connection, result) => {
                let wanted = false;
                try {
                    [wanted] = connection.call_finish(result).deepUnpack();
                } catch (e) {
                    return;
                }
                if (!wanted || !this._selection)
                    return;
                St.Clipboard.get_default().get_text(stType, (_clipboard, text) => {
                    if (!text || text.length > MAX_TEXT_LENGTH)
                        return;
                    // An app can't name itself: only the window tracker's name counts.
                    const own = mimeTypes.filter(type => !type.startsWith(SOURCE_PREFIX));
                    const types = source ? [...own, SOURCE_PREFIX + source] : own;
                    this._callDaemon('Ingest', new GLib.Variant('(sas)', [text, types]),
                        Gio.DBusCallFlags.NO_AUTO_START);
                });
            });
    }

    _sendImage(stType, mimeType, mimeTypes) {
        St.Clipboard.get_default().get_content(stType, mimeType, (_clipboard, bytes) => {
            const size = bytes?.get_size() ?? 0;
            if (size === 0 || size > MAX_IMAGE_BYTES)
                return;
            // Wrapped without copying the picture into a JavaScript array first.
            const data = GLib.Variant.new_from_bytes(new GLib.VariantType('ay'), bytes, true);
            const parameters = GLib.Variant.new_tuple(
                [data, new GLib.Variant('s', mimeType), new GLib.Variant('as', mimeTypes)]);
            this._callDaemon('IngestImage', parameters, Gio.DBusCallFlags.NO_AUTO_START);
        });
    }

    // `ifUnknown`: called when the daemon is older and has no such method (IngestRich), to send the plain version.
    _callDaemon(method, parameters, flags, ifUnknown = null) {
        Gio.DBus.session.call(DAEMON_NAME, DAEMON_PATH, DAEMON_IFACE, method, parameters, null,
            flags, -1, null, (connection, result) => {
                try {
                    connection.call_finish(result);
                } catch (e) {
                    if (ifUnknown && (e.matches?.(Gio.DBusError, Gio.DBusError.UNKNOWN_METHOD) ||
                        e.matches?.(Gio.DBusError, Gio.DBusError.INVALID_ARGS))) {
                        ifUnknown();
                        return;
                    }
                    // A daemon that is not running is normal (logged out of it, not installed).
                    const notRunning = e.matches?.(Gio.DBusError, Gio.DBusError.SERVICE_UNKNOWN) ||
                        e.matches?.(Gio.DBusError, Gio.DBusError.NAME_HAS_NO_OWNER);
                    if (!notRunning)
                        console.warn(`ClipItTo: ${method} failed: ${e.message}`);
                }
            });
    }

    _activatePopup(position) {
        this._cancelPendingActivation();
        const win = global.get_window_actors().map(actor => actor.meta_window).find(isPopupMain);
        if (win) {
            this._focus(win, position);
            return;
        }

        // The daemon calls us right after showing the popup, which may not be mapped yet.
        this._windowCreatedId = global.display.connect('window-created', (_display, created) => {
            if (!isPopupMain(created))
                return;
            this._cancelPendingActivation();
            const shownId = created.connect('shown', () => {
                created.disconnect(shownId);
                this._focus(created, position);
            });
        });
        this._activationTimeoutId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, ACTIVATION_WAIT_MS, () => {
            this._activationTimeoutId = 0;
            this._cancelPendingActivation();
            return GLib.SOURCE_REMOVE;
        });
    }

    _cancelPendingActivation() {
        if (this._windowCreatedId) {
            global.display.disconnect(this._windowCreatedId);
            this._windowCreatedId = 0;
        }
        if (this._activationTimeoutId) {
            GLib.Source.remove(this._activationTimeoutId);
            this._activationTimeoutId = 0;
        }
    }

    // Places the popup as `position` says (see popupPlacement), then focuses it. "last" is where
    // it was last put or dragged during this shell session.
    _focus(win, position) {
        let monitor = global.display.get_current_monitor();
        let last = null;
        if (position === 'last' && this._lastPlace) {
            last = this._lastPlace;
            monitor = global.display.get_monitor_index_for_rect?.(
                {x: last.x, y: last.y, width: 1, height: 1}) ?? monitor;
            if (monitor < 0)
                monitor = global.display.get_current_monitor();
        }
        const area = Main.layoutManager.getWorkAreaForMonitor(monitor);
        const frame = win.get_frame_rect();
        const [px, py] = global.get_pointer?.() ?? [area.x + area.width / 2, area.y];
        const place = popupPlacement(position, area, {width: frame.width, height: frame.height},
            {x: px, y: py}, last);
        win.move_frame(true, place.x, place.y);
        // Remember where the user leaves it (dragging it by its name moves it).
        win.connect('position-changed', () => {
            const f = win.get_frame_rect();
            this._lastPlace = {x: f.x, y: f.y};
        });
        Main.activateWindow(win);
    }

    // A dialog of ClipItTo's (Settings) opens centered over the popup, on the popup's monitor.
    _onWindowCreated(win) {
        if (!isPopupWindow(win) || !win.get_transient_for?.())
            return;
        const shownId = win.connect('shown', () => {
            win.disconnect(shownId);
            const parent = win.get_transient_for();
            if (!parent)
                return;
            const p = parent.get_frame_rect();
            const f = win.get_frame_rect();
            const monitor = global.display.get_monitor_index_for_rect?.(p) ?? global.display.get_current_monitor();
            const area = Main.layoutManager.getWorkAreaForMonitor(monitor);
            const x = Math.max(area.x, Math.min(p.x + Math.round((p.width - f.width) / 2),
                area.x + area.width - f.width));
            const y = Math.max(area.y, Math.min(p.y + Math.round((p.height - f.height) / 2),
                area.y + area.height - f.height));
            win.move_frame(true, x, y);
        });
    }

    // The program that has the focus, which the paste will go to: asked by the daemon just before it shows
    // the popup. While the popup itself has the focus, the last other one.
    _focusedApp() {
        const win = global.display.focus_window;
        if (win && !isPopupWindow(win))
            this._lastFocused = focusedAppIds(win);
        return this._lastFocused ?? [];
    }

    _paste(keys) {
        if (!this._virtualKeyboard) {
            const seat = Clutter.get_default_backend().get_default_seat();
            this._virtualKeyboard = seat.create_virtual_device(Clutter.InputDeviceType.KEYBOARD_DEVICE);
        }
        let combo = [Clutter.KEY_Control_L, Clutter.KEY_v];
        if (keys === 'shift+insert')
            combo = [Clutter.KEY_Shift_L, Clutter.KEY_Insert];
        else if (keys === 'ctrl+shift+v')
            combo = [Clutter.KEY_Control_L, Clutter.KEY_Shift_L, Clutter.KEY_v];
        const time = GLib.get_monotonic_time();
        for (const key of combo)
            this._virtualKeyboard.notify_keyval(time, key, Clutter.KeyState.PRESSED);
        for (const key of [...combo].reverse())
            this._virtualKeyboard.notify_keyval(time, key, Clutter.KeyState.RELEASED);
    }

    _updateKeybinding() {
        this._removeKeybinding();
        if (this._settings.get_strv('toggle-shortcut').length === 0)
            return;
        Main.wm.addKeybinding('toggle-shortcut', this._settings, Meta.KeyBindingFlags.IGNORE_AUTOREPEAT,
            Shell.ActionMode.NORMAL | Shell.ActionMode.OVERVIEW,
            // Auto-start allowed: the shortcut should bring a stopped daemon up.
            () => this._callDaemon('Toggle', new GLib.Variant('(s)', ['']), Gio.DBusCallFlags.NONE));
        this._keybindingAdded = true;
    }

    _removeKeybinding() {
        if (this._keybindingAdded) {
            Main.wm.removeKeybinding('toggle-shortcut');
            this._keybindingAdded = false;
        }
    }
}
