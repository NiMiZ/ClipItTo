#include "PhoneWindow.h"

#include "QrImage.h"

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace clipitto::ui {

struct PhoneWindow::Row {
    int id = 0;
    QFrame* frame = nullptr;
    QLabel* thumb = nullptr;
    QLabel* status = nullptr;
    QPlainTextEdit* text = nullptr;
    QPushButton* copy = nullptr;
    QPushButton* paste = nullptr;
};

PhoneWindow::PhoneWindow(QWidget* parent)
    : QDialog(parent)
    , m_qr(new QLabel(this))
    , m_url(new QLabel(this))
    , m_others(new QLabel(this))
    , m_countdown(new QLabel(this))
    , m_notice(new QLabel(this))
    , m_status(new QLabel(this))
    , m_addresses(new QComboBox(this))
    , m_stop(new QPushButton(tr("Stop"), this))
    , m_extend(new QPushButton(tr("Extend 5 minutes"), this))
    , m_tick(new QTimer(this))
{
    setWindowTitle(tr("Receive from phone"));
    setObjectName(QStringLiteral("phoneWindow"));
    m_qr->setObjectName(QStringLiteral("phoneQr"));
    m_qr->setAlignment(Qt::AlignCenter);
    m_qr->setMinimumSize(240, 240);
    m_url->setObjectName(QStringLiteral("phoneUrl"));
    m_url->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_url->setWordWrap(true);
    m_url->setAlignment(Qt::AlignCenter);
    m_others->setObjectName(QStringLiteral("phoneOthers"));
    m_others->setWordWrap(true);
    m_others->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_others->setEnabled(false);
    m_addresses->setObjectName(QStringLiteral("phoneAddresses"));
    m_addresses->setToolTip(tr("If the phone cannot open the address, choose another network address of this computer."));
    m_countdown->setObjectName(QStringLiteral("phoneCountdown"));
    m_countdown->setAlignment(Qt::AlignCenter);
    m_notice->setObjectName(QStringLiteral("phoneNotice"));
    m_notice->setWordWrap(true);
    m_notice->setText(
        tr("Scan the code with the phone's camera (the phone must be on the same Wi-Fi), then send a picture or type text. "
           "The text in the picture is added to your history. Anyone on your Wi-Fi who sees this address could send you a "
           "picture; the address stops working after 5 minutes (or Stop). The connection is plain HTTP inside your home "
           "network; nothing goes over the internet. If your computer asks to allow incoming connections, allow it for "
           "private networks."));
    m_status->setObjectName(QStringLiteral("phoneStatus"));
    m_status->setWordWrap(true);
    m_stop->setObjectName(QStringLiteral("phoneStop"));
    m_extend->setObjectName(QStringLiteral("phoneExtend"));

    auto* itemsHost = new QWidget(this);
    m_items = new QVBoxLayout(itemsHost);
    m_items->addStretch(1);
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("phoneItems"));
    scroll->setWidgetResizable(true);
    scroll->setWidget(itemsHost);

    auto* buttons = new QHBoxLayout;
    auto* close = new QPushButton(tr("Close"), this);
    buttons->addWidget(m_stop);
    buttons->addWidget(m_extend);
    buttons->addStretch(1);
    buttons->addWidget(close);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(m_qr);
    layout->addWidget(m_url);
    layout->addWidget(m_addresses);
    layout->addWidget(m_others);
    layout->addWidget(m_countdown);
    layout->addLayout(buttons);
    layout->addWidget(m_notice);
    layout->addWidget(m_status);
    layout->addWidget(scroll, 1);
    resize(560, 760);

    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(m_stop, &QPushButton::clicked, this, &PhoneWindow::stopRequested);
    connect(m_extend, &QPushButton::clicked, this, &PhoneWindow::extendRequested);
    connect(m_addresses, &QComboBox::currentIndexChanged, this, [this](int i) { showAddress(i); });
    m_tick->setInterval(1000);
    connect(m_tick, &QTimer::timeout, this, &PhoneWindow::updateCountdown);
    m_stop->setEnabled(false);
    m_extend->setEnabled(false);
}

PhoneWindow::~PhoneWindow()
{
    qDeleteAll(m_rows);
}

void PhoneWindow::setSession(const QStringList& urls, const QStringList& labels, std::function<qint64()> remainingMs)
{
    m_urls = urls;
    m_labels = labels;
    m_remaining = std::move(remainingMs);
    m_live = !urls.isEmpty();
    m_addresses->blockSignals(true);
    m_addresses->clear();
    QStringList others;
    for (qsizetype i = 0; i < urls.size(); ++i) {
        const QString ip = QUrl(urls.at(i)).host();
        m_addresses->addItem(labels.value(i).isEmpty() ? ip : tr("%1  (%2)").arg(ip, labels.value(i)));
        if (i > 0) {
            others << urls.at(i);
        }
    }
    m_addresses->blockSignals(false);
    m_addresses->setVisible(urls.size() > 1);
    m_others->setVisible(!others.isEmpty());
    m_others->setText(tr("Other addresses of this computer, if the first does not work:\n%1").arg(others.join(QLatin1Char('\n'))));
    m_stop->setEnabled(m_live);
    m_extend->setEnabled(m_live);
    showAddress(0);
    updateCountdown();
    m_tick->start();
}

void PhoneWindow::showAddress(int index)
{
    if (index < 0 || index >= m_urls.size()) {
        return;
    }
    QString error;
    const QImage qr = qrImage(m_urls.at(index), 240, &error);
    m_qr->setPixmap(QPixmap::fromImage(qr));
    m_url->setText(m_urls.at(index));
}

void PhoneWindow::setUnavailable(const QString& reason)
{
    m_live = false;
    m_tick->stop();
    m_qr->clear();
    m_url->clear();
    m_addresses->setVisible(false);
    m_others->setVisible(false);
    m_urls.clear();
    m_stop->setEnabled(false);
    m_extend->setEnabled(false);
    m_countdown->setText(QString());
    m_status->setText(reason);
}

void PhoneWindow::sessionEnded(const QString& why)
{
    m_live = false;
    m_tick->stop();
    m_qr->clear(); // the address no longer works: it is not left on the screen
    m_url->clear();
    m_urls.clear();
    m_addresses->setVisible(false);
    m_others->setVisible(false);
    m_stop->setEnabled(false);
    m_extend->setEnabled(false);
    m_countdown->setText(why);
}

void PhoneWindow::updateCountdown()
{
    if (!m_live || !m_remaining) {
        return;
    }
    const qint64 s = (m_remaining() + 999) / 1000;
    m_countdown->setText(tr("The address works for %1:%2 more").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0')));
}

void PhoneWindow::addItem(int id, const QImage& thumbnail)
{
    auto* r = new Row;
    r->id = id;
    r->frame = new QFrame(this);
    r->frame->setFrameShape(QFrame::StyledPanel);
    r->frame->setObjectName(QStringLiteral("phoneItem"));
    r->thumb = new QLabel(r->frame);
    r->thumb->setObjectName(QStringLiteral("phoneItemThumb"));
    r->thumb->setFixedSize(96, 96);
    r->thumb->setAlignment(Qt::AlignCenter);
    r->status = new QLabel(tr("Sent"), r->frame);
    r->status->setObjectName(QStringLiteral("phoneItemStatus"));
    r->text = new QPlainTextEdit(r->frame);
    r->text->setObjectName(QStringLiteral("phoneItemText"));
    r->text->setReadOnly(true);
    r->text->setMaximumHeight(90);
    r->copy = new QPushButton(tr("Copy text"), r->frame);
    r->copy->setObjectName(QStringLiteral("phoneItemCopy"));
    r->paste = new QPushButton(tr("Paste"), r->frame);
    r->paste->setObjectName(QStringLiteral("phoneItemPaste"));
    r->copy->setEnabled(false);
    r->paste->setEnabled(false);
    auto* right = new QVBoxLayout;
    right->addWidget(r->status);
    right->addWidget(r->text);
    auto* b = new QHBoxLayout;
    b->addWidget(r->copy);
    b->addWidget(r->paste);
    b->addStretch(1);
    right->addLayout(b);
    auto* h = new QHBoxLayout(r->frame);
    h->addWidget(r->thumb, 0, Qt::AlignTop);
    h->addLayout(right, 1);
    m_items->insertWidget(0, r->frame);
    m_rows.prepend(r);
    connect(r->copy, &QPushButton::clicked, this, [this, r] { emit copyRequested(r->text->toPlainText()); });
    connect(r->paste, &QPushButton::clicked, this, [this, r] { emit pasteRequested(r->text->toPlainText()); });
    setItemThumbnail(id, thumbnail);
}

PhoneWindow::Row* PhoneWindow::row(int id) const
{
    for (Row* r : m_rows) {
        if (r->id == id) {
            return r;
        }
    }
    return nullptr;
}

void PhoneWindow::setItemThumbnail(int id, const QImage& thumbnail)
{
    if (Row* r = row(id)) {
        if (thumbnail.isNull()) {
            r->thumb->setText(tr("Text"));
            r->thumb->setPixmap(QPixmap());
        } else {
            r->thumb->setPixmap(QPixmap::fromImage(thumbnail.scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        }
    }
}

void PhoneWindow::setItemStatus(int id, const QString& status)
{
    if (Row* r = row(id)) {
        r->status->setText(status);
    }
}

void PhoneWindow::setItemText(int id, const QString& text)
{
    if (Row* r = row(id)) {
        r->text->setPlainText(text);
        r->copy->setEnabled(!text.trimmed().isEmpty());
        r->paste->setEnabled(!text.trimmed().isEmpty());
    }
}

int PhoneWindow::itemCount() const { return int(m_rows.size()); }
QString PhoneWindow::itemStatus(int id) const { Row* r = row(id); return r ? r->status->text() : QString(); }
QString PhoneWindow::itemText(int id) const { Row* r = row(id); return r ? r->text->toPlainText() : QString(); }
QLabel* PhoneWindow::itemThumbnail(int id) const { Row* r = row(id); return r ? r->thumb : nullptr; }
QPushButton* PhoneWindow::itemCopyButton(int id) const { Row* r = row(id); return r ? r->copy : nullptr; }
QPushButton* PhoneWindow::itemPasteButton(int id) const { Row* r = row(id); return r ? r->paste : nullptr; }
bool PhoneWindow::hasQr() const { const QPixmap p = m_qr->pixmap(); return !p.isNull(); }

} // namespace clipitto::ui
