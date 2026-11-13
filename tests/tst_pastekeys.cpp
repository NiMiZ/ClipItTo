#include "PasteKeys.h"

#include <QSet>
#include <QTest>

using namespace clipitto;
using namespace clipitto::pastekeys;

class TestPasteKeys : public QObject {
    Q_OBJECT

private:
    static PasteTarget app(const QStringList& ids) { return PasteTarget::known(ids); }

private slots:
    void chordsRoundTripAndTolerateCaseAndSpaces()
    {
        for (const PasteKeys keys : {PasteKeys::CtrlV, PasteKeys::ShiftInsert, PasteKeys::CtrlShiftV}) {
            PasteKeys back = PasteKeys::CtrlV;
            QVERIFY(parseChord(chordName(keys), back));
            QVERIFY(back == keys);
        }
        PasteKeys keys = PasteKeys::CtrlV;
        QVERIFY(parseChord(QStringLiteral(" Ctrl + Shift + V "), keys));
        QVERIFY(keys == PasteKeys::CtrlShiftV);
        QVERIFY(!parseChord(QStringLiteral("ctrl+c"), keys));
        QVERIFY(!parseChord(QString(), keys));
        QCOMPARE(chordNames(), (QStringList{QStringLiteral("ctrl+v"), QStringLiteral("shift+insert"),
                                            QStringLiteral("ctrl+shift+v")}));
    }

    void rulesParseFormatAndRejectNonsense()
    {
        PasteKeyRule rule;
        QVERIFY(parse(QStringLiteral("  WindowsTerminal.exe=Ctrl+Shift+V "), rule));
        QCOMPARE(rule.pattern, QStringLiteral("WindowsTerminal.exe"));
        QVERIFY(rule.keys == PasteKeys::CtrlShiftV);
        QCOMPARE(format(rule), QStringLiteral("WindowsTerminal.exe = ctrl+shift+v"));

        PasteKeyRule untouched{QStringLiteral("keep"), PasteKeys::ShiftInsert};
        for (const char* bad : {"", "xterm", "xterm = ", "xterm = ctrl+c", " = ctrl+v", "a\"b = ctrl+v", "*** = ctrl+v",
                                "x = y = ctrl+v", "xterm = ctrl+v+x"}) {
            QVERIFY2(!parse(QString::fromLatin1(bad), untouched), bad);
        }
        QCOMPARE(untouched.pattern, QStringLiteral("keep")); // a refusal leaves it alone
        QVERIFY(!parse(QStringLiteral("%1 = ctrl+v").arg(QString(kMaxPatternLength + 1, QLatin1Char('a'))), untouched));
        QVERIFY(parse(QStringLiteral("%1 = ctrl+v").arg(QString(kMaxPatternLength, QLatin1Char('a'))), untouched));
    }

    void validationSaysWhy()
    {
        QVERIFY(validatePattern(QStringLiteral("xterm")).isEmpty());
        QVERIFY(validatePattern(QStringLiteral("org.gnome.Terminal")).isEmpty());
        QVERIFY(validatePattern(QStringLiteral("term*")).isEmpty());
        QVERIFY(!validatePattern(QString()).isEmpty());
        QVERIFY(!validatePattern(QStringLiteral("   ")).isEmpty());
        QVERIFY(!validatePattern(QStringLiteral("a=b")).isEmpty());
        QVERIFY(!validatePattern(QStringLiteral("a\nb")).isEmpty());
        QVERIFY(!validatePattern(QStringLiteral("*a*a*a*a")).isEmpty()); // too many wildcards
        QVERIFY(!validatePattern(QStringLiteral("*?*")).isEmpty()); // would match every program
        QVERIFY(!validatePattern(QString(kMaxPatternLength + 1, QLatin1Char('x'))).isEmpty());
    }

    void matchingIgnoresCaseAndExeAndSupportsWildcards()
    {
        const QStringList ids{QStringLiteral("WindowsTerminal.exe"), QStringLiteral("CASCADIA_HOSTING_WINDOW_CLASS")};
        QVERIFY(matches(QStringLiteral("windowsterminal"), ids));
        QVERIFY(matches(QStringLiteral("WINDOWSTERMINAL.EXE"), ids));
        QVERIFY(matches(QStringLiteral("cascadia_hosting_window_class"), ids));
        QVERIFY(matches(QStringLiteral("windows*"), ids));
        QVERIFY(matches(QStringLiteral("windowsterminal.ex?"), ids) == false); // ".exe" is dropped from both sides
        QVERIFY(matches(QStringLiteral("windowsterminal?"), ids) == false);
        QVERIFY(!matches(QStringLiteral("terminal"), ids)); // whole names, not parts
        QVERIFY(!matches(QStringLiteral("windows"), ids));
        QVERIFY(!matches(QString(), ids));
        QVERIFY(!matches(QStringLiteral("anything"), {}));
        // A dot in a pattern is a dot, not "any character".
        QVERIFY(!matches(QStringLiteral("org.gnome.Terminal"), {QStringLiteral("orgXgnomeXTerminal")}));
        QVERIFY(matches(QStringLiteral("org.gnome.*"), {QStringLiteral("org.gnome.Terminal")}));
    }

    void builtInTerminalsAreKnownAndWellFormed()
    {
        QSet<QString> seen;
        for (const PasteKeyRule& rule : builtInRules()) {
            QVERIFY2(validate(rule).isEmpty(), qPrintable(rule.pattern));
            QVERIFY2(!seen.contains(rule.pattern.toLower()), qPrintable(rule.pattern)); // no entry hides another
            seen.insert(rule.pattern.toLower());
        }
        const auto keysFor = [](const QStringList& ids) { return choose(PasteKeys::CtrlV, true, {}, PasteTarget::known(ids)); };
        for (const char* terminal : {"WindowsTerminal.exe", "alacritty", "kitty", "WezTerm", "gnome-terminal-server",
                                     "Gnome-terminal", "org.gnome.Terminal", "konsole", "XTerm", "Tilix", "terminator",
                                     "foot", "ghostty", "com.mitchellh.ghostty"}) {
            const Choice c = keysFor({QString::fromLatin1(terminal)});
            QVERIFY2(c.keys == PasteKeys::CtrlShiftV && c.builtIn, terminal);
        }
        // Windows consoles take Shift+Insert whatever their settings.
        for (const char* console : {"conhost.exe", "ConsoleWindowClass", "cmd.exe", "powershell.exe", "mintty"}) {
            QVERIFY2(keysFor({QString::fromLatin1(console)}).keys == PasteKeys::ShiftInsert, console);
        }
        // Ordinary programs are not terminals.
        for (const char* other : {"chrome.exe", "firefox", "notepad.exe", "Code", "libreoffice", "slack"}) {
            const Choice c = keysFor({QString::fromLatin1(other)});
            QVERIFY2(c.keys == PasteKeys::CtrlV && c.rule.isEmpty(), other);
        }
    }

    void yourRulesComeBeforeTheBuiltInOnes()
    {
        const QList<PasteKeyRule> mine{{QStringLiteral("xterm"), PasteKeys::ShiftInsert},
                                       {QStringLiteral("code"), PasteKeys::CtrlShiftV},
                                       {QStringLiteral("x*"), PasteKeys::CtrlV}};
        // xterm is built in as ctrl+shift+v: the user's rule wins, and the first of two matching user rules.
        Choice c = choose(PasteKeys::CtrlV, true, mine, app({QStringLiteral("xterm"), QStringLiteral("XTerm")}));
        QVERIFY(c.keys == PasteKeys::ShiftInsert);
        QCOMPARE(c.rule, QStringLiteral("xterm"));
        QVERIFY(!c.builtIn);
        // A program only the user's rules know.
        c = choose(PasteKeys::CtrlV, true, mine, app({QStringLiteral("Code")}));
        QVERIFY(c.keys == PasteKeys::CtrlShiftV && !c.builtIn);
        // Not in your rules: the built-in terminal list.
        c = choose(PasteKeys::CtrlV, true, mine, app({QStringLiteral("kitty")}));
        QVERIFY(c.keys == PasteKeys::CtrlShiftV && c.builtIn);
        // Any one of the ids is enough (class or executable).
        c = choose(PasteKeys::CtrlV, true, mine, app({QStringLiteral("unrelated"), QStringLiteral("code")}));
        QVERIFY(c.keys == PasteKeys::CtrlShiftV);
        // A rule can also turn a terminal back to plain ctrl+v.
        c = choose(PasteKeys::ShiftInsert, true, {{QStringLiteral("kitty"), PasteKeys::CtrlV}}, app({QStringLiteral("kitty")}));
        QVERIFY(c.keys == PasteKeys::CtrlV);
    }

    void theGlobalSettingIsTheDefaultForEverythingElse()
    {
        for (const PasteKeys global : {PasteKeys::CtrlV, PasteKeys::ShiftInsert}) {
            const Choice c = choose(global, true, {}, app({QStringLiteral("notepad")}));
            QVERIFY(c.keys == global && c.rule.isEmpty() && !c.builtIn);
        }
    }

    void withoutAKnownTargetOrWithTheSwitchOffTheGlobalSettingIsUsed()
    {
        const QList<PasteKeyRule> mine{{QStringLiteral("kitty"), PasteKeys::CtrlShiftV}};
        const PasteTarget terminal = app({QStringLiteral("kitty")});
        // Off: not even your own rules apply.
        QVERIFY(choose(PasteKeys::ShiftInsert, false, mine, terminal).keys == PasteKeys::ShiftInsert);
        // Unsupported platform and "nothing remembered" both fall back, with a rule list that would match.
        for (const PasteTarget& target : {PasteTarget::unsupported(), PasteTarget::unknown()}) {
            const Choice c = choose(PasteKeys::ShiftInsert, true, mine, target);
            QVERIFY(c.keys == PasteKeys::ShiftInsert && c.rule.isEmpty());
        }
        // An "unsupported" target never carries names, but even if it did they would not count.
        PasteTarget odd;
        odd.ids = {QStringLiteral("kitty")};
        QVERIFY(choose(PasteKeys::CtrlV, true, mine, odd).keys == PasteKeys::CtrlV);
        QVERIFY(PasteTarget::unsupported().state == PasteTarget::State::Unsupported);
        QVERIFY(PasteTarget::unknown().state == PasteTarget::State::Unknown);
        QVERIFY(terminal.state == PasteTarget::State::Known);
    }
};

QTEST_GUILESS_MAIN(TestPasteKeys)
#include "tst_pastekeys.moc"
