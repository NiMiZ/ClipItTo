#include "PasswordIgnore.h"

#include <QTest>

using namespace clipitto;
using namespace clipitto::passwordignore;

class TestPasswordIgnore : public QObject {
    Q_OBJECT

private slots:
    void ignoredShapes_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("shape");
        const auto row = [](const char* text, const char* shape) {
            QTest::newRow(text) << QString::fromUtf8(text) << QString::fromLatin1(shape);
        };
        const char* env = "environment variable";
        const char* path = "file path";
        row("%AppData%\\rig", env);
        row("%appdata%\\rig", env);
        row("%ProgramData%", env);
        row("%ProgramData%\\x", env);
        row("%USERPROFILE%\\Documents", env);
        row("%SystemRoot%", env);
        row("%COMPUTERNAME%", env);
        row("%ProgramFiles(x86)%\\Tool", env);
        row("%TEMP%/a.txt", env);
        row("C:\\x\\%TEMP%\\y", env);
        row("$USER", env);
        row("$HOME/bin", env);
        row("$HOME", env);
        row("$XDG_DATA_HOME/clipitto", env);
        row("${XDG_DATA_HOME}/clipitto", env);
        row("${Home}", env);
        row("$env:APPDATA\\rig", env);
        row("$ENV:AppData", env);
        row("~", env);
        row("~/.config", env);
        row("~\\Documents", env);
        row("~nimiz/work", env);
        row("C:\\Users\\me\\x", path);
        row("d:/work/Project-1/src", path);
        row("C:\\", path);
        row("\\\\srv\\share\\f", path);
        row("\\\\srv\\share", path);
        row("/usr/local/bin", path);
        row("/etc", path);
        row("/home/me/.bashrc", path);
        row("/tmp/aB3$xY9!qLm2", path);
        row("/opt/My Tool/bin", path);
        row("/srv-data/app.v2", path);
        row("./build/out", path);
        row("../Docs/ReadMe.md", path);
        row(".\\scripts\\run.ps1", path);
        row("..\\x", path);
        row("HKEY_LOCAL_MACHINE\\SOFTWARE\\Vendor", "registry key");
        row("HKLM\\Software\\Vendor\\Key", "registry key");
        row("HKCU", "registry key");
        row("localhost", "host and port");
        row("localhost:8080", "host and port");
        row("localhost:8080/api", "host and port");
        row("example.com:443", "host and port");
        row("192.168.1.10:22", "host and port");
        row("com.example.Thing", "dotted name");
        row("System.Collections.Generic", "dotted name");
        row("https://example.com/a?b=C1&d=E2", "URL");
        row("HTTP://Example.COM", "URL");
        row("ftp://host/Dir", "URL");
        row("www.example.com/x", "URL");
        row("mailto:me@example.com", "URL");
        row("me.Name+tag@example.com", "email address");
    }

    void ignoredShapes()
    {
        QFETCH(QString, text);
        QFETCH(QString, shape);
        QCOMPARE(builtinShape(text), shape);
        const Verdict v = check(true, {}, text);
        QVERIFY(v.ignored());
        QCOMPARE(v.kind, Verdict::Kind::Builtin);
        QVERIFY(!check(false, {}, text).ignored()); // the built-in switch
        QVERIFY(!describe(v).contains(text.left(6)) || text.left(6).size() < 3 || shape.contains(text.left(6)));
    }

    // Real-looking generated passwords (some with $ % / \ inside, even at the start) must never be ignored.
    void passwordsStay_data()
    {
        QTest::addColumn<QString>("text");
        const char* passwords[] = {
            "aB3$xY9!qLm2",
            "Tr0ub4dor&3",
            "x7#Kp2$mN9",
            "k9#Vf2!qLm8@Zp",
            "Zq8%wE4&rT1",
            "p@Ss/w0rD!x",
            "nB7\\mK3^vC",
            "G4$h/J9k%L2q",
            "$aB3xY9!qLm2",
            "$Tr0ub4dor&3",
            "$aB3xY9qLm2",
            "$uPq7Zx2Kd",
            "%aB3$xY9",
            "%Tr0ub4dor",
            "%x7#Kp2$mN9%",
            "x%aB3%y",
            "aB3%xY9%/q!2",
            "Pa%ss%/w0rd",
            "/aB3$xY9!qLm2",
            "/Kp2$mN9x",
            "\\aB3$xY9!qLm2",
            "\\\\aB3$xY9!qLm2",
            "~aB3$xY9!qLm2",
            "~Kp2xY9qLm",
            "C:aB3$xY9!q",
            "C|\\aB3xY9",
            "q8W#e7R!t6Y",
            "m!N4b@V5c#X6",
            "Lk9^Jh8&Gf7",
            "7hG$5fD%3sA",
            "wX2/yZ4\\vU6",
            "aB.3$xY9!qLm2",
            "Qw3rty!Uiop9",
            "z$9Xc@2Vb#1N",
            "pA$$w0rd!Zq",
            "aB3$xY9!qLm2@",
            "r5T$y6U&i7O",
            "$HOME2xY9!q",
            "${aB3$xY9",
            "$env;aB3xY9",
            "Zx9$kL@pQ.ab",
            "q:/Xk9#mL2",
            "/Ab3x/Zq7mK",
            "./Xk9#mL2q",
            "../aB3$xY9",
            "~aB3/xY9#q",
            "$XKQMZPLW12",
            "$XKQMZPLWAB",
            "Correct.Horse.Battery",
            "www.Xk9#mL2!",
            "http://user:pa55@host.com",
            "Xk9;%Ab%/q",
        };
        for (const char* p : passwords) {
            QTest::newRow(p) << QString::fromUtf8(p);
        }
    }

    void passwordsStay()
    {
        QFETCH(QString, text);
        QCOMPARE(builtinShape(text), QString());
        QVERIFY(!check(true, {}, text).ignored());
    }

    void multiLineAndHugeAreNotLookedAt()
    {
        QVERIFY(!check(true, {QStringLiteral("*abc*")}, QStringLiteral("%AppData%\\rig\nabc")).ignored());
        QVERIFY(!check(true, {}, QString(2000, QLatin1Char('a'))).ignored());
        QVERIFY(!check(true, {}, QString()).ignored());
    }

    void ruleValidation()
    {
        QVERIFY(validate(QStringLiteral("%appdata%*")).isEmpty());
        QVERIFY(validate(QStringLiteral("*\\Users\\*")).isEmpty());
        QVERIFY(validate(QStringLiteral("C:\\work\\*")).isEmpty());
        QVERIFY(validate(QStringLiteral("a*b*c*d")).isEmpty()); // 3 wildcards
        QVERIFY(!validate(QStringLiteral("a*b*c*d*e")).isEmpty()); // 4
        QVERIFY(!validate(QStringLiteral("*")).isEmpty());
        QVERIFY(!validate(QStringLiteral("**")).isEmpty());
        QVERIFY(!validate(QStringLiteral("*a*")).isEmpty());
        QVERIFY(!validate(QStringLiteral("ab*")).isEmpty());
        QVERIFY(!validate(QStringLiteral("?")).isEmpty());
        QVERIFY(!validate(QStringLiteral("")).isEmpty());
        QVERIFY(!validate(QStringLiteral("   ")).isEmpty());
        QVERIFY(validate(QStringLiteral("abc")).isEmpty());
        QVERIFY(validate(QStringLiteral("  abc  ")).isEmpty());
        QVERIFY(!validate(QStringLiteral("ab\ncd")).isEmpty());
        QVERIFY(!validate(QString(kMaxRuleLength + 1, QLatin1Char('a'))).isEmpty());
        QVERIFY(validate(QString(kMaxRuleLength, QLatin1Char('a'))).isEmpty());
        // The reason never echoes the rule.
        QVERIFY(!validate(QStringLiteral("zq*")).contains(QLatin1String("zq")));
    }

    void ruleMatching()
    {
        QVERIFY(ruleMatches(QStringLiteral("%appdata%*"), QStringLiteral("%AppData%\\rig")));
        QVERIFY(ruleMatches(QStringLiteral("%appdata%*"), QStringLiteral("%APPDATA%")));
        QVERIFY(ruleMatches(QStringLiteral("*\\Users\\*"), QStringLiteral("D:\\data\\users\\Me\\x")));
        QVERIFY(!ruleMatches(QStringLiteral("*\\Users\\*"), QStringLiteral("D:\\data\\Me\\x")));
        QVERIFY(ruleMatches(QStringLiteral("C:\\work\\*"), QStringLiteral("c:\\WORK\\a")));
        QVERIFY(!ruleMatches(QStringLiteral("C:\\work\\*"), QStringLiteral("xC:\\work\\a"))); // whole copy
        QVERIFY(!ruleMatches(QStringLiteral("abc"), QStringLiteral("abcd")));
        QVERIFY(ruleMatches(QStringLiteral("abc"), QStringLiteral("ABC")));
        QVERIFY(ruleMatches(QStringLiteral("a*b*c*d"), QStringLiteral("a--b--c--d")));
        QVERIFY(ruleMatches(QStringLiteral("a*b*c*d"), QStringLiteral("abcd")));
        QVERIFY(!ruleMatches(QStringLiteral("a*b*c*d"), QStringLiteral("a--c--b--d")));
        QVERIFY(ruleMatches(QStringLiteral("  abc*  "), QStringLiteral("abcdef")));
        QVERIFY(ruleMatches(QStringLiteral("a?c*"), QStringLiteral("a?cdef")));  // ? is a plain character
        QVERIFY(!ruleMatches(QStringLiteral("a?c*"), QStringLiteral("abcdef")));
        QVERIFY(!ruleMatches(QStringLiteral("*"), QStringLiteral("anything")));  // invalid rules match nothing
        QVERIFY(!ruleMatches(QStringLiteral("abc*"), QString()));
    }

    void checkOrderAndDescribe()
    {
        const QStringList rules{QStringLiteral("zzz*"), QStringLiteral("%appdata%*")};
        Verdict v = check(true, rules, QStringLiteral("%AppData%\\rig"));
        QCOMPARE(v.kind, Verdict::Kind::Rule); // your rule is named before the built-in shape
        QCOMPARE(v.rule, 1);
        QCOMPARE(describe(v), QStringLiteral("your ignore rule 2"));
        v = check(true, rules, QStringLiteral("C:\\Users\\me"));
        QCOMPARE(describe(v), QStringLiteral("built-in shape: file path"));
        v = check(false, rules, QStringLiteral("C:\\Users\\me"));
        QVERIFY(!v.ignored());
        QVERIFY(describe(v).isEmpty());
        v = check(false, rules, QStringLiteral("ZZZ-1"));
        QCOMPARE(v.rule, 0);
        // Invalid rules are skipped, the numbering stays.
        QCOMPARE(check(false, {QStringLiteral("*"), QStringLiteral("abc*")}, QStringLiteral("abcd")).rule, 1);
    }

    void shapeList()
    {
        const QStringList names = shapeNames();
        QVERIFY(names.contains(QStringLiteral("environment variable")));
        QVERIFY(names.contains(QStringLiteral("file path")));
        QCOMPARE(names.size(), QSet<QString>(names.begin(), names.end()).size());
    }
};

QTEST_APPLESS_MAIN(TestPasswordIgnore)
#include "tst_passwordignore.moc"
