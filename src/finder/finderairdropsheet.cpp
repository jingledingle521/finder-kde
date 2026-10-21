/*
 * SPDX-FileCopyrightText: 2026 Brhett
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "finderairdropsheet.h"
#include "finderaccent.h"
#include "finderairdrop.h"
#include "finderairdropservice.h"

#include <KConfigGroup>
#include <KLocalizedString>
#include <KSharedConfig>

#include <QCheckBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace
{
QLabel *glyph(QWidget *parent, int size)
{
    auto *label = new QLabel(parent);
    label->setPixmap(FinderAirDropView::icon(finderAccent()).pixmap(QSize(size, size), parent->devicePixelRatioF()));
    label->setAlignment(Qt::AlignCenter);
    return label;
}

QLabel *heading(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    QFont font = label->font();
    font.setWeight(QFont::DemiBold);
    font.setPointSizeF(font.pointSizeF() * 1.15);
    label->setFont(font);
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    return label;
}

KConfigGroup settings()
{
    return KSharedConfig::openConfig()->group(QStringLiteral("Finder"));
}
}

FinderAirDropSheet::FinderAirDropSheet(const QList<QUrl> &files, QWidget *parent)
    : QDialog(parent)
    , m_files(files)
{
    setWindowTitle(i18nc("@title:window", "AirDrop"));
    setModal(true);
    setMinimumWidth(520);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 22, 24, 18);
    m_pages = new QStackedWidget(this);
    layout->addWidget(m_pages);

    const QString what = files.size() == 1 ? QStringLiteral("“%1”").arg(files.first().fileName())
                                           : i18ncp("@info Finder AirDrop", "1 item", "%1 items", files.size());

    // Page 1: the Wi-Fi warning.
    auto *warning = new QWidget(m_pages);
    auto *w = new QVBoxLayout(warning);
    w->setSpacing(10);
    w->addWidget(glyph(warning, 56));
    w->addWidget(heading(i18nc("@title Finder AirDrop", "Turn on AirDrop to share %1?", what), warning));
    auto *body = new QLabel(i18nc("@info Finder AirDrop",
                                  "AirDrop takes over this computer's Wi-Fi card, so Wi-Fi disconnects while it's on. "
                                  "Downloads, calls and VPNs will drop for a moment. Wi-Fi reconnects on its own when the transfer is done."),
                            warning);
    body->setWordWrap(true);
    body->setAlignment(Qt::AlignCenter);
    body->setStyleSheet(QStringLiteral("color: rgba(255, 255, 255, 180);"));
    w->addWidget(body);
    m_warningState = new QLabel(warning);
    m_warningState->setAlignment(Qt::AlignCenter);
    m_warningState->setWordWrap(true);
    m_warningState->setStyleSheet(QStringLiteral("color: #ff9f0a;"));
    m_warningState->hide();
    w->addWidget(m_warningState);
    m_dontWarn = new QCheckBox(i18nc("@option:check Finder AirDrop", "Don't warn me again"), warning);
    w->addWidget(m_dontWarn, 0, Qt::AlignCenter);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto *cancel = new QPushButton(i18nc("@action:button", "Cancel"), warning);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    buttons->addWidget(cancel);
    m_turnOnButton = new QPushButton(i18nc("@action:button Finder AirDrop", "Turn On AirDrop"), warning);
    m_turnOnButton->setDefault(true);
    connect(m_turnOnButton, &QPushButton::clicked, this, [this]() {
        if (m_dontWarn->isChecked()) {
            KConfigGroup group = settings();
            group.writeEntry("AirDropWarned", true);
            group.sync();
        }
        turnOn();
    });
    buttons->addWidget(m_turnOnButton);
    w->addLayout(buttons);
    m_pages->addWidget(warning);

    // Page 2: nearby devices.
    auto *devices = new QWidget(m_pages);
    auto *d = new QVBoxLayout(devices);
    d->setSpacing(10);
    d->addWidget(heading(i18nc("@title Finder AirDrop", "AirDrop %1", what), devices));
    m_peerArea = new QWidget(devices);
    m_peerArea->setMinimumHeight(130);
    m_peerLayout = new QHBoxLayout(m_peerArea);
    m_peerLayout->setSpacing(16);
    m_peerLayout->setAlignment(Qt::AlignCenter);
    d->addWidget(m_peerArea);
    m_searching = new QLabel(devices);
    m_searching->setAlignment(Qt::AlignCenter);
    m_searching->setWordWrap(true);
    m_searching->setStyleSheet(QStringLiteral("color: rgba(255, 255, 255, 150);"));
    d->addWidget(m_searching);
    auto *row = new QHBoxLayout;
    row->addStretch(1);
    auto *close = new QPushButton(i18nc("@action:button", "Cancel"), devices);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    row->addWidget(close);
    d->addLayout(row);
    m_pages->addWidget(devices);

    auto *airDrop = FinderAirDrop::instance();
    connect(airDrop, &FinderAirDrop::peersChanged, this, &FinderAirDropSheet::updatePeers);

    // Already on, or warned before: straight to the devices.
    if (airDrop->isVisible()) {
        showDevices();
    } else if (settings().readEntry("AirDropWarned", false)) {
        turnOn();
    } else {
        m_pages->setCurrentIndex(0);
        // Make sure omdrop can actually do this before offering it.
        airDrop->checkReady([sheet = QPointer<FinderAirDropSheet>(this)](bool ready, const QString &why) {
            if (sheet && !ready) {
                sheet->m_turnOnButton->setEnabled(false);
                sheet->m_warningState->setText(why);
                sheet->m_warningState->show();
            }
        });
    }
}

FinderAirDropSheet::~FinderAirDropSheet()
{
    FinderAirDrop::instance()->unwatchPeers(this);
    if (m_turnedOn && !m_sent) {
        // Closed without sending: give Wi-Fi back.
        FinderAirDrop::instance()->setDiscoverable(false);
    }
}

void FinderAirDropSheet::turnOn()
{
    m_turnOnButton->setEnabled(false);
    m_turnOnButton->setText(i18nc("@action:button Finder AirDrop", "Turning On…"));
    FinderAirDrop::instance()->setDiscoverable(true, [sheet = QPointer<FinderAirDropSheet>(this)](bool ok, const QString &error) {
        if (!sheet) {
            if (ok) {
                FinderAirDrop::instance()->setDiscoverable(false); // closed meanwhile
            }
            return;
        }
        if (!ok) {
            sheet->m_pages->setCurrentIndex(0);
            sheet->m_turnOnButton->setEnabled(true);
            sheet->m_turnOnButton->setText(i18nc("@action:button Finder AirDrop", "Turn On AirDrop"));
            sheet->m_warningState->setText(error);
            sheet->m_warningState->show();
            return;
        }
        sheet->m_turnedOn = true;
        sheet->showDevices();
    });
}

void FinderAirDropSheet::showDevices()
{
    m_pages->setCurrentIndex(1);
    m_searching->setText(i18nc("@info Finder AirDrop", "Looking for nearby devices… Make sure the other device is unlocked, with AirDrop on."));
    FinderAirDrop::instance()->watchPeers(this);
    updatePeers();
}

void FinderAirDropSheet::updatePeers()
{
    while (QLayoutItem *item = m_peerLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const QList<FinderAirDropPeer> peers = FinderAirDrop::instance()->peers();
    for (const FinderAirDropPeer &peer : peers) {
        m_peerLayout->addWidget(new FinderPeerTile(peer, [this, peer](const QList<QUrl> &) {
            m_sent = true;
            FinderAirDrop::instance()->send(peer, m_files, m_turnedOn);
            accept();
        }, m_peerArea));
    }
    if (!peers.isEmpty()) {
        m_searching->setText(i18nc("@info Finder AirDrop", "Click a device to send. Finding names takes a few seconds, so more may appear."));
    }
}

#include "moc_finderairdropsheet.cpp"
