#include "Hotkey.h"

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
        QTest::newRow("win aliases") << "Super+Shift+1" << uint(win32::ModWin | win32::ModShift) << 0x31u
                                      << "Shift+Win+1";
        QTest::newRow("function key") << "Win+F12" << uint(win32::ModWin) << 0x7Bu << "Win+F12";
        QTest::newRow("named key") << "ctrl+space" << uint(win32::ModControl) << 0x20u << "Ctrl+Space";
        QTest::newRow("page down") << "Alt+PageDown" << uint(win32::ModAlt) << 0x22u << "Alt+PageDown";
        QTest::newRow("backtick") << "Ctrl+`" << uint(win32::ModControl) << 0xC0u << "Ctrl+`";
        QTest::newRow("grave alias") << "ctrl+grave" << uint(win32::ModControl) << 0xC0u << "Ctrl+`";
        QTest::newRow("slash") << "Ctrl+Shift+/" << uint(win32::ModControl | win32::ModShift) << 0xBFu
                               << "Ctrl+Shift+/";
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
