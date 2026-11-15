#pragma once

#include <QDialog>
#include <QImage>
#include <QStringList>

#include <functional>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTimer;
class QVBoxLayout;

namespace clipitto::ui {

// What Settings needs for "Receive from phone": the daemon opens (or raises) the window, with `parent` as its owner.
struct PhoneSupport {
    std::function<void(QWidget* parent)> open; // null: not available here
};

// "Receive from phone" (roadmap 28): the QR code and the address, a countdown, Stop / Extend, and one row per item received
// (thumbnail, status, the recognized text, Copy text, Paste). It holds no server: the daemon owns that and feeds this window.
// The address (it contains the secret token) is shown here and nowhere else.
class PhoneWindow : public QDialog {
    Q_OBJECT

public:
    explicit PhoneWindow(QWidget* parent = nullptr);
    ~PhoneWindow() override;

    // The addresses a phone could use (best first), each with a label ("wlan0"); the first is shown. `remainingMs` gives the time left.
    void setSession(const QStringList& urls, const QStringList& labels, std::function<qint64()> remainingMs);
    // Nothing to show instead of a QR code (no network, the port could not be opened).
    void setUnavailable(const QString& reason);
    // The server is gone (stopped, expired): the QR code and the address are cleared, the items stay.
    void sessionEnded(const QString& why);

    void addItem(int id, const QImage& thumbnail);
    void setItemThumbnail(int id, const QImage& thumbnail);
    void setItemStatus(int id, const QString& status);
    void setItemText(int id, const QString& text);

    // For tests.
    QLabel* qrLabel() const { return m_qr; }
    QLabel* urlLabel() const { return m_url; }
    QLabel* othersLabel() const { return m_others; }
    QLabel* countdownLabel() const { return m_countdown; }
    QLabel* noticeLabel() const { return m_notice; }
    QLabel* statusLabel() const { return m_status; }
    QComboBox* addressBox() const { return m_addresses; }
    QPushButton* stopButton() const { return m_stop; }
    QPushButton* extendButton() const { return m_extend; }
    int itemCount() const;
    QString itemStatus(int id) const;
    QString itemText(int id) const;
    QLabel* itemThumbnail(int id) const;
    QPushButton* itemCopyButton(int id) const;
    QPushButton* itemPasteButton(int id) const;
    bool hasQr() const;

signals:
    void stopRequested();
    void extendRequested();
    void copyRequested(const QString& text);
    void pasteRequested(const QString& text);

private:
    struct Row;
    Row* row(int id) const;
    void showAddress(int index);
    void updateCountdown();

    QLabel* m_qr;
    QLabel* m_url;
    QLabel* m_others;
    QLabel* m_countdown;
    QLabel* m_notice;
    QLabel* m_status;
    QComboBox* m_addresses;
    QPushButton* m_stop;
    QPushButton* m_extend;
    QVBoxLayout* m_items;
    QTimer* m_tick;
    QStringList m_urls;
    QStringList m_labels;
    std::function<qint64()> m_remaining;
    QList<Row*> m_rows;
    bool m_live = false;
};

} // namespace clipitto::ui
