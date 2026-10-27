# How capture works per session

Back to the [README](../README.md).

| Session | Backend |
|---|---|
| GNOME Wayland + the bundled extension | `gnome-ext` |
| Other Wayland compositors with data-control (wlroots, KDE) + `wl-clipboard` | `wl-paste` |
| X11, Windows | `qt-clipboard` |
| Wayland without either | `qt-clipboard`, degraded: only copies made while the popup has focus |

GNOME's Wayland session lets no regular app watch the clipboard or take focus, so ClipItTo
ships a small [GNOME Shell extension](../gnome-extension/clipitto@nimiz.github.io) that reports
clipboard changes to the daemon and lets the popup take focus and paste. The daemon notices
the extension coming and going. The extension can also own the shortcut itself, which is the
most reliable option on Wayland (then remove the custom shortcut with `clipitto-shortcut --remove`):

```bash
gsettings --schemadir /usr/share/gnome-shell/extensions/clipitto@nimiz.github.io/schemas \
  set org.gnome.shell.extensions.clipitto toggle-shortcut "['<Control><Alt>v']"
```
