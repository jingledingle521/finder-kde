/*
 * SPDX-FileCopyrightText: 2026 Brhett
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "findercolumnview.h"

#include <KDirLister>
#include <KDirModel>
#include <KDirSortFilterProxyModel>
#include <KFileItem>
#include <KIO/JobUiDelegateFactory>
#include <KIO/OpenUrlJob>
#include <KLocalizedString>

#include <QApplication>
#include <QLabel>
#include <QListView>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace
{
constexpr int RowHeight = 24;
constexpr int IconSize = 16;
constexpr int ColumnWidth = 230;

const QColor Accent(0x0a, 0x84, 0xff);
const QColor InactiveSelection(255, 255, 255, 46);
const QColor Divider(255, 255, 255, 20);

// Small icons are the full-color artwork scaled down; icon themes often ship
// monochrome outlines at 16 px.
QPixmap smallIcon(const QIcon &icon, qreal dpr)
{
    QPixmap pm = icon.pixmap(QSize(32, 32), dpr);
    pm = pm.scaled(QSize(IconSize, IconSize) * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    pm.setDevicePixelRatio(dpr);
    return pm;
}

/** One row: icon, name, and a chevron for folders; Finder's rounded selection. */
class ColumnDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override
    {
        return QSize(ColumnWidth, RowHeight);
    }

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRectF row = QRectF(option.rect).adjusted(6, 1, -6, -1);
        const bool selected = option.state & QStyle::State_Selected;
        // Blue in the column that has focus; the folders leading to it stay gray.
        const bool focused = option.widget && option.widget->hasFocus() && option.widget->isActiveWindow();
        if (selected) {
            QPainterPath path;
            path.addRoundedRect(row, 5, 5);
            p->fillPath(path, focused ? Accent : InactiveSelection);
        }

        const qreal dpr = p->device() ? p->device()->devicePixelRatioF() : 1.0;
        const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
        const int iconX = int(row.left()) + 6;
        p->drawPixmap(QPoint(iconX, option.rect.top() + (option.rect.height() - IconSize) / 2), smallIcon(icon, dpr));

        const bool isDir = index.data(KDirModel::FileItemRole).value<KFileItem>().isDir();
        const int chevronSpace = isDir ? 18 : 6;
        const QRect textRect(iconX + IconSize + 7, option.rect.top(), int(row.right()) - (iconX + IconSize + 7) - chevronSpace, option.rect.height());
        p->setPen(selected && focused ? QColor(Qt::white) : QColor(255, 255, 255, 225));
        p->setFont(option.font);
        p->drawText(textRect,
                    Qt::AlignVCenter | Qt::AlignLeft,
                    option.fontMetrics.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideMiddle, textRect.width()));

        if (isDir) {
            const QPointF c(row.right() - 10, row.center().y());
            p->setPen(QPen(selected && focused ? QColor(255, 255, 255, 230) : QColor(255, 255, 255, 120), 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p->drawPolyline(QPolygonF{c + QPointF(-1.5, -3.5), c + QPointF(2, 0), c + QPointF(-1.5, 3.5)});
        }
        p->restore();
    }
};
}

FinderColumnView::FinderColumnView(QWidget *parent)
    : QColumnView(parent)
    , m_model(new KDirModel(this))
    , m_proxy(new KDirSortFilterProxyModel(this))
{
    m_proxy->setSourceModel(m_model);
    m_proxy->setSortFoldersFirst(false); // Finder sorts folders and files together
    m_proxy->setSortCaseSensitivity(Qt::CaseInsensitive);
    m_proxy->sort(KDirModel::Name);
    m_model->dirLister()->setAutoErrorHandlingEnabled(false);
    setModel(m_proxy);

    setItemDelegate(new ColumnDelegate(this));
    setResizeGripsVisible(false);
    setColumnWidths({ColumnWidth});
    setFrameShape(QFrame::NoFrame);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    // Opaque in the window's content color: it covers Dolphin's own view.
    setStyleSheet(QStringLiteral("QColumnView { background: rgb(32, 32, 37); border: none; }"));

    // Preview column for a selected file: big icon, name, kind and details.
    m_preview = new QWidget;
    auto *layout = new QVBoxLayout(m_preview);
    layout->setContentsMargins(16, 24, 16, 16);
    layout->setSpacing(6);
    m_previewIcon = new QLabel;
    m_previewIcon->setAlignment(Qt::AlignCenter);
    m_previewName = new QLabel;
    m_previewName->setAlignment(Qt::AlignCenter);
    m_previewName->setWordWrap(true);
    QFont bold = m_previewName->font();
    bold.setWeight(QFont::DemiBold);
    bold.setPointSizeF(bold.pointSizeF() * 1.1);
    m_previewName->setFont(bold);
    m_previewKind = new QLabel;
    m_previewKind->setAlignment(Qt::AlignCenter);
    m_previewKind->setStyleSheet(QStringLiteral("color: rgba(255, 255, 255, 140);"));
    m_previewDetails = new QLabel;
    m_previewDetails->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_previewDetails->setWordWrap(true);
    m_previewDetails->setTextFormat(Qt::RichText);
    layout->addWidget(m_previewIcon);
    layout->addSpacing(6);
    layout->addWidget(m_previewName);
    layout->addWidget(m_previewKind);
    layout->addSpacing(12);
    layout->addWidget(m_previewDetails);
    layout->addStretch(1);
    m_preview->setMinimumWidth(260);
    m_preview->setAttribute(Qt::WA_TranslucentBackground);
    setPreviewWidget(m_preview);
    connect(this, &QColumnView::updatePreviewWidget, this, &FinderColumnView::updatePreview);

    connect(this, &QAbstractItemView::activated, this, &FinderColumnView::openIndex);
}

void FinderColumnView::setRootUrl(const QUrl &url)
{
    if (url == m_root) {
        return;
    }
    m_root = url;
    m_model->dirLister()->openUrl(url);
    setRootIndex(QModelIndex());
}

QUrl FinderColumnView::rootUrl() const
{
    return m_root;
}

QAbstractItemView *FinderColumnView::createColumn(const QModelIndex &rootIndex)
{
    QAbstractItemView *column = QColumnView::createColumn(rootIndex);
    column->setFrameShape(QFrame::NoFrame);
    column->setAttribute(Qt::WA_TranslucentBackground);
    column->viewport()->setAutoFillBackground(false);
    // A hairline between columns, as in Finder.
    column->setStyleSheet(QStringLiteral("QAbstractItemView { background: transparent; border: none;"
                                         " border-right: 1px solid rgba(255, 255, 255, 20); }"));
    column->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    if (auto *list = qobject_cast<QListView *>(column)) {
        list->setUniformItemSizes(true);
        list->setSpacing(0);
    }
    // Repaint the selection when focus moves between columns (blue ↔ gray).
    column->installEventFilter(this);
    return column;
}

bool FinderColumnView::eventFilter(QObject *watched, QEvent *event)
{
    // Columns don't repaint their selection when focus moves between them.
    if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut) {
        if (auto *column = qobject_cast<QAbstractItemView *>(watched)) {
            column->viewport()->update();
        }
    }
    return QColumnView::eventFilter(watched, event);
}

KFileItem FinderColumnView::itemAt(const QModelIndex &index) const
{
    return m_model->itemForIndex(m_proxy->mapToSource(index));
}

void FinderColumnView::openIndex(const QModelIndex &index)
{
    const KFileItem item = itemAt(index);
    if (item.isNull()) {
        return;
    }
    if (item.isDir()) {
        Q_EMIT folderActivated(item.url());
        return;
    }
    auto *job = new KIO::OpenUrlJob(item.targetUrl(), item.mimetype());
    job->setUiDelegate(KIO::createDefaultJobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, window()));
    job->setShowOpenOrExecuteDialog(true);
    job->start();
}

void FinderColumnView::updatePreview(const QModelIndex &index)
{
    const KFileItem item = itemAt(index);
    if (item.isNull()) {
        return;
    }
    const qreal dpr = devicePixelRatioF();
    QPixmap icon = QIcon::fromTheme(item.iconName()).pixmap(QSize(128, 128), dpr);
    m_previewIcon->setPixmap(icon);
    m_previewName->setText(item.text());
    m_previewKind->setText(item.isDir() ? i18nc("@info Finder preview", "Folder")
                                        : i18nc("@info Finder preview: kind - size", "%1 – %2", item.mimeComment(), KIO::convertSize(item.size())));
    const QLocale locale;
    const auto row = [](const QString &label, const QString &value) {
        return QStringLiteral("<tr><td style='color: rgba(255,255,255,0.55); padding-right: 10px;'>%1</td><td>%2</td></tr>")
            .arg(label.toHtmlEscaped(), value.toHtmlEscaped());
    };
    QString html = QStringLiteral("<b>") + i18nc("@title Finder preview", "Information") + QStringLiteral("</b><table style='margin-top: 6px;'>");
    const QDateTime created = item.time(KFileItem::CreationTime);
    if (created.isValid()) {
        html += row(i18nc("@label Finder preview", "Created"), locale.toString(created, QLocale::LongFormat));
    }
    html += row(i18nc("@label Finder preview", "Modified"), locale.toString(item.time(KFileItem::ModificationTime), QLocale::LongFormat));
    html += QStringLiteral("</table>");
    m_previewDetails->setText(html);
}

#include "moc_findercolumnview.cpp"
