/*
 * SPDX-FileCopyrightText: 2026 Brhett
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "finderairdrop.h"
#include "finderairdropservice.h"

#include <KLocalizedString>

#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPointer>
#include <QVBoxLayout>

namespace
{
const QColor Accent(0x0a, 0x84, 0xff);
const QColor ContentColor(32, 32, 37);
}

FinderAirDropView::FinderAirDropView(QWidget *parent)
    : QWidget(parent)
{
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
    m_discover->setEnabled(false);
    connect(m_discover, &QComboBox::activated, this, [this](int index) {
        const bool on = index == 1;
        m_discover->setEnabled(false);
        setState(on ? i18nc("@info Finder AirDrop", "Turning AirDrop on… Wi-Fi will disconnect.") : i18nc("@info Finder AirDrop", "Turning AirDrop off… Wi-Fi will reconnect."));
        FinderAirDrop::instance()->setDiscoverable(on, [view = QPointer<FinderAirDropView>(this)](bool ok, const QString &error) {
            if (!view) {
                return;
            }
            view->m_discover->setEnabled(view->m_ready);
            if (!ok) {
                view->setState(error);
            }
        });
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

    auto *airDrop = FinderAirDrop::instance();
    connect(airDrop, &FinderAirDrop::visibleChanged, this, &FinderAirDropView::updateState);
    connect(airDrop, &FinderAirDrop::peersChanged, this, &FinderAirDropView::updatePeers);
}

void FinderAirDropView::setPendingFiles(const QList<QUrl> &files)
{
    m_pending = files;
    updateState();
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
    FinderAirDrop::instance()->checkReady([view = QPointer<FinderAirDropView>(this)](bool ready, const QString &why) {
        if (!view) {
            return;
        }
        view->m_ready = ready;
        view->m_notReady = why;
        view->m_discover->setEnabled(ready);
        if (ready && view->isVisible()) {
            FinderAirDrop::instance()->watchPeers(view);
        }
        view->updateState();
    });
}

void FinderAirDropView::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    FinderAirDrop::instance()->unwatchPeers(this);
}

void FinderAirDropView::updateState()
{
    const bool visible = FinderAirDrop::instance()->isVisible();
    m_discover->setCurrentIndex(visible ? 1 : 0);
    if (!m_ready) {
        setState(m_notReady);
    } else if (!m_pending.isEmpty()) {
        setState(i18ncp("@info Finder AirDrop", "Choose a device to send 1 item to.", "Choose a device to send %1 items to.", m_pending.size()));
    } else if (!visible) {
        setState(i18nc("@info Finder AirDrop", "Turn on discovery to see nearby devices."));
    } else if (m_peerLayout->count() == 0) {
        setState(i18nc("@info Finder AirDrop", "Looking for nearby devices… Open the share sheet on your iPhone or Mac."));
    } else {
        setState(i18nc("@info Finder AirDrop", "Drop files on a device, or click it to choose files."));
    }
    if (!visible) {
        updatePeers();
    }
}

void FinderAirDropView::updatePeers()
{
    while (QLayoutItem *item = m_peerLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    if (FinderAirDrop::instance()->isVisible()) {
        const QList<FinderAirDropPeer> peers = FinderAirDrop::instance()->peers();
        for (const FinderAirDropPeer &peer : peers) {
            m_peerLayout->addWidget(new FinderPeerTile(peer, [this, peer](const QList<QUrl> &dropped) {
                QList<QUrl> files = dropped.isEmpty() ? m_pending : dropped;
                if (files.isEmpty()) {
                    files = QFileDialog::getOpenFileUrls(this, i18nc("@title:window", "Send with AirDrop to %1", peer.name));
                }
                if (!files.isEmpty()) {
                    m_pending.clear();
                    FinderAirDrop::instance()->send(peer, files, false);
                    updateState();
                }
            }, m_peerArea));
        }
    }
    if (m_ready && m_pending.isEmpty() && FinderAirDrop::instance()->isVisible()) {
        setState(m_peerLayout->count() ? i18nc("@info Finder AirDrop", "Drop files on a device, or click it to choose files.")
                                       : i18nc("@info Finder AirDrop", "Looking for nearby devices… Open the share sheet on your iPhone or Mac."));
    }
}

void FinderAirDropView::setState(const QString &text)
{
    m_state->setText(text);
}

#include "moc_finderairdrop.cpp"
