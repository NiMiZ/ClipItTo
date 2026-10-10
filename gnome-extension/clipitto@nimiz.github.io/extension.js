// ClipItTo companion extension.
//
// GNOME's Wayland session offers regular apps no way to watch the clipboard, take focus
// or type keys into other windows. This extension runs inside the shell and does those
// things for the ClipItTo daemon:
//   - reports every clipboard change to the daemon (io.github.nimiz.ClipItTo.Ingest);
//   - exports io.github.nimiz.ClipItTo.Shell so the daemon can focus its popup, set the
//     clipboard and paste;
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
    <method name="SetClipboard">
      <arg name="text" type="s" direction="in"/>
    </method>
    <method name="Paste">
      <arg name="keys" type="s" direction="in"/>
    </method>
  </interface>
</node>`;

// The popup's Wayland app_id (QGuiApplication::setDesktopFileName) and X11 WM_CLASS values.
const APP_ID = 'io.github.nimiz.ClipItTo';
const X11_CLASSES = ['clipitto', 'clipitto-daemon'];

// Larger texts are not worth a D-Bus round trip; the daemon applies its own, configurable cap.
const MAX_TEXT_LENGTH = 1024 * 1024;
const SENSITIVE_MIME_TYPES = [
    'x-kde-passwordmanagerhint',
    'application/x-kde-passwordmanagerhint',
    'application/x-nspasteboard-concealed-type',
];
const X11_TEXT_TARGETS = ['UTF8_STRING', 'STRING', 'TEXT', 'COMPOUND_TEXT'];

// How long ActivateWindow waits for a popup that is not mapped yet.
const ACTIVATION_WAIT_MS = 1000;

export function isSensitive(mimeTypes) {
    return mimeTypes.some(type => SENSITIVE_MIME_TYPES.includes(type.toLowerCase()));
}

export function hasText(mimeTypes) {
    return mimeTypes.some(type => type.startsWith('text/') || X11_TEXT_TARGETS.includes(type));
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
            ActivateWindow: () => this._activatePopup(),
            SetClipboard: text => St.Clipboard.get_default().set_text(St.ClipboardType.CLIPBOARD, text),
            Paste: keys => this._paste(keys),
        });
        this._dbus.export(Gio.DBus.session, SHELL_PATH);
        // Owning the name is how the daemon learns the extension is running.
        this._nameId = Gio.bus_own_name_on_connection(Gio.DBus.session, SHELL_NAME,
            Gio.BusNameOwnerFlags.NONE, null, null);

        this._shortcutChangedId = this._settings.connect('changed::toggle-shortcut',
            () => this._updateKeybinding());
        this._updateKeybinding();
    }

    disable() {
        this._cancelPendingActivation();
        this._removeKeybinding();
        this._settings.disconnect(this._shortcutChangedId);
        this._selection.disconnect(this._ownerChangedId);
        Gio.bus_unown_name(this._nameId);
        this._dbus.unexport();

        this._virtualKeyboard = null;
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

        // Check the MIME types first so the content of a secret is never even read.
        const mimeTypes = this._selection.get_mimetypes(type);
        if (isSensitive(mimeTypes) || !hasText(mimeTypes))
            return;

        St.Clipboard.get_default().get_text(stType, (_clipboard, text) => {
            if (!text || text.length > MAX_TEXT_LENGTH)
                return;
            // NO_AUTO_START: a stopped daemon stays stopped; the copy simply isn't recorded.
            this._callDaemon('Ingest', new GLib.Variant('(sas)', [text, mimeTypes]),
                Gio.DBusCallFlags.NO_AUTO_START);
        });
    }

    _callDaemon(method, parameters, flags) {
        Gio.DBus.session.call(DAEMON_NAME, DAEMON_PATH, DAEMON_IFACE, method, parameters, null,
            flags, -1, null, (connection, result) => {
                try {
                    connection.call_finish(result);
                } catch (e) {
                    // A daemon that is not running is normal (logged out of it, not installed).
                    const notRunning = e.matches?.(Gio.DBusError, Gio.DBusError.SERVICE_UNKNOWN) ||
                        e.matches?.(Gio.DBusError, Gio.DBusError.NAME_HAS_NO_OWNER);
                    if (!notRunning)
                        console.warn(`ClipItTo: ${method} failed: ${e.message}`);
                }
            });
    }

    _activatePopup() {
        this._cancelPendingActivation();
        const win = global.get_window_actors().map(actor => actor.meta_window).find(isPopupWindow);
        if (win) {
            this._focus(win);
            return;
        }

        // The daemon calls us right after showing the popup, which may not be mapped yet.
        this._windowCreatedId = global.display.connect('window-created', (_display, created) => {
            if (!isPopupWindow(created))
                return;
            this._cancelPendingActivation();
            const shownId = created.connect('shown', () => {
                created.disconnect(shownId);
                this._focus(created);
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

    // Centers the popup in the upper part of the monitor under the pointer, then focuses it.
    _focus(win) {
        const monitor = global.display.get_current_monitor();
        const area = Main.layoutManager.getWorkAreaForMonitor(monitor);
        const frame = win.get_frame_rect();
        const x = area.x + Math.round((area.width - frame.width) / 2);
        const y = area.y + Math.round((area.height - frame.height) / 3);
        win.move_frame(true, x, y);
        Main.activateWindow(win);
    }

    _paste(keys) {
        if (!this._virtualKeyboard) {
            const seat = Clutter.get_default_backend().get_default_seat();
            this._virtualKeyboard = seat.create_virtual_device(Clutter.InputDeviceType.KEYBOARD_DEVICE);
        }
        const combo = keys === 'shift+insert'
            ? [Clutter.KEY_Shift_L, Clutter.KEY_Insert]
            : [Clutter.KEY_Control_L, Clutter.KEY_v];
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
