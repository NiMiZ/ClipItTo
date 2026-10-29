#include "ZipReader.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>

#include <array>
#include <cstdint>

namespace clipitto::translate {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("clipitto::translate", text);
}

bool fail(QString* error, const QString& why)
{
    if (error) {
        *error = why;
    }
    return false;
}

// --- CRC-32 -------------------------------------------------------------------------------------------

const std::array<quint32, 256>& crcTable()
{
    static const std::array<quint32, 256> table = [] {
        std::array<quint32, 256> t{};
        for (quint32 n = 0; n < 256; ++n) {
            quint32 c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            t[n] = c;
        }
        return t;
    }();
    return table;
}

quint32 crc32(const QByteArray& data)
{
    quint32 c = 0xFFFFFFFFu;
    const auto& t = crcTable();
    for (const char b : data) {
        c = t[(c ^ quint8(b)) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

// --- Inflate (RFC 1951), after the structure of zlib's puff.c ------------------------------------------

constexpr int kMaxBits = 15;
constexpr int kMaxLitCodes = 286;
constexpr int kMaxDistCodes = 30;
constexpr int kFixedLitCodes = 288;

struct Huffman {
    std::array<int16_t, kMaxBits + 1> count{};
    std::array<int16_t, kFixedLitCodes> symbol{};
};

struct Stream {
    const quint8* in;
    qint64 inLen;
    qint64 inPos = 0;
    quint32 bitBuf = 0;
    int bitCnt = 0;
    QByteArray* out;
    qint64 limit;
    bool bad = false;

    int bits(int need)
    {
        quint32 val = bitBuf;
        while (bitCnt < need) {
            if (inPos >= inLen) {
                bad = true;
                return 0;
            }
            val |= quint32(in[inPos++]) << bitCnt;
            bitCnt += 8;
        }
        bitBuf = val >> need;
        bitCnt -= need;
        return int(val & ((1u << need) - 1));
    }
};

// Builds the decoding tables for code lengths `length[0..n)`. Negative: over-subscribed; positive: incomplete.
int construct(Huffman& h, const int16_t* length, int n)
{
    h.count.fill(0);
    for (int s = 0; s < n; ++s) {
        ++h.count[length[s]];
    }
    if (h.count[0] == n) {
        return 0;
    }
    int left = 1;
    for (int len = 1; len <= kMaxBits; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) {
            return left;
        }
    }
    std::array<int16_t, kMaxBits + 1> offs{};
    for (int len = 1; len < kMaxBits; ++len) {
        offs[len + 1] = offs[len] + h.count[len];
    }
    for (int s = 0; s < n; ++s) {
        if (length[s] != 0) {
            h.symbol[offs[length[s]]++] = int16_t(s);
        }
    }
    return left;
}

int decode(Stream& s, const Huffman& h)
{
    int code = 0;
    int first = 0;
    int index = 0;
    for (int len = 1; len <= kMaxBits; ++len) {
        code |= s.bits(1);
        if (s.bad) {
            return -1;
        }
        const int count = h.count[len];
        if (code - count < first) {
            return h.symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

bool codes(Stream& s, const Huffman& lencode, const Huffman& distcode)
{
    static const int16_t lens[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                     35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const int16_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                     3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const int16_t dists[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                      257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                      8193, 12289, 16385, 24577};
    static const int16_t dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                     7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    int symbol;
    do {
        symbol = decode(s, lencode);
        if (symbol < 0) {
            return false;
        }
        if (symbol < 256) {
            if (s.out->size() >= s.limit) {
                return false;
            }
            s.out->append(char(symbol));
        } else if (symbol > 256) {
            symbol -= 257;
            if (symbol >= 29) {
                return false;
            }
            const int len = lens[symbol] + s.bits(lext[symbol]);
            const int dsym = decode(s, distcode);
            if (dsym < 0 || dsym >= 30) {
                return false;
            }
            const qint64 dist = dists[dsym] + s.bits(dext[dsym]);
            if (s.bad || dist > s.out->size() || s.out->size() + len > s.limit) {
                return false;
            }
            qint64 from = s.out->size() - dist;
            for (int i = 0; i < len; ++i) {
                s.out->append((*s.out)[from + i]); // may overlap itself: byte by byte on purpose
            }
        }
    } while (symbol != 256);
    return true;
}

bool fixedBlock(Stream& s)
{
    static Huffman lencode;
    static Huffman distcode;
    static bool built = false;
    if (!built) {
        int16_t lengths[kFixedLitCodes];
        int sym = 0;
        for (; sym < 144; ++sym) lengths[sym] = 8;
        for (; sym < 256; ++sym) lengths[sym] = 9;
        for (; sym < 280; ++sym) lengths[sym] = 7;
        for (; sym < kFixedLitCodes; ++sym) lengths[sym] = 8;
        construct(lencode, lengths, kFixedLitCodes);
        for (sym = 0; sym < kMaxDistCodes; ++sym) lengths[sym] = 5;
        construct(distcode, lengths, kMaxDistCodes);
        built = true;
    }
    return codes(s, lencode, distcode);
}

bool dynamicBlock(Stream& s)
{
    static const int16_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    const int nlen = s.bits(5) + 257;
    const int ndist = s.bits(5) + 1;
    const int ncode = s.bits(4) + 4;
    if (s.bad || nlen > kMaxLitCodes || ndist > kMaxDistCodes) {
        return false;
    }
    int16_t lengths[kMaxLitCodes + kMaxDistCodes];
    int index = 0;
    for (; index < ncode; ++index) {
        lengths[order[index]] = int16_t(s.bits(3));
    }
    for (; index < 19; ++index) {
        lengths[order[index]] = 0;
    }
    Huffman lencode;
    if (construct(lencode, lengths, 19) != 0) {
        return false;
    }
    index = 0;
    while (index < nlen + ndist) {
        int symbol = decode(s, lencode);
        if (symbol < 0) {
            return false;
        }
        if (symbol < 16) {
            lengths[index++] = int16_t(symbol);
        } else {
            int16_t len = 0;
            int rep;
            if (symbol == 16) {
                if (index == 0) {
                    return false;
                }
                len = lengths[index - 1];
                rep = 3 + s.bits(2);
            } else if (symbol == 17) {
                rep = 3 + s.bits(3);
            } else {
                rep = 11 + s.bits(7);
            }
            if (s.bad || index + rep > nlen + ndist) {
                return false;
            }
            while (rep--) {
                lengths[index++] = len;
            }
        }
    }
    if (lengths[256] == 0) {
        return false; // no end-of-block code
    }
    Huffman litcode;
    Huffman distcode;
    int err = construct(litcode, lengths, nlen);
    if (err != 0 && (err < 0 || nlen != litcode.count[0] + litcode.count[1])) {
        return false;
    }
    err = construct(distcode, lengths + nlen, ndist);
    if (err != 0 && (err < 0 || ndist != distcode.count[0] + distcode.count[1])) {
        return false;
    }
    return codes(s, litcode, distcode);
}

bool storedBlock(Stream& s)
{
    s.bitBuf = 0;
    s.bitCnt = 0;
    if (s.inPos + 4 > s.inLen) {
        return false;
    }
    const unsigned len = s.in[s.inPos] | (s.in[s.inPos + 1] << 8);
    const unsigned nlen = s.in[s.inPos + 2] | (s.in[s.inPos + 3] << 8);
    s.inPos += 4;
    if (len != (~nlen & 0xFFFF) || s.inPos + len > s.inLen || s.out->size() + len > s.limit) {
        return false;
    }
    s.out->append(reinterpret_cast<const char*>(s.in + s.inPos), int(len));
    s.inPos += len;
    return true;
}

// --- Zip structures -----------------------------------------------------------------------------------

quint16 le16(const QByteArray& b, int at)
{
    return quint16(quint8(b[at]) | (quint8(b[at + 1]) << 8));
}
quint32 le32(const QByteArray& b, int at)
{
    return quint32(le16(b, at)) | (quint32(le16(b, at + 2)) << 16);
}

} // namespace

bool inflateRaw(const QByteArray& in, QByteArray* out, qint64 limit)
{
    Stream s{reinterpret_cast<const quint8*>(in.constData()), in.size(), 0, 0, 0, out, limit};
    int last;
    do {
        last = s.bits(1);
        const int type = s.bits(2);
        if (s.bad) {
            return false;
        }
        bool ok;
        switch (type) {
        case 0:
            ok = storedBlock(s);
            break;
        case 1:
            ok = fixedBlock(s);
            break;
        case 2:
            ok = dynamicBlock(s);
            break;
        default:
            ok = false;
        }
        if (!ok || s.bad) {
            return false;
        }
    } while (!last);
    return true;
}

bool ZipReader::open(const QString& path, QString* error)
{
    m_entries.clear();
    m_file.close();
    m_file.setFileName(path);
    if (!m_file.open(QIODevice::ReadOnly)) {
        return fail(error, tr("The file could not be opened."));
    }
    // The end-of-central-directory record is in the last 64 KiB + 22 bytes.
    const qint64 tailSize = qMin<qint64>(m_file.size(), 65557);
    m_file.seek(m_file.size() - tailSize);
    const QByteArray tail = m_file.read(tailSize);
    int eocd = -1;
    for (int i = int(tail.size()) - 22; i >= 0; --i) {
        if (le32(tail, i) == 0x06054b50u) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        return fail(error, tr("This is not a zip file."));
    }
    const int count = le16(tail, eocd + 10);
    const quint32 dirSize = le32(tail, eocd + 12);
    const quint32 dirOffset = le32(tail, eocd + 16);
    if (count == 0xFFFF || dirOffset == 0xFFFFFFFFu) {
        return fail(error, tr("Zip64 files are not supported."));
    }
    m_file.seek(dirOffset);
    const QByteArray dir = m_file.read(dirSize);
    int at = 0;
    for (int n = 0; n < count; ++n) {
        if (at + 46 > dir.size() || le32(dir, at) != 0x02014b50u) {
            return fail(error, tr("The zip file is damaged."));
        }
        Entry e;
        const quint16 flags = le16(dir, at + 8);
        e.method = le16(dir, at + 10);
        e.crc = le32(dir, at + 16);
        e.compressedSize = le32(dir, at + 20);
        e.size = le32(dir, at + 24);
        const int nameLen = le16(dir, at + 28);
        const int extraLen = le16(dir, at + 30);
        const int commentLen = le16(dir, at + 32);
        e.headerOffset = le32(dir, at + 42);
        if (at + 46 + nameLen > dir.size()) {
            return fail(error, tr("The zip file is damaged."));
        }
        e.name = QString::fromUtf8(dir.mid(at + 46, nameLen));
        if (flags & 1) {
            return fail(error, tr("Encrypted zip files are not supported."));
        }
        m_entries.append(e);
        at += 46 + nameLen + extraLen + commentLen;
    }
    return true;
}

QStringList ZipReader::names() const
{
    QStringList out;
    for (const Entry& e : m_entries) {
        out << e.name;
    }
    return out;
}

const ZipReader::Entry* ZipReader::find(const QString& name) const
{
    for (const Entry& e : m_entries) {
        if (e.name == name) {
            return &e;
        }
    }
    return nullptr;
}

qint64 ZipReader::sizeOf(const QString& name) const
{
    const Entry* e = find(name);
    return e ? qint64(e->size) : -1;
}

bool ZipReader::load(const Entry& e, QByteArray* out, qint64 maxSize, QString* error)
{
    if (qint64(e.size) > maxSize) {
        return fail(error, tr("A file in the zip is too large."));
    }
    m_file.seek(qint64(e.headerOffset));
    const QByteArray local = m_file.read(30);
    if (local.size() < 30 || le32(local, 0) != 0x04034b50u) {
        return fail(error, tr("The zip file is damaged."));
    }
    const qint64 dataAt = qint64(e.headerOffset) + 30 + le16(local, 26) + le16(local, 28);
    m_file.seek(dataAt);
    const QByteArray packed = m_file.read(qint64(e.compressedSize));
    if (packed.size() != qint64(e.compressedSize)) {
        return fail(error, tr("The zip file is incomplete."));
    }
    out->clear();
    out->reserve(int(qMin<quint64>(e.size, quint64(maxSize))));
    if (e.method == 0) {
        *out = packed;
    } else if (e.method == 8) {
        if (!inflateRaw(packed, out, maxSize)) {
            return fail(error, tr("The zip file is damaged."));
        }
    } else {
        return fail(error, tr("This zip file uses a compression that is not supported."));
    }
    if (quint64(out->size()) != e.size || crc32(*out) != e.crc) {
        return fail(error, tr("The zip file is damaged (checksum)."));
    }
    return true;
}

bool ZipReader::extract(const QString& name, const QString& destination, QString* error)
{
    const Entry* e = find(name);
    if (!e) {
        return fail(error, tr("\"%1\" is not in the zip file.").arg(name));
    }
    QByteArray data;
    if (!load(*e, &data, qint64(2) << 30, error)) {
        return false;
    }
    QDir().mkpath(QFileInfo(destination).absolutePath());
    QSaveFile out(destination);
    if (!out.open(QIODevice::WriteOnly) || out.write(data) != data.size() || !out.commit()) {
        return fail(error, tr("Could not write %1.").arg(destination));
    }
    return true;
}

QByteArray ZipReader::read(const QString& name, QString* error, qint64 maxSize)
{
    const Entry* e = find(name);
    QByteArray data;
    if (!e) {
        fail(error, tr("\"%1\" is not in the zip file.").arg(name));
        return {};
    }
    if (!load(*e, &data, maxSize, error)) {
        return {};
    }
    return data;
}

} // namespace clipitto::translate
