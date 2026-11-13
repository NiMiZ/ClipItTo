#pragma once

#include "Config.h"
#include "Hotkey.h"

#include <QString>

namespace clipitto {

// What the panic shortcut should be, from the settings: registered only when the panic key is on and a shortcut is set,
// and never the same as the shortcut that opens the popup.
struct PanicShortcut {
    bool wanted = false; // register it
    QString shortcut; // canonical spelling, when wanted
    QString problem; // why it cannot be registered (the key is on, the shortcut is not usable); empty otherwise
};

inline PanicShortcut panicShortcutFor(const Config& config)
{
    PanicShortcut plan;
    if (!config.panicKey) {
        return plan;
    }
    if (config.panicHotkey.isEmpty()) {
        plan.problem = QStringLiteral("no shortcut set");
        return plan;
    }
    const auto parsed = parseHotkey(config.panicHotkey);
    if (!parsed) {
        plan.problem = QStringLiteral("not a valid shortcut: %1").arg(config.panicHotkey);
        return plan;
    }
    const auto popup = parseHotkey(config.hotkey);
    if (popup && popup->canonical == parsed->canonical) {
        plan.problem = QStringLiteral("%1 is already the shortcut that opens the popup").arg(parsed->canonical);
        return plan;
    }
    plan.wanted = true;
    plan.shortcut = parsed->canonical;
    return plan;
}

// Registers (or unregisters) the panic shortcut with a platform hotkey (WindowsHotkey / MacHotkey, or a test double with
// setShortcut / clear / isRegistered / shortcut). Returns what `--status` shows for "panic_hotkey": empty when the panic
// key is off, else the shortcut, or "unavailable: why" (a conflict with another program is reported like the main one).
template <typename Hotkey>
QString applyPanicHotkey(Hotkey& hotkey, const Config& config)
{
    const PanicShortcut plan = panicShortcutFor(config);
    if (!plan.wanted) {
        hotkey.clear();
        return config.panicKey ? QStringLiteral("unavailable: ") + plan.problem : QString();
    }
    QString error;
    if (!hotkey.setShortcut(plan.shortcut, &error)) {
        hotkey.clear();
        return QStringLiteral("unavailable: ") + error;
    }
    return hotkey.shortcut();
}

} // namespace clipitto
