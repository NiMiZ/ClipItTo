#include "Theme.h"

#include <QApplication>
#include <QPalette>
#include <QProcess>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>

#include <vector>

#ifdef Q_OS_WIN
#include <QSettings>
#endif

namespace clipitto::ui {

namespace {

struct Original {
    QString styleName;
    QPalette palette;
};

// The look before we touched anything, to return to for "System" in light mode.
const Original& original()
{
    static const Original value{QApplication::style()->name(), QApplication::palette()};
    return value;
}

Theme g_current = Theme::System;
bool g_dark = false;

std::vector<std::function<void()>>& listeners()
{
    static std::vector<std::function<void()>> value;
    return value;
}

void setStyle(const QString& name)
{
    if (QApplication::style()->name().compare(name, Qt::CaseInsensitive) != 0) {
        QApplication::setStyle(QStyleFactory::create(name));
    }
}

void apply()
{
    const bool dark = g_current == Theme::Dark || (g_current == Theme::System && systemPrefersDark());
    g_dark = dark;
    if (dark) {
        setStyle(QStringLiteral("Fusion"));
        QApplication::setPalette(darkPalette());
    } else if (g_current == Theme::Light) {
        setStyle(QStringLiteral("Fusion"));
        QApplication::setPalette(QApplication::style()->standardPalette());
    } else {
        setStyle(original().styleName);
        QApplication::setPalette(original().palette);
    }
    for (const auto& listener : listeners()) {
        listener();
    }
}

} // namespace

QPalette darkPalette()
{
    QPalette p;
    const QColor window(0x2b, 0x2d, 0x31);
    const QColor base(0x1f, 0x21, 0x24);
    const QColor text(0xe6, 0xe8, 0xeb);
    const QColor disabled(0x80, 0x84, 0x8b);
    const QColor highlight(0x3d, 0x7e, 0xd6);
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, window);
    p.setColor(QPalette::ToolTipBase, base);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, disabled);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, window);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Highlight, highlight);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Link, QColor(0x6c, 0xa8, 0xf0));
    p.setColor(QPalette::Mid, QColor(0x3a, 0x3d, 0x42));
    p.setColor(QPalette::Dark, QColor(0x18, 0x19, 0x1b));
    p.setColor(QPalette::Light, QColor(0x45, 0x48, 0x4e));
    for (const auto role : {QPalette::Text, QPalette::WindowText, QPalette::ButtonText}) {
        p.setColor(QPalette::Disabled, role, disabled);
    }
    return p;
}

bool currentThemeIsDark()
{
    return g_dark;
}

void onThemeApplied(std::function<void()> callback)
{
    listeners().push_back(std::move(callback));
}

bool systemPrefersDark()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    switch (QGuiApplication::styleHints()->colorScheme()) {
    case Qt::ColorScheme::Dark:
        return true;
    case Qt::ColorScheme::Light:
        return false;
    default:
        break;
    }
#endif
#if defined(Q_OS_WIN)
    const QSettings personalize(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"),
        QSettings::NativeFormat);
    return personalize.value(QStringLiteral("AppsUseLightTheme"), 1).toInt() == 0;
#elif defined(Q_OS_UNIX)
    if (qEnvironmentVariable("XDG_CURRENT_DESKTOP").contains(QLatin1String("GNOME"), Qt::CaseInsensitive)) {
        QProcess gsettings;
        gsettings.start(QStringLiteral("gsettings"),
                        {QStringLiteral("get"), QStringLiteral("org.gnome.desktop.interface"),
                         QStringLiteral("color-scheme")});
        if (gsettings.waitForFinished(1000) && gsettings.exitCode() == 0) {
            return gsettings.readAllStandardOutput().contains("dark");
        }
    }
#endif
    return original().palette.color(QPalette::Window).lightness() < 128;
}

void applyTheme(Theme theme)
{
    original(); // capture the platform look before the first change
    g_current = theme;
    apply();
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    static const bool followsDesktop = [] {
        QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, qApp, [] {
            if (g_current == Theme::System) {
                apply();
            }
        });
        return true;
    }();
    Q_UNUSED(followsDesktop);
#endif
}

} // namespace clipitto::ui
