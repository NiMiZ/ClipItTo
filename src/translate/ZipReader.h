#pragma once

#include <QFile>
#include <QString>
#include <QStringList>

namespace clipitto::translate {

// A small reader for ordinary zip files (stored and deflated entries, no encryption, no zip64): all that a
// language pack needs. Entries are checked against their CRC-32. Raw deflate (RFC 1951) is decoded here, so
// no zip library is needed.
class ZipReader {
public:
    bool open(const QString& path, QString* error = nullptr);
    QStringList names() const;
    // The uncompressed size of an entry, -1 when there is none of that name.
    qint64 sizeOf(const QString& name) const;
    // Extracts `name` into the file `destination`. `maxSize` caps the uncompressed size.
    bool extract(const QString& name, const QString& destination, QString* error = nullptr);
    // Extracts a small entry into memory.
    QByteArray read(const QString& name, QString* error = nullptr, qint64 maxSize = 1 << 20);

private:
    struct Entry {
        QString name;
        quint16 method = 0;
        quint32 crc = 0;
        quint64 compressedSize = 0;
        quint64 size = 0;
        quint64 headerOffset = 0;
    };
    const Entry* find(const QString& name) const;
    bool load(const Entry& e, QByteArray* out, qint64 maxSize, QString* error);

    QFile m_file;
    QList<Entry> m_entries;
};

// Raw deflate decoder: `in` is a deflate stream, the output is appended to `out`. False for corrupt data or
// when the output would pass `limit` bytes.
bool inflateRaw(const QByteArray& in, QByteArray* out, qint64 limit);

} // namespace clipitto::translate
