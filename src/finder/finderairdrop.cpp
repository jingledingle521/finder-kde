/*
 * SPDX-FileCopyrightText: 2026 Brhett
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "finderairdrop.h"

#include <KLocalizedString>

#include <QComboBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QStandardPaths>
#include <QVBoxLayout>

#include <cmath>

namespace
{
const QColor Accent(0x0a, 0x84, 0xff);
const QColor ContentColor(32, 32, 37);

/** A nearby device: a round tile with its initials and name; files can be dropped on it. */
class PeerTile : public QWidget
{
public:
    PeerTile(const QString &name, std::function<void(const QList<QUrl> &)> onFiles, QWidget *parent)
        : QWidget(parent)
        , m_name(name)
        , m_onFiles(std::move(onFiles))
    {
        setAcceptDrops(true);
        setCursor(Qt::PointingHandCursor);
        setFixedSize(112, 118);
        setToolTip(i18nc("@info:tooltip Finder AirDrop", "Drop files here, or click to choose files to send"));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF circle((width() - 72) / 2.0, 4, 72, 72);
        QLinearGradient fill(circle.topLeft(), circle.bottomLeft());
        fill.setColorAt(0, m_hot ? Accent.lighter(130) : QColor(150, 150, 160));
        fill.setColorAt(1, m_hot ? Accent : QColor(100, 100, 110));
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawEllipse(circle);

        // Initials, like a contact without a photo.
        QString initials;
        for (const QString &word : m_name.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
            if (initials.size() < 2 && word.at(0).isLetterOrNumber()) {
                initials += word.at(0).toUpper();
            }
        }
        QFont big = font();
        big.setPointSizeF(big.pointSizeF() * 1.8);
        big.setWeight(QFont::DemiBold);
        p.setFont(big);
        p.setPen(Qt::white);
        p.drawText(circle, Qt::AlignCenter, initials.isEmpty() ? QStringLiteral("?") : initials);

        p.setFont(font());
        p.setPen(QColor(255, 255, 255, 225));
        const QRect text(0, 84, width(), height() - 84);
        p.drawText(text, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, fontMetrics().elidedText(m_name, Qt::ElideRight, width() * 2 - 8));
    }

    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (event->mimeData()->hasUrls()) {
            event->acceptProposedAction();
            m_hot = true;
            update();
        }
    }

    void dragLeaveEvent(QDragLeaveEvent *) override
    {
        m_hot = false;
        update();
    }

    void dropEvent(QDropEvent *event) override
    {
        m_hot = false;
        update();
        QList<QUrl> files;
        for (const QUrl &url : event->mimeData()->urls()) {
            if (url.isLocalFile()) {
                files.append(url);
            }
        }
        if (!files.isEmpty()) {
            event->acceptProposedAction();
            m_onFiles(files);
        }
    }

    void mouseReleaseEvent(QMouseEvent *) override
    {
        m_onFiles({});
    }

private:
    QString m_name;
    std::function<void(const QList<QUrl> &)> m_onFiles;
    bool m_hot = false;
};
}

FinderAirDropView::FinderAirDropView(QWidget *parent)
    : QWidget(parent)
{
    // Prefer the user's copy (omdrop setup puts it in ~/.local/bin).
    m_omdrop = QStandardPaths::findExecutable(QStringLiteral("omdrop"), {QDir::homePath() + QStringLiteral("/.local/bin")});
    if (m_omdrop.isEmpty()) {
        m_omdrop = QStandardPaths::findExecutable(QStringLiteral("omdrop"));
    }

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(10);

    m_peerArea = new QWidget(this);
    m_peerLayout = new QHBoxLayout(m_peerArea);
    m_peerLayout->setSpacing(18);
    m_peerLayout->setAlignment(Qt::AlignCenter);
    layout->addStretch(2);
    layout->addWidget(m_peerArea);
    layout->addStretch(3);

    m_state = new QLabel(this);
    m_state->setAlignment(Qt::AlignCenter);
    m_state->setWordWrap(true);
    m_state->setStyleSheet(QStringLiteral("color: rgba(255, 255, 255, 140); padding: 0px 60px;"));
    layout->addWidget(m_state);
    layout->addSpacing(96); // the AirDrop glyph is painted here

    m_lead = new QLabel(i18nc("@info Finder AirDrop", "AirDrop lets you share instantly with people nearby."), this);
    m_lead->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_lead);

    auto *row = new QHBoxLayout;
    row->addStretch(1);
    auto *label = new QLabel(i18nc("@label Finder AirDrop", "Allow me to be discovered by:"), this);
    label->setStyleSheet(QStringLiteral("color: rgba(255, 255, 255, 170);"));
    row->addWidget(label);
    m_discover = new QComboBox(this);
    m_discover->addItem(i18nc("@item Finder AirDrop visibility", "No One"));
    m_discover->addItem(i18nc("@item Finder AirDrop visibility", "Everyone for 10 Minutes"));
    connect(m_discover, &QComboBox::activated, this, [this](int index) {
        setDiscoverable(index == 1);
    });
    row->addWidget(m_discover);
    row->addStretch(1);
    layout->addLayout(row);

    // The Wi-Fi caveat: omdrop-owl takes over Intel Wi-Fi cards while visible.
    m_warning = new QLabel(i18nc("@info Finder AirDrop",
                                 "⚠  While AirDrop is on, this computer's Wi-Fi disconnects for a moment and stays off until AirDrop turns off "
                                 "(this Wi-Fi card can't do both at once). Downloads, calls and VPNs will drop."),
                           this);
    m_warning->setAlignment(Qt::AlignCenter);
    m_warning->setWordWrap(true);
    m_warning->setStyleSheet(QStringLiteral("color: #ffd60a; padding: 6px 40px;"));
    layout->addWidget(m_warning);

    m_statusTimer.setInterval(2000);
    connect(&m_statusTimer, &QTimer::timeout, this, &FinderAirDropView::refreshStatus);
    m_peersTimer.setSingleShot(true);
    m_peersTimer.setInterval(3000);
    connect(&m_peersTimer, &QTimer::timeout, this, &FinderAirDropView::refreshPeers);
}

FinderAirDropView::~FinderAirDropView()
{
    stopPeers();
}

void FinderAirDropView::setPendingFiles(const QList<QUrl> &files)
{
    m_pending = files;
    if (!files.isEmpty()) {
        setState(i18ncp("@info Finder AirDrop", "Choose a device to send 1 item to.", "Choose a device to send %1 items to.", files.size()));
    }
}

QIcon FinderAirDropView::icon(const QColor &color)
{
    QIcon icon;
    for (const int size : {16, 18, 24, 32, 64, 128}) {
        for (const qreal dpr : {1.0, 1.5, 2.0}) {
            QPixmap pm(QSize(size, size) * dpr);
            pm.setDevicePixelRatio(dpr);
            pm.fill(Qt::transparent);
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            p.scale(size / 16.0, size / 16.0);
            p.setPen(QPen(color, 1.1, Qt::SolidLine, Qt::RoundCap));
            p.setBrush(Qt::NoBrush);
            const QPointF c(8, 9);
            // Three arcs opening downward around a dot, like Apple's glyph.
            for (const qreal r : {2.6, 4.9, 7.2}) {
                p.drawArc(QRectF(c.x() - r, c.y() - r, 2 * r, 2 * r), int(-40 * 16), int(260 * 16));
            }
            p.setPen(Qt::NoPen);
            p.setBrush(color);
            p.drawEllipse(c, 1.1, 1.1);
            p.end();
            icon.addPixmap(pm);
        }
    }
    return icon;
}

void FinderAirDropView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), ContentColor);

    // Radar rings spreading from the AirDrop glyph above the caption.
    const QPointF center(width() / 2.0, m_lead->geometry().top() - 56);
    for (int i = 1; i <= 6; ++i) {
        const qreal r = 70.0 * i;
        p.setPen(QPen(QColor(255, 255, 255, std::max(4, 22 - i * 3)), 1));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(center, r, r);
    }
    p.setPen(Qt::NoPen);
    QLinearGradient fill(center - QPointF(0, 40), center + QPointF(0, 40));
    fill.setColorAt(0, QColor(0x5a, 0xc8, 0xfa));
    fill.setColorAt(1, Accent);
    p.setBrush(fill);
    p.drawEllipse(center, 40, 40);
    const QPixmap glyph = icon(Qt::white).pixmap(QSize(52, 52), devicePixelRatioF());
    p.drawPixmap(QPointF(center.x() - 26, center.y() - 27), glyph);
}

void FinderAirDropView::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    checkReady();
}

void FinderAirDropView::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    m_statusTimer.stop();
    m_peersTimer.stop();
    stopPeers();
}

void FinderAirDropView::runOmdrop(const QStringList &args, std::function<void(int, const QByteArray &, const QByteArray &)> done)
{
    auto *process = new QProcess(this);
    connect(process, &QProcess::finished, this, [process, done](int code, QProcess::ExitStatus) {
        done(code, process->readAllStandardOutput(), process->readAllStandardError());
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, this, [process, done](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            done(-1, {}, {});
            process->deleteLater();
        }
    });
    process->start(m_omdrop, args);
}

void FinderAirDropView::checkReady()
{
    if (m_omdrop.isEmpty()) {
        m_ready = false;
        m_discover->setEnabled(false);
        setState(i18nc("@info Finder AirDrop", "AirDrop needs omdrop. Install omdrop-owl and run “omdrop setup”."));
        return;
    }
    runOmdrop({QStringLiteral("doctor"), QStringLiteral("--json")}, [this](int, const QByteArray &out, const QByteArray &) {
        const QJsonObject doctor = QJsonDocument::fromJson(out).object();
        m_ready = doctor.value(QStringLiteral("ready")).toBool();
        m_discover->setEnabled(m_ready);
        if (!m_ready) {
            QStringList missing;
            for (const QJsonValue &item : doctor.value(QStringLiteral("missing")).toArray()) {
                missing.append(item.toObject().value(QStringLiteral("say")).toString());
            }
            setState(i18nc("@info Finder AirDrop", "AirDrop isn't ready: %1", missing.isEmpty() ? i18n("omdrop did not answer.") : missing.join(QLatin1Char(' '))));
            return;
        }
        refreshStatus();
        m_statusTimer.start();
    });
}

void FinderAirDropView::refreshStatus()
{
    runOmdrop({QStringLiteral("status"), QStringLiteral("--json")}, [this](int, const QByteArray &out, const QByteArray &) {
        const QJsonObject status = QJsonDocument::fromJson(out).object();
        const bool visible = status.value(QStringLiteral("visible")).toBool() || status.value(QStringLiteral("receiving")).toBool();
        m_discover->setCurrentIndex(visible ? 1 : 0);
        if (visible != m_visible) {
            m_visible = visible;
            if (visible) {
                refreshPeers();
            } else {
                stopPeers();
                showPeers({});
            }
        }
        if (!m_sending && m_pending.isEmpty()) {
            if (!visible) {
                setState(i18nc("@info Finder AirDrop", "Turn on discovery to see nearby devices."));
            } else if (m_peerLayout->count() == 0) {
                setState(i18nc("@info Finder AirDrop", "Looking for nearby devices… Open the share sheet on your iPhone or Mac."));
            }
        }
    });
}

void FinderAirDropView::refreshPeers()
{
    if (!m_visible || m_peersProcess) {
        return;
    }
    // A named lookup takes 15-20 seconds; run one at a time, then again shortly after.
    auto *process = new QProcess(this);
    m_peersProcess = process;
    connect(process, &QProcess::finished, this, [this, process](int code, QProcess::ExitStatus) {
        const QJsonArray array = QJsonDocument::fromJson(process->readAllStandardOutput()).array();
        process->deleteLater();
        if (code == 0 || !array.isEmpty()) {
            QList<Peer> peers;
            for (const QJsonValue &value : array) {
                const QJsonObject peer = value.toObject();
                const QString name = peer.value(QStringLiteral("name")).toString();
                if (!name.isEmpty()) { // unnamed devices can't be picked by a person
                    peers.append({peer.value(QStringLiteral("mac")).toString(), name});
                }
            }
            showPeers(peers);
        }
        if (m_visible && isVisible()) {
            m_peersTimer.start();
        }
    });
    process->start(m_omdrop, {QStringLiteral("peers"), QStringLiteral("-n"), QStringLiteral("--json")});
}

void FinderAirDropView::stopPeers()
{
    if (m_peersProcess) {
        // omdrop runs the lookup as its own process group and cleans up on TERM.
        m_peersProcess->disconnect(this);
        m_peersProcess->terminate();
        m_peersProcess->waitForFinished(2000);
        m_peersProcess->deleteLater();
    }
}

void FinderAirDropView::setDiscoverable(bool on)
{
    m_discover->setEnabled(false);
    setState(on ? i18nc("@info Finder AirDrop", "Turning AirDrop on… Wi-Fi will disconnect.") : i18nc("@info Finder AirDrop", "Turning AirDrop off… Wi-Fi will reconnect."));
    const QStringList args = on ? QStringList{QStringLiteral("on"), QStringLiteral("-e"), QStringLiteral("10m")} : QStringList{QStringLiteral("off")};
    runOmdrop(args, [this](int code, const QByteArray &, const QByteArray &err) {
        m_discover->setEnabled(m_ready);
        if (code != 0) {
            const QString message = QString::fromUtf8(err).trimmed().section(QLatin1Char('\n'), -1);
            setState(message.isEmpty() ? i18nc("@info Finder AirDrop", "omdrop could not change AirDrop.") : message);
        }
        refreshStatus();
    });
}

void FinderAirDropView::showPeers(const QList<Peer> &peers)
{
    while (QLayoutItem *item = m_peerLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    for (const Peer &peer : peers) {
        auto *tile = new PeerTile(peer.name, [this, peer](const QList<QUrl> &dropped) {
            QList<QUrl> files = dropped;
            if (files.isEmpty()) {
                files = m_pending;
            }
            if (files.isEmpty()) {
                files = QFileDialog::getOpenFileUrls(this, i18nc("@title:window", "Send with AirDrop to %1", peer.name));
            }
            if (!files.isEmpty()) {
                send(peer, files);
            }
        }, m_peerArea);
        m_peerLayout->addWidget(tile);
    }
}

void FinderAirDropView::send(const Peer &peer, const QList<QUrl> &files)
{
    if (m_sending) {
        return;
    }
    m_sending = true;
    m_pending.clear();
    setState(i18ncp("@info Finder AirDrop", "Sending 1 item to %2… Accept it on that device.", "Sending %1 items to %2… Accept them on that device.", files.size(), peer.name));
    QStringList args{QStringLiteral("send"), QStringLiteral("--to"), peer.mac};
    for (const QUrl &file : files) {
        args.append(file.toLocalFile());
    }
    runOmdrop(args, [this, peer](int code, const QByteArray &, const QByteArray &err) {
        m_sending = false;
        if (code == 0) {
            setState(i18nc("@info Finder AirDrop", "Sent to %1.", peer.name));
        } else {
            const QString message = QString::fromUtf8(err).trimmed().section(QLatin1Char('\n'), -1);
            setState(i18nc("@info Finder AirDrop", "Couldn't send to %1: %2", peer.name, message.isEmpty() ? i18n("the transfer failed.") : message));
        }
    });
}

void FinderAirDropView::setState(const QString &text)
{
    m_state->setText(text);
}

#include "moc_finderairdrop.cpp"
