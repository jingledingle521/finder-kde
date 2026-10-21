/*
 * SPDX-FileCopyrightText: 2026 Brhett
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "findertransfers.h"
#include "finderaccent.h"
#include "finderairdrop.h"

#include <KFormat>
#include <KIO/OpenFileManagerWindowJob>
#include <KLocalizedString>

#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace
{
constexpr int CardWidth = 340;

/** One transfer: icon, title, detail, a thin progress bar and an action button. */
class TransferCard : public QWidget
{
public:
    explicit TransferCard(QWidget *parent)
        : QWidget(parent)
    {
        setFixedWidth(CardWidth);
        setAttribute(Qt::WA_TranslucentBackground);
        m_title = new QLabel(this);
        QFont bold = m_title->font();
        bold.setWeight(QFont::DemiBold);
        m_title->setFont(bold);
        m_detail = new QLabel(this);
        m_detail->setStyleSheet(QStringLiteral("color: rgba(255, 255, 255, 150);"));
        m_button = new QToolButton(this);
        m_button->setAutoRaise(true);
        m_button->setStyleSheet(QStringLiteral("QToolButton { color: rgba(255,255,255,200); border: none; padding: 2px 6px; border-radius: 9px; }"
                                               "QToolButton:hover { background: rgba(255,255,255,30); }"));
        setFixedHeight(76);
    }

    void set(const FinderTransfer &t)
    {
        m_transfer = t;
        const QString verb = t.incoming ? (t.state == FinderTransfer::Done ? i18nc("@info Finder AirDrop", "Received") : i18nc("@info Finder AirDrop", "Receiving"))
                                        : (t.state == FinderTransfer::Done ? i18nc("@info Finder AirDrop", "Sent") : i18nc("@info Finder AirDrop", "Sending"));
        m_title->setText(fontMetrics().elidedText(i18nc("@info Finder AirDrop: verb, file", "%1 “%2”", verb, t.title), Qt::ElideMiddle, CardWidth - 120));

        QString detail = t.detail;
        if (t.state == FinderTransfer::Running && t.total > 0) {
            const KFormat format;
            detail = i18nc("@info Finder AirDrop: done of total — peer", "%1 of %2 — %3", format.formatByteSize(t.done), format.formatByteSize(t.total),
                           t.incoming ? i18nc("@info", "from %1", t.peer) : i18nc("@info", "to %1", t.peer));
        }
        m_detail->setText(fontMetrics().elidedText(detail, Qt::ElideRight, CardWidth - 90));

        if (t.state == FinderTransfer::Done && t.incoming && !t.files.isEmpty()) {
            m_button->setText(i18nc("@action:button Finder AirDrop", "Show"));
            m_button->setToolTip(i18nc("@info:tooltip", "Show in Downloads"));
        } else if (!t.incoming && (t.state == FinderTransfer::Waiting || t.state == FinderTransfer::Running)) {
            m_button->setText(QStringLiteral("✕"));
            m_button->setToolTip(i18nc("@info:tooltip", "Cancel"));
        } else {
            m_button->setText(QStringLiteral("✕"));
            m_button->setToolTip(i18nc("@info:tooltip", "Close"));
        }
        m_button->disconnect();
        connect(m_button, &QToolButton::clicked, this, [this]() {
            const FinderTransfer &t = m_transfer;
            if (t.state == FinderTransfer::Done && t.incoming && !t.files.isEmpty()) {
                QList<QUrl> urls;
                for (const QString &path : t.files) {
                    urls.append(QUrl::fromLocalFile(path));
                }
                KIO::highlightInFileManager(urls);
            } else if (!t.incoming && (t.state == FinderTransfer::Waiting || t.state == FinderTransfer::Running)) {
                FinderAirDrop::instance()->cancel(t.id);
                return;
            }
            hide();
            deleteLater();
        });
        update();
    }

protected:
    void resizeEvent(QResizeEvent *) override
    {
        m_title->setGeometry(64, 12, width() - 64 - 70, 20);
        m_detail->setGeometry(64, 32, width() - 64 - 16, 18);
        m_button->setGeometry(width() - 62, 10, 52, 22);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF card = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath path;
        path.addRoundedRect(card, 14, 14);
        p.fillPath(path, QColor(50, 50, 56, 245));
        p.setPen(QPen(QColor(255, 255, 255, 30), 1));
        p.drawPath(path);

        // AirDrop badge: blue disc with the glyph; green tick when done, red when failed.
        const QRectF badge(14, 14, 38, 38);
        QColor badgeColor = finderAccent();
        if (m_transfer.state == FinderTransfer::Done) {
            badgeColor = QColor(0x30, 0xd1, 0x58);
        } else if (m_transfer.state == FinderTransfer::Failed) {
            badgeColor = QColor(0xff, 0x45, 0x3a);
        }
        p.setPen(Qt::NoPen);
        p.setBrush(badgeColor);
        p.drawEllipse(badge);
        p.drawPixmap(badge.adjusted(7, 7, -7, -7).toRect(), FinderAirDropView::icon(Qt::white).pixmap(QSize(24, 24), devicePixelRatioF()));

        // Progress: a thin bar, empty until bytes start moving.
        const QRectF track(64, height() - 20, width() - 64 - 16, 5);
        QPainterPath trackPath;
        trackPath.addRoundedRect(track, 2.5, 2.5);
        p.fillPath(trackPath, QColor(255, 255, 255, 30));
        qreal fraction = 0;
        if (m_transfer.state == FinderTransfer::Done) {
            fraction = 1;
        } else if (m_transfer.total > 0) {
            fraction = std::clamp(qreal(m_transfer.done) / qreal(m_transfer.total), 0.0, 1.0);
        }
        if (fraction > 0 && m_transfer.state != FinderTransfer::Failed) {
            QPainterPath bar;
            bar.addRoundedRect(QRectF(track.left(), track.top(), std::max(5.0, track.width() * fraction), track.height()), 2.5, 2.5);
            p.fillPath(bar, m_transfer.state == FinderTransfer::Done ? QColor(0x30, 0xd1, 0x58) : finderAccent());
        }
    }

private:
    FinderTransfer m_transfer;
    QLabel *m_title;
    QLabel *m_detail;
    QToolButton *m_button;
};
}

FinderTransfersView::FinderTransfersView(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TranslucentBackground);
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(8);
    hide();
    connect(FinderAirDrop::instance(), &FinderAirDrop::transferChanged, this, &FinderTransfersView::showTransfer);
}

void FinderTransfersView::setAnchor(const QPoint &bottomRight)
{
    m_anchor = bottomRight;
    relayout();
}

void FinderTransfersView::showTransfer(const FinderTransfer &transfer)
{
    auto *card = static_cast<TransferCard *>(m_cards.value(transfer.id));
    if (!card) {
        card = new TransferCard(this);
        m_cards.insert(transfer.id, card);
        m_layout->addWidget(card);
        connect(card, &QObject::destroyed, this, [this, id = transfer.id]() {
            m_cards.remove(id);
            QTimer::singleShot(0, this, &FinderTransfersView::relayout);
        });
    }
    card->set(transfer);
    card->show();
    if (transfer.state == FinderTransfer::Done || transfer.state == FinderTransfer::Failed) {
        // Finished cards stay a little while (longer for failures), then go.
        QTimer::singleShot(transfer.state == FinderTransfer::Done ? 8000 : 15000, card, [card]() {
            card->hide();
            card->deleteLater();
        });
    }
    relayout();
}

void FinderTransfersView::relayout()
{
    if (m_cards.isEmpty()) {
        hide();
        return;
    }
    adjustSize();
    const QSize size = sizeHint();
    setGeometry(m_anchor.x() - size.width(), m_anchor.y() - size.height(), size.width(), size.height());
    show();
    raise();
}

#include "moc_findertransfers.cpp"
