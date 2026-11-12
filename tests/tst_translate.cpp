#include "Translate.h"
#include "ZipReader.h"

#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

using namespace clipitto::translate;

namespace {

quint32 crc32(const QByteArray& data)
{
    quint32 c = 0xFFFFFFFFu;
    for (const char b : data) {
        c ^= quint8(b);
        for (int k = 0; k < 8; ++k) {
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
    }
    return c ^ 0xFFFFFFFFu;
}

void put16(QByteArray& b, quint16 v) { b.append(char(v & 0xFF)).append(char(v >> 8)); }
void put32(QByteArray& b, quint32 v) { put16(b, quint16(v & 0xFFFF)); put16(b, quint16(v >> 16)); }

// Raw deflate of `data`: qCompress writes length + zlib header + deflate + Adler-32.
QByteArray deflate(const QByteArray& data)
{
    const QByteArray z = qCompress(data, 9);
    return z.mid(4 + 2, z.size() - 4 - 2 - 4);
}

struct Item {
    QString name;
    QByteArray data;
    bool deflated;
};

QByteArray makeZip(const QList<Item>& items)
{
    QByteArray out;
    QByteArray dir;
    for (const Item& it : items) {
        const QByteArray packed = it.deflated ? deflate(it.data) : it.data;
        const QByteArray name = it.name.toUtf8();
        const quint32 offset = quint32(out.size());
        put32(out, 0x04034b50u); put16(out, 20); put16(out, 0); put16(out, it.deflated ? 8 : 0);
        put16(out, 0); put16(out, 0); put32(out, crc32(it.data));
        put32(out, quint32(packed.size())); put32(out, quint32(it.data.size()));
        put16(out, quint16(name.size())); put16(out, 0);
        out += name; out += packed;
        put32(dir, 0x02014b50u); put16(dir, 20); put16(dir, 20); put16(dir, 0); put16(dir, it.deflated ? 8 : 0);
        put16(dir, 0); put16(dir, 0); put32(dir, crc32(it.data));
        put32(dir, quint32(packed.size())); put32(dir, quint32(it.data.size()));
        put16(dir, quint16(name.size())); put16(dir, 0); put16(dir, 0); put16(dir, 0); put16(dir, 0);
        put32(dir, 0); put32(dir, offset);
        dir += name;
    }
    const quint32 dirOffset = quint32(out.size());
    out += dir;
    put32(out, 0x06054b50u); put16(out, 0); put16(out, 0); put16(out, quint16(items.size()));
    put16(out, quint16(items.size())); put32(out, quint32(dir.size())); put32(out, dirOffset); put16(out, 0);
    return out;
}

QByteArray bigText()
{
    QByteArray t;
    for (int i = 0; i < 400; ++i) {
        t += "Sentence number " + QByteArray::number(i) + " of a text that repeats itself, with some variety " +
             QByteArray::number(i * 7919 % 1000) + ".\n";
    }
    return t;
}

QByteArray packZip(const QString& from = QStringLiteral("zh"), const QString& to = QStringLiteral("en"))
{
    const QString top = QStringLiteral("translate-%1_%2-1_9").arg(from, to);
    QJsonObject meta{{"package_version", "1.9"}, {"from_code", from}, {"from_name", "Chinese"},
                     {"to_code", to}, {"to_name", "English"}};
    return makeZip({
        {top + "/metadata.json", QJsonDocument(meta).toJson(), true},
        {top + "/README.md", "readme", false},
        {top + "/sentencepiece.model", QByteArray(5000, 'S'), true},
        {top + "/model/model.bin", bigText(), true},
        {top + "/model/config.json", "{}", false},
        {top + "/stanza/zh-hans/tokenize/gsdsimp.pt", "not wanted", true},
        {"../evil.txt", "never extracted", false},
    });
}

} // namespace

class TestTranslate : public QObject {
    Q_OBJECT

private slots:
    void inflateReadsRealDeflateStreams()
    {
        for (const QByteArray& data : {QByteArray("a"), QByteArray(10000, 'x'), bigText(),
                                       QByteArray::fromHex("00ff00ff10203040")}) {
            QByteArray out;
            QVERIFY(inflateRaw(deflate(data), &out, 1 << 24));
            QCOMPARE(out, data);
        }
        QByteArray out;
        QVERIFY(!inflateRaw(QByteArray("\xff\xff\xff\xff", 4), &out, 1 << 20)); // garbage
        QVERIFY(!inflateRaw(deflate(QByteArray(10000, 'x')), &out, 100)); // over the size limit
    }

    void zipReaderReadsStoredAndDeflatedEntriesAndChecksThem()
    {
        QTemporaryDir dir;
        QFile f(dir.filePath("a.zip"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(makeZip({{"one.txt", "hello", false}, {"dir/two.txt", bigText(), true}}));
        f.close();
        ZipReader zip;
        QString error;
        QVERIFY2(zip.open(f.fileName(), &error), qPrintable(error));
        QCOMPARE(zip.names(), (QStringList{"one.txt", "dir/two.txt"}));
        QCOMPARE(zip.read("one.txt"), QByteArray("hello"));
        QCOMPARE(zip.read("dir/two.txt"), bigText());
        QCOMPARE(zip.sizeOf("dir/two.txt"), qint64(bigText().size()));
        QCOMPARE(zip.sizeOf("missing"), qint64(-1));
        QVERIFY(zip.read("missing", &error).isEmpty());
        // A damaged entry fails its checksum.
        QByteArray bytes = makeZip({{"x.txt", "hello world", false}});
        bytes[bytes.indexOf("hello world")] = 'j';
        QFile bad(dir.filePath("bad.zip"));
        QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write(bytes);
        bad.close();
        ZipReader damaged;
        QVERIFY(damaged.open(bad.fileName()));
        QVERIFY(damaged.read("x.txt", &error).isEmpty());
        QVERIFY(!error.isEmpty());
        // Not a zip at all.
        QFile notZip(dir.filePath("n.zip"));
        QVERIFY(notZip.open(QIODevice::WriteOnly));
        notZip.write("just text");
        notZip.close();
        ZipReader none;
        QVERIFY(!none.open(notZip.fileName(), &error));
    }

    void aPackInstallsListsAndIsRemoved()
    {
        QTemporaryDir dir;
        QFile f(dir.filePath("pack.argosmodel"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(packZip());
        f.close();
        PackStore store(dir.filePath("store"));
        QVERIFY(store.installed().isEmpty());
        QString error;
        QVERIFY2(store.install(f.fileName(), &error), qPrintable(error));
        const QList<Pack> packs = store.installed();
        QCOMPARE(packs.size(), 1);
        QCOMPARE(packs.first().id, QStringLiteral("zh-en"));
        QCOMPARE(packs.first().label(), QStringLiteral("Chinese → English"));
        QVERIFY(QFile::exists(packs.first().path + "/model/model.bin"));
        QCOMPARE(QFile(packs.first().path + "/model/model.bin").size(), qint64(bigText().size()));
        QVERIFY(!QFile::exists(packs.first().path + "/stanza")); // only what the engine reads
        QVERIFY(!QFile::exists(dir.filePath("evil.txt")));
        QVERIFY(store.install(f.fileName(), &error)); // again: replaces it
        QCOMPARE(store.installed().size(), 1);
        QVERIFY(!store.remove("../store"));
        QVERIFY(store.remove("zh-en"));
        QVERIFY(store.installed().isEmpty());
    }

    void notAPackIsRefused()
    {
        QTemporaryDir dir;
        PackStore store(dir.filePath("store"));
        QFile f(dir.filePath("other.zip"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(makeZip({{"folder/readme.txt", "hi", false}}));
        f.close();
        QString error;
        QVERIFY(!store.install(f.fileName(), &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!store.install(dir.filePath("missing.zip"), &error));
        QVERIFY(store.installed().isEmpty());
    }

    void scriptsAndWhichPackFits()
    {
        QCOMPARE(scriptOfText(QStringLiteral("你好，世界")), QStringLiteral("han"));
        QCOMPARE(scriptOfText(QStringLiteral("こんにちは 世界")), QStringLiteral("kana"));
        QCOMPARE(scriptOfText(QStringLiteral("שלום עולם")), QStringLiteral("hebrew"));
        QCOMPARE(scriptOfText(QStringLiteral("Hello, world 123")), QStringLiteral("latin"));
        QCOMPARE(scriptOfText(QStringLiteral("Привет")), QStringLiteral("cyrillic"));
        QCOMPARE(scriptOfText(QStringLiteral("123 !?")), QString());
        QCOMPARE(scriptOfLanguage(QStringLiteral("zh")), QStringLiteral("han"));
        QCOMPARE(scriptOfLanguage(QStringLiteral("de")), QStringLiteral("latin"));
        Pack zh;
        zh.from = "zh";
        Pack en;
        en.from = "en";
        Pack he;
        he.from = "he";
        QVERIFY(fits(zh, QStringLiteral("这是一个测试")));
        QVERIFY(!fits(en, QStringLiteral("这是一个测试")));
        QVERIFY(fits(en, QStringLiteral("This is a test")));
        QVERIFY(fits(he, QStringLiteral("שלום")));
        QVERIFY(!fits(he, QStringLiteral("123")));
    }

    void sentencesAreSplitAndSeparatorsKept()
    {
        const auto rejoin = [](const QList<Piece>& pieces) {
            QString s;
            for (const Piece& p : pieces) {
                s += p.text + p.after;
            }
            return s;
        };
        const QStringList samples = {
            QStringLiteral("Hello world. How are you? Fine!\nSecond line\r\n\r\nThird."),
            QStringLiteral("这是第一句。这是第二句！第三句？\n新的一行"),
            QStringLiteral("Pi is 3.14 and v1.2.3 is out. Done."),
            QStringLiteral("no stop at all"),
            QStringLiteral("\n\n"),
            QStringLiteral(""),
        };
        for (const QString& s : samples) {
            QCOMPARE(rejoin(splitSentences(s)), s); // nothing lost, nothing added
        }
        const QList<Piece> english = splitSentences(QStringLiteral("Hello world. How are you? Fine!\nNext"));
        QCOMPARE(english.size(), 4);
        QCOMPARE(english[0].text, QStringLiteral("Hello world."));
        QCOMPARE(english[2].text, QStringLiteral("Fine!"));
        QCOMPARE(english[2].after, QStringLiteral("\n"));
        QCOMPARE(splitSentences(QStringLiteral("Pi is 3.14 and v1.2.3 is out. Done.")).size(), 2);
        const QList<Piece> chinese = splitSentences(QStringLiteral("这是第一句。这是第二句！"));
        QCOMPARE(chinese.size(), 2);
        QCOMPARE(chinese[0].text, QStringLiteral("这是第一句。"));
    }

    // ---- scripts and pack choice ----

    void everyScriptIsRecognisedAndAMostlyDigitTextFollowsItsLetters()
    {
        const auto script = [](const char* utf8) { return scriptOfText(QString::fromUtf8(utf8)); };
        QCOMPARE(script("Hello"), QStringLiteral("latin"));
        QCOMPARE(script("Ünïcödé façade"), QStringLiteral("latin"));
        QCOMPARE(script("שלום"), QStringLiteral("hebrew"));
        QCOMPARE(script("מכשול"), QStringLiteral("hebrew"));
        QCOMPARE(script("מְכַשּׁוֹל"), QStringLiteral("hebrew")); // with vowel points
        QCOMPARE(script("Привет, мир"), QStringLiteral("cyrillic"));
        QCOMPARE(script("Γειά σου"), QStringLiteral("greek"));
        QCOMPARE(script("你好"), QStringLiteral("han"));
        QCOMPARE(script("こんにちは"), QStringLiteral("kana"));
        QCOMPARE(script("東京にいます"), QStringLiteral("kana")); // kana decides over the kanji
        QCOMPARE(script("안녕하세요"), QStringLiteral("hangul"));
        QCOMPARE(script("مرحبا"), QStringLiteral("arabic"));
        QCOMPARE(script("नमस्ते"), QStringLiteral("devanagari"));
        QCOMPARE(script("1234567890 12:30 #4711 3.14"), QString()); // no letters: no script, so no pack fits
        QCOMPARE(script("12345 67890 11 22 33 44 55 שלום"), QStringLiteral("hebrew")); // digits don't count
        QCOMPARE(script("שלום hello world today"), QStringLiteral("latin")); // the majority of letters
        QCOMPARE(script("שלום עולם hello"), QStringLiteral("hebrew"));
        QCOMPARE(script(""), QString());

        Pack he;
        he.from = QStringLiteral("he");
        Pack en;
        en.from = QStringLiteral("en");
        Pack ru;
        ru.from = QStringLiteral("ru");
        Pack zt;
        zt.from = QStringLiteral("zt"); // traditional Chinese
        Pack pt;
        pt.from = QStringLiteral("pt-BR");
        QVERIFY(fits(he, QString::fromUtf8("מכשול")));
        QVERIFY(!fits(en, QString::fromUtf8("מכשול")));
        QVERIFY(!fits(he, QStringLiteral("hello")));
        QVERIFY(fits(ru, QString::fromUtf8("Привет")));
        QVERIFY(fits(zt, QString::fromUtf8("你好")));
        QVERIFY(fits(pt, QStringLiteral("olá")));
        QVERIFY(!fits(he, QStringLiteral("42")));
        QVERIFY(!fits(en, QStringLiteral("42")));
        QVERIFY(!fits(en, QString()));
    }

    // ---- what happens to a text around the engine (the same steps Translate_ct2.cpp takes) ----

    // Stands in for the model: marks a translated piece with <...>.
    static QString throughAFakeModel(const QString& text, QStringList* sent = nullptr)
    {
        const QList<Piece> pieces = splitSentences(text);
        QStringList translated(pieces.size());
        for (qsizetype i = 0; i < pieces.size(); ++i) {
            if (worthTranslating(pieces[i].text)) {
                if (sent) {
                    *sent << pieces[i].text;
                }
                translated[i] = QStringLiteral("<") + pieces[i].text.simplified() + QStringLiteral(">");
            }
        }
        return assemble(pieces, translated);
    }

    void numbersIdsAndLinksAreLeftAlone()
    {
        for (const char* kept : {"12345", "4711 0815", "12:30", "#4711", "3.14", "---", "(1) (2)", "https://example.com/a?b=1",
                                 "www.example.com", "someone@example.com", "", "   "}) {
            QVERIFY2(!worthTranslating(QString::fromUtf8(kept)), kept);
            QCOMPARE(throughAFakeModel(QString::fromUtf8(kept)), QString::fromUtf8(kept));
        }
        QVERIFY(worthTranslating(QString::fromUtf8("מכשול")));
        QVERIFY(worthTranslating(QStringLiteral("a")));
        QVERIFY(worthTranslating(QStringLiteral("Order 4711 is late")));
        QVERIFY(worthTranslating(QStringLiteral("see https://example.com now"))); // a sentence with a link is a sentence
        QStringList sent;
        QCOMPARE(throughAFakeModel(QStringLiteral("Invoice\n4711\n2024-05-01\nTotal due"), &sent),
                 QStringLiteral("<Invoice>\n4711\n2024-05-01\n<Total due>"));
        QCOMPARE(sent, (QStringList{QStringLiteral("Invoice"), QStringLiteral("Total due")}));
    }

    void whitespaceAndLineBreaksSurviveTranslation()
    {
        QCOMPARE(throughAFakeModel(QStringLiteral("  hello  ")), QStringLiteral("  <hello>  "));
        QCOMPARE(throughAFakeModel(QStringLiteral("\n\nhello\n")), QStringLiteral("\n\n<hello>\n"));
        QCOMPARE(throughAFakeModel(QStringLiteral("    indented code line")), QStringLiteral("    <indented code line>"));
        QCOMPARE(throughAFakeModel(QStringLiteral("one\n\ntwo\n\nthree")), QStringLiteral("<one>\n\n<two>\n\n<three>"));
        QCOMPARE(throughAFakeModel(QStringLiteral("one\r\ntwo\r\n")), QStringLiteral("<one>\r\n<two>\r\n"));
        QCOMPARE(throughAFakeModel(QStringLiteral("Hi. Bye!  Ok?")), QStringLiteral("<Hi.> <Bye!>  <Ok?>"));
        QCOMPARE(throughAFakeModel(QStringLiteral("He said \"stop.\" Then (left).")), QStringLiteral("<He said \"stop.\"> <Then (left).>"));
        QCOMPARE(throughAFakeModel(QString::fromUtf8("שלום. מה שלומך?\nטוב")), QString::fromUtf8("<שלום.> <מה שלומך?>\n<טוב>"));
        QCOMPARE(throughAFakeModel(QStringLiteral("x")), QStringLiteral("<x>")); // one character
        QCOMPARE(throughAFakeModel(QString()), QString());
    }

    void aPieceTheModelAnswersNothingForStaysAsItWas()
    {
        const QList<Piece> pieces = splitSentences(QStringLiteral("  one.  two."));
        QCOMPARE(assemble(pieces, QStringList{QString(), QStringLiteral("2.")}), QStringLiteral("  one.  2."));
        QCOMPARE(assemble(pieces, QStringList{}), QStringLiteral("  one.  two."));
    }

    // ---- the language-pack store with fake files ----

    static QString writeFile(const QTemporaryDir& dir, const QString& name, const QByteArray& bytes)
    {
        QFile f(dir.filePath(name));
        if (!f.open(QIODevice::WriteOnly)) {
            return {};
        }
        f.write(bytes);
        f.close(); // closed before anything is deleted (Windows)
        return f.fileName();
    }

    static QByteArray packWith(const QString& from, const QString& to, const QByteArray& metadata, bool model = true, bool sp = true,
                               const QString& top = QStringLiteral("pack"))
    {
        QList<Item> items = {{top + "/metadata.json", metadata, true}};
        if (sp) {
            items.push_back({top + "/sentencepiece.model", QByteArray(100, 'S'), true});
        }
        if (model) {
            items.push_back({top + "/model/model.bin", QByteArray(100, 'M'), true});
        }
        Q_UNUSED(from)
        Q_UNUSED(to)
        return makeZip(items);
    }

    static QByteArray meta(const QString& from, const QString& to, const QString& fromName = QString(), const QString& toName = QString())
    {
        QJsonObject o{{"from_code", from}, {"to_code", to}};
        if (!fromName.isEmpty()) {
            o["from_name"] = fromName;
        }
        if (!toName.isEmpty()) {
            o["to_name"] = toName;
        }
        return QJsonDocument(o).toJson();
    }

    void packMetadataIsParsedIntoIdLabelAndLanguages()
    {
        QTemporaryDir dir;
        PackStore store(dir.filePath("store"));
        QString error;
        QVERIFY2(store.install(writeFile(dir, "he.argosmodel", packWith("he", "en", meta("he", "en", "Hebrew", "English"))), &error), qPrintable(error));
        const QList<Pack> packs = store.installed();
        QCOMPARE(packs.size(), 1);
        QCOMPARE(packs[0].id, QStringLiteral("he-en"));
        QCOMPARE(packs[0].from, QStringLiteral("he"));
        QCOMPARE(packs[0].to, QStringLiteral("en"));
        QCOMPARE(packs[0].label(), QStringLiteral("Hebrew → English"));
        // No names in the metadata: the codes are shown.
        QVERIFY2(store.install(writeFile(dir, "ru.argosmodel", packWith("ru", "en", meta("ru", "en"))), &error), qPrintable(error));
        QCOMPARE(store.installed().size(), 2);
        QCOMPARE(store.installed()[1].label(), QStringLiteral("ru → en"));
        // Listed in a steady order (by id), whatever the install order was.
        QCOMPARE(store.installed()[0].id, QStringLiteral("he-en"));
    }

    void badPackFilesAreRefusedWithAReasonAndLeaveTheStoreAlone()
    {
        QTemporaryDir dir;
        PackStore store(dir.filePath("store"));
        QString error;
        QVERIFY2(store.install(writeFile(dir, "good.argosmodel", packWith("he", "en", meta("he", "en", "Hebrew", "English"))), &error), qPrintable(error));
        const QByteArray good = packWith("he", "en", meta("he", "en", "Hebrew", "English"));
        const QList<QPair<QString, QByteArray>> bad = {
            {"garbage", QByteArray("this is not a zip file at all")},
            {"empty", QByteArray()},
            {"truncated", good.left(good.size() / 2)},
            {"notjson", packWith("he", "en", "{ not json")},
            {"nocodes", packWith("he", "en", QByteArray("{}"))},
            {"badcode", packWith("he", "en", meta("../x", "en"))},
            {"badcode2", packWith("he", "en", meta("he", "english-language"))},
            {"nomodel", packWith("he", "en", meta("he", "en"), /*model=*/false)},
            {"nospm", packWith("he", "en", meta("he", "en"), true, /*sp=*/false)},
            {"deep", makeZip({{"a/b/metadata.json", meta("he", "en"), true}})},
        };
        for (const auto& b : bad) {
            error.clear();
            QVERIFY2(!store.install(writeFile(dir, b.first + ".argosmodel", b.second), &error), qPrintable(b.first));
            QVERIFY2(!error.isEmpty(), qPrintable(b.first));
        }
        // The pack that was there is still there and whole.
        const QList<Pack> packs = store.installed();
        QCOMPARE(packs.size(), 1);
        QVERIFY(QFile::exists(packs[0].path + "/model/model.bin"));
        QVERIFY(!QDir(dir.filePath("store")).entryList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).contains(QStringLiteral(".installing-he-en")));
    }

    void aSecondPackOfTheSameDirectionReplacesTheFirst()
    {
        QTemporaryDir dir;
        PackStore store(dir.filePath("store"));
        QString error;
        QVERIFY(store.install(writeFile(dir, "v1.argosmodel", packWith("he", "en", meta("he", "en", "Hebrew", "English"))), &error));
        QVERIFY(store.install(writeFile(dir, "v2.argosmodel", packWith("he", "en", meta("he", "en", "Ivrit", "English"), true, true, "other-top")), &error));
        const QList<Pack> packs = store.installed();
        QCOMPARE(packs.size(), 1); // not two
        QCOMPARE(packs[0].label(), QStringLiteral("Ivrit → English"));
        // Both directions of a language are two packs.
        QVERIFY(store.install(writeFile(dir, "back.argosmodel", packWith("en", "he", meta("en", "he", "English", "Hebrew"))), &error));
        QCOMPARE(store.installed().size(), 2);
    }

    void aDamagedFolderInTheStoreIsNotListed()
    {
        QTemporaryDir dir;
        PackStore store(dir.filePath("store"));
        QString error;
        QVERIFY(store.install(writeFile(dir, "he.argosmodel", packWith("he", "en", meta("he", "en"))), &error));
        const QString he = store.installed()[0].path;
        QVERIFY(QDir().mkpath(dir.filePath("store/zz-en"))); // a folder without a pack in it
        QVERIFY(QDir().mkpath(dir.filePath("store/.installing-ru-en"))); // an install that was cut off
        writeFile(dir, "store/zz-en/metadata.json", "garbage");
        QCOMPARE(store.installed().size(), 1);
        QVERIFY(QFile::remove(he + "/model/model.bin")); // the model file went missing
        QVERIFY(store.installed().isEmpty());
        QVERIFY(store.remove(QStringLiteral("he-en"))); // and can still be removed
        QVERIFY(!store.remove(QStringLiteral("nonsense")));
        QVERIFY(!store.remove(QString()));
        QVERIFY(!store.remove(QStringLiteral("he-en/../../x")));
    }

    // With a real pack (the path in CLIPITTO_TEST_PACK, a Chinese→English .argosmodel) and an engine in the build.
    void translatesWithARealPack()
    {
        const QString file = qEnvironmentVariable("CLIPITTO_TEST_PACK");
        if (file.isEmpty() || !builtIn()) {
            QSKIP("set CLIPITTO_TEST_PACK to a zh->en .argosmodel and build with the translation libraries");
        }
        QTemporaryDir dir;
        PackStore store(dir.path());
        QString error;
        QVERIFY2(store.install(file, &error), qPrintable(error));
        const Pack pack = store.installed().first();
        const auto out = translate(pack, QStringLiteral("我爱你。\n今天天气很好，我们去公园散步吧。"), &error);
        QVERIFY2(out.has_value(), qPrintable(error));
        QVERIFY2(out->contains("love", Qt::CaseInsensitive), qPrintable(*out));
        QVERIFY2(out->contains("park", Qt::CaseInsensitive), qPrintable(*out));
        QVERIFY(out->contains('\n')); // the line break survived
        QVERIFY(!out->contains(QChar(0x2581)));
    }

    // With an English -> Hebrew pack (CLIPITTO_TEST_PACK_EN_HE): a single word is not said twice ("hello" used to give
    // "שלום שלום" because the model was trained on sentences).
    void aSingleWordIsNotRepeated()
    {
        const QString file = qEnvironmentVariable("CLIPITTO_TEST_PACK_EN_HE");
        if (file.isEmpty() || !builtIn()) {
            QSKIP("set CLIPITTO_TEST_PACK_EN_HE to an en->he .argosmodel and build with the translation libraries");
        }
        QTemporaryDir dir;
        PackStore store(dir.path());
        QString error;
        QVERIFY2(store.install(file, &error), qPrintable(error));
        const Pack pack = store.installed().first();
        for (const QString& word : {QStringLiteral("hello"), QStringLiteral("Hello"), QStringLiteral("hello ")}) {
            const auto out = translate(pack, word, &error);
            QVERIFY2(out.has_value(), qPrintable(error));
            const QStringList words = out->split(QLatin1Char(' '), Qt::SkipEmptyParts);
            QVERIFY2(!words.isEmpty(), qPrintable(word));
            for (qsizetype i = 1; i < words.size(); ++i) {
                QVERIFY2(words[i] != words[i - 1], qPrintable(word + " -> " + *out));
            }
        }
        // A sentence is still translated as a whole.
        const auto sentence = translate(pack, QStringLiteral("Hello, how are you today?"), &error);
        QVERIFY(sentence.has_value());
        QVERIFY2(sentence->contains(QStringLiteral("איך")), qPrintable(*sentence));
    }

    // For diagnosing a text that "gets stuck": CLIPITTO_TEST_PACK_ANY is any pack, CLIPITTO_TEST_TEXT the text. It
    // must come back within a few seconds.
    void aTextFromTheEnvironmentTranslatesInTime()
    {
        const QString file = qEnvironmentVariable("CLIPITTO_TEST_PACK_ANY");
        const QString text = qEnvironmentVariable("CLIPITTO_TEST_TEXT");
        if (file.isEmpty() || text.isEmpty() || !builtIn()) {
            QSKIP("set CLIPITTO_TEST_PACK_ANY and CLIPITTO_TEST_TEXT");
        }
        QTemporaryDir dir;
        PackStore store(dir.path());
        QString error;
        QVERIFY2(store.install(file, &error), qPrintable(error));
        QElapsedTimer timer;
        timer.start();
        const auto out = translate(store.installed().first(), text, &error);
        qInfo().noquote() << "translated in" << timer.elapsed() << "ms:" << out.value_or(error);
        QVERIFY2(out.has_value(), qPrintable(error));
        QVERIFY2(timer.elapsed() < 10000, "took too long");
    }
};

QTEST_GUILESS_MAIN(TestTranslate)
#include "tst_translate.moc"
