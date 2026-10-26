#include "Hotkey.h"

#include <QSet>
#include <QTest>

using clipitto::parseHotkey;
namespace win32 = clipitto::win32;

class TestHotkey : public QObject {
    Q_OBJECT

private slots:
    void parses_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<uint>("modifiers");
        QTest::addColumn<uint>("vk");
        QTest::addColumn<QString>("canonical");

        QTest::newRow("default") << "Ctrl+Alt+V" << uint(win32::ModControl | win32::ModAlt) << 0x56u << "Ctrl+Alt+V";
        QTest::newRow("case and spaces") << " ctrl + shift + v " << uint(win32::ModControl | win32::ModShift) << 0x56u
                                          << "Ctrl+Shift+V";
        QTest::newRow("order normalized") << "Alt+Control+x" << uint(win32::ModControl | win32::ModAlt) << 0x58u
                                           << "Ctrl+Alt+X";
        // The Windows key is spelled Cmd on a Mac (it is the ⌘ key there).
#ifdef Q_OS_MACOS
        const QString win = QStringLiteral("Cmd");
#else
        const QString win = QStringLiteral("Win");
#endif
        QTest::newRow("win aliases") << "Super+Shift+1" << uint(win32::ModWin | win32::ModShift) << 0x31u
                                      << "Shift+" + win + "+1";
        QTest::newRow("cmd aliases") << "command+Option+v" << uint(win32::ModWin | win32::ModAlt) << 0x56u
                                      << "Alt+" + win + "+V";
        QTest::newRow("function key") << "Win+F12" << uint(win32::ModWin) << 0x7Bu << win + "+F12";
        QTest::newRow("named key") << "ctrl+space" << uint(win32::ModControl) << 0x20u << "Ctrl+Space";
        QTest::newRow("page down") << "Alt+PageDown" << uint(win32::ModAlt) << 0x22u << "Alt+PageDown";
        QTest::newRow("backtick") << "Ctrl+`" << uint(win32::ModControl) << 0xC0u << "Ctrl+`";
        QTest::newRow("grave alias") << "ctrl+grave" << uint(win32::ModControl) << 0xC0u << "Ctrl+`";
        QTest::newRow("slash") << "Ctrl+Shift+/" << uint(win32::ModControl | win32::ModShift) << 0xBFu
                               << "Ctrl+Shift+/";
    }

    void macKeyCodesAndModifiers_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<uint>("macModifiers");
        QTest::addColumn<uint>("macKey");
        // Carbon: cmd 0x100, shift 0x200, option 0x800, control 0x1000; kVK_* by key position.
        QTest::newRow("cmd+alt+v") << "Cmd+Alt+V" << 0x900u << 0x09u;
        QTest::newRow("ctrl+alt+v") << "Ctrl+Alt+V" << 0x1800u << 0x09u;
        QTest::newRow("a") << "Cmd+Shift+A" << 0x300u << 0x00u;
        QTest::newRow("z") << "Cmd+Z" << 0x100u << 0x06u;
        QTest::newRow("digit 1") << "Ctrl+1" << 0x1000u << 0x12u;
        QTest::newRow("digit 0") << "Ctrl+0" << 0x1000u << 0x1Du;
        QTest::newRow("backtick") << "Cmd+`" << 0x100u << 0x32u;
        QTest::newRow("space") << "Ctrl+Space" << 0x1000u << 0x31u;
        QTest::newRow("f1") << "Cmd+F1" << 0x100u << 0x7Au;
        QTest::newRow("f12") << "Cmd+F12" << 0x100u << 0x6Fu;
        QTest::newRow("f20") << "Cmd+F20" << 0x100u << 0x5Au;
        QTest::newRow("slash") << "Cmd+/" << 0x100u << 0x2Cu;
        QTest::newRow("page down") << "Alt+PageDown" << 0x800u << 0x79u;
        QTest::newRow("f21 has no Mac key") << "Cmd+F21" << 0x100u << uint(clipitto::NativeHotkey::kNoMacKey);
    }

    void macKeyCodesAndModifiers()
    {
        QFETCH(QString, text);
        QFETCH(uint, macModifiers);
        QFETCH(uint, macKey);
        const auto hotkey = parseHotkey(text);
        QVERIFY(hotkey);
        QCOMPARE(uint(hotkey->macModifiers), macModifiers);
        QCOMPARE(uint(hotkey->macKeyCode), macKey);
    }

    void everyLetterAndDigitHasADistinctMacKey()
    {
        QSet<uint> seen;
        for (const char c : QByteArrayLiteral("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789")) {
            const auto hotkey = parseHotkey(QStringLiteral("Cmd+") + QChar(c));
            QVERIFY(hotkey);
            QVERIFY(hotkey->macKeyCode != clipitto::NativeHotkey::kNoMacKey);
            QVERIFY2(!seen.contains(hotkey->macKeyCode), qPrintable(QString(QChar(c))));
            seen.insert(hotkey->macKeyCode);
        }
        QCOMPARE(seen.size(), 36);
    }

    void gtkAccelerators_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("gtk");
        QTest::newRow("default") << "Ctrl+Alt+V" << "<Control><Alt>v";
        QTest::newRow("backtick") << "Ctrl+`" << "<Control>grave";
        QTest::newRow("super f-key") << "Win+Shift+F9" << "<Shift><Super>F9";
        QTest::newRow("space") << "Ctrl+Space" << "<Control>space";
        QTest::newRow("page up") << "Alt+PageUp" << "<Alt>Page_Up";
    }

    void gtkAccelerators()
    {
        QFETCH(QString, text);
        QFETCH(QString, gtk);
        const auto hotkey = parseHotkey(text);
        QVERIFY(hotkey);
        QCOMPARE(hotkey->gtkAccelerator, gtk);
    }

    void parses()
    {
        QFETCH(QString, text);
        QFETCH(uint, modifiers);
        QFETCH(uint, vk);
        QFETCH(QString, canonical);
        const auto hotkey = parseHotkey(text);
        QVERIFY(hotkey);
        QCOMPARE(hotkey->modifiers, modifiers);
        QCOMPARE(hotkey->virtualKey, vk);
        QCOMPARE(hotkey->canonical, canonical);
    }

    void rejects_data()
    {
        QTest::addColumn<QString>("text");
        QTest::newRow("empty") << "";
        QTest::newRow("bare key") << "V";
        QTest::newRow("no key") << "Ctrl+";
        QTest::newRow("unknown modifier") << "Hyper+V";
        QTest::newRow("repeated modifier") << "Ctrl+Ctrl+V";
        QTest::newRow("unknown key") << "Ctrl+Banana";
        QTest::newRow("F25") << "Ctrl+F25";
        QTest::newRow("non-ascii") << "Ctrl+é";
        QTest::newRow("unsupported punctuation") << "Ctrl+~";
    }

    void rejects()
    {
        QFETCH(QString, text);
        QVERIFY(!parseHotkey(text));
    }
};

QTEST_GUILESS_MAIN(TestHotkey)
#include "tst_hotkey.moc"
