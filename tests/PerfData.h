#pragma once

// Test data for the perf tests: a big, varied history without a clipboard in sight.
// Texts are synthetic; nothing here is ever printed.

#include "ImageClip.h"
#include "db/Repository.h"
#include "db/Sqlite.h"

#include <QBuffer>
#include <QByteArray>
#include <QFile>
#include <QImage>
#include <QRandomGenerator>
#include <QString>

#include <cstdio>

namespace perf {

// A line of text with a few words from a small vocabulary and a number, so search has something to match and the
// copies are all different.
inline QString sampleText(int i)
{
    static const char* words[] = {"kubectl", "deploy", "SELECT", "users", "docker", "compose", "git", "commit", "lorem",
                                  "ipsum", "function", "return", "const", "ssh", "server", "config", "report", "invoice"};
    QRandomGenerator rng(quint32(i) * 2654435761u + 7u);
    QStringList line;
    const int n = 6 + int(rng.bounded(30));
    for (int w = 0; w < n; ++w) {
        line << QString::fromLatin1(words[rng.bounded(18)]);
    }
    line << QStringLiteral("n%1").arg(i);
    return line.join(QLatin1Char(' '));
}

// Adds copies [from, to) in one transaction.
inline void fill(clipitto::db::Db& db, clipitto::db::Repository& repo, int from, int to)
{
    clipitto::db::Transaction tx(db);
    for (int i = from; i < to; ++i) {
        repo.ingest(sampleText(i));
    }
    tx.commit();
}

inline clipitto::ImageClip fakePicture(int n, int w = 800, int h = 600)
{
    QImage thumb(320, 180, QImage::Format_RGB32);
    thumb.fill(QColor::fromHsv(n * 37 % 360, 200, 200));
    QByteArray thumbPng;
    QBuffer buf(&thumbPng);
    buf.open(QIODevice::WriteOnly);
    thumb.save(&buf, "PNG");
    clipitto::ImageClip clip;
    clip.png = QByteArray("PNG-") + QByteArray::number(n) + QByteArray(64, 'x');
    clip.thumbnail = thumbPng;
    clip.width = w;
    clip.height = h;
    clip.pixelHash = QStringLiteral("perfhash%1").arg(n);
    return clip;
}

// Random-looking pixels (do not compress): a picture of about w*h*3 bytes as PNG.
inline QImage noisyImage(int w, int h)
{
    QImage image(w, h, QImage::Format_RGB32);
    QRandomGenerator rng(12345);
    for (int y = 0; y < h; ++y) {
        auto* line = reinterpret_cast<quint32*>(image.scanLine(y));
        for (int x = 0; x < w; ++x) {
            line[x] = rng.generate() | 0xFF000000u;
        }
    }
    return image;
}

// Resident memory of this process in KiB (Linux; 0 elsewhere).
// True in an AddressSanitizer / UndefinedBehaviorSanitizer / ThreadSanitizer build.
inline bool underSanitizer()
{
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
    return true;
#else
    return false;
#endif
#else
    return false;
#endif
}

inline qint64 residentKb()
{
#ifdef Q_OS_LINUX
    QFile f(QStringLiteral("/proc/self/statm"));
    if (f.open(QIODevice::ReadOnly)) {
        const QList<QByteArray> parts = f.readAll().split(' ');
        if (parts.size() > 1) {
            return parts.at(1).toLongLong() * 4;
        }
    }
#endif
    return 0;
}

// CPU time (user+system) of this process in ms (Linux; 0 elsewhere).
inline qint64 cpuMs()
{
#ifdef Q_OS_LINUX
    QFile f(QStringLiteral("/proc/self/stat"));
    if (f.open(QIODevice::ReadOnly)) {
        const QByteArray all = f.readAll();
        const QList<QByteArray> parts = all.mid(all.lastIndexOf(')') + 2).split(' ');
        if (parts.size() > 13) {
            return (parts.at(11).toLongLong() + parts.at(12).toLongLong()) * 10; // clock ticks of 10 ms
        }
    }
#endif
    return 0;
}

inline void note(const char* what, const QString& value)
{
    std::fprintf(stderr, "PERF note  %-44s %s\n", what, qPrintable(value));
}

} // namespace perf
