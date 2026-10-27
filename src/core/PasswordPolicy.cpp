#include "PasswordPolicy.h"

#include <QCoreApplication>
#include <QDir>
#include <QStringList>
#include <QtGlobal>

namespace clipitto::passwordpolicy {

namespace {

QString t(const char* text)
{
    return QCoreApplication::translate("clipitto::passwordpolicy", text);
}

// Three or more digits in a row that each step up or down by one: 123, 456, 987, 321.
bool hasDigitRun(const QString& password)
{
    int run = 1;
    int direction = 0;
    for (qsizetype i = 1; i < password.size(); ++i) {
        const QChar a = password.at(i - 1);
        const QChar b = password.at(i);
        const int step = (a.isDigit() && b.isDigit()) ? b.digitValue() - a.digitValue() : 0;
        if (step == 1 || step == -1) {
            run = (step == direction) ? run + 1 : 2;
            direction = step;
            if (run >= 3) {
                return true;
            }
        } else {
            run = 1;
            direction = 0;
        }
    }
    return false;
}

} // namespace

QString normalized(const QString& text)
{
    QString out;
    for (const QChar raw : text.toCaseFolded()) {
        QChar c = raw;
        switch (raw.unicode()) {
        case u'@':
        case u'4':
            c = u'a';
            break;
        case u'$':
        case u'5':
            c = u's';
            break;
        case u'0':
            c = u'o';
            break;
        case u'1':
        case u'!':
        case u'|':
            c = u'i';
            break;
        case u'3':
            c = u'e';
            break;
        case u'7':
            c = u't';
            break;
        default:
            break;
        }
        if (c.isLetterOrNumber()) {
            out.append(c);
        }
    }
    return out;
}

QString currentUserName()
{
    for (const char* name : {"USER", "USERNAME", "LOGNAME"}) {
        const QString value = qEnvironmentVariable(name);
        if (!value.isEmpty()) {
            return value;
        }
    }
    return QDir::home().dirName();
}

QList<Rule> check(const QString& password, const QString& userName)
{
    bool upper = false;
    bool lower = false;
    bool digit = false;
    for (const QChar c : password) {
        upper = upper || c.isUpper();
        lower = lower || c.isLower();
        digit = digit || c.isDigit();
    }
    const QString flat = normalized(password);
    const QString user = normalized(userName);
    const bool forbiddenWord = flat.contains(QLatin1String("password")) || flat.contains(QLatin1String("admin"))
        || flat.contains(QLatin1String("root"));
    const bool hasUser = user.size() >= 3 && flat.contains(user);

    return {
        {t("At least 8 characters"), password.size() >= kMinLength},
        {t("An uppercase letter"), upper},
        {t("A lowercase letter"), lower},
        {t("A digit"), digit},
        {t("No \"password\", \"admin\" or \"root\", however it is spelled (P@ssw0rd, ADMIN, r00t)"), !forbiddenWord},
        {t("Not your user name"), !hasUser},
        {t("No 3 digits in a row that count up or down (123, 654)"), !hasDigitRun(password)},
    };
}

bool acceptable(const QString& password, const QString& userName)
{
    for (const Rule& r : check(password, userName)) {
        if (!r.met) {
            return false;
        }
    }
    return true;
}

} // namespace clipitto::passwordpolicy
