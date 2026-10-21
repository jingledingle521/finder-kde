/*
 * SPDX-FileCopyrightText: 2026 Brhett
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "findercolumnview.h"
#include "finderaccent.h"
#include "finderairdrop.h"
#include "finderairdropservice.h"

#include <KDirLister>
#include <KDirModel>
#include <KDirSortFilterProxyModel>
#include <KFileItem>
#include <KFileItemActions>
#include <KFileItemListProperties>
#include <KIO/DeleteOrTrashJob>
#include <KIO/JobUiDelegateFactory>
#include <KIO/MkdirJob>
#include <KIO/OpenUrlJob>
#include <KIO/Paste>
#include <KIO/PasteJob>
#include <KIO/PreviewJob>
#include <KIO/RenameFileDialog>
#include <KLocalizedString>
#include <KPropertiesDialog>
#include <KUrlMimeData>

#include <QApplication>
#include <QClipboard>
#include <QInputDialog>
#include <QMenu>
#include <QMimeData>
#include <QTextLayout>
#include <QTimer>
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
    explicit ColumnDelegate(QAbstractItemView *view)
        : QStyledItemDelegate(view)
        , m_view(view)
    {
    }

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override
    {
        return QSize(ColumnWidth, RowHeight);
    }

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRectF row = QRectF(option.rect).adjusted(6, 1, -6, -1);
        // The current item and the folders leading to it are highlighted:
        // blue in the active column (the current item's), gray before it.
        const QModelIndex current = m_view->currentIndex();
        bool onPath = false;
        for (QModelIndex at = current; at.isValid(); at = at.parent()) {
            if (at == index) {
                onPath = true;
                break;
            }
        }
        const bool selected = (option.state & QStyle::State_Selected) || onPath;
        const bool focused = current.isValid() && index.parent() == current.parent() && m_view->isActiveWindow();
        if (selected) {
            QPainterPath path;
            path.addRoundedRect(row, 5, 5);
            p->fillPath(path, focused ? finderAccent() : InactiveSelection);
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

private:
    QAbstractItemView *m_view;
};

/** Finder's sizes: decimal units, "193 bytes", "245 KB", "1.2 MB". */
QString finderSize(KIO::filesize_t size)
{
    if (size < 1000) {
        return i18ncp("@item Finder size", "%1 byte", "%1 bytes", qulonglong(size));
    }
    static const char *units[] = {"KB", "MB", "GB", "TB", "PB"};
    double value = size / 1000.0;
    int unit = 0;
    while (value >= 999.5 && unit < 4) {
        value /= 1000.0;
        ++unit;
    }
    QString number = value >= 99.95 ? QString::number(qRound(value)) : QLocale().toString(value, 'f', 1);
    if (number.endsWith(QLatin1String(".0")) || number.endsWith(QLatin1String(",0"))) {
        number.chop(2);
    }
    return number + QLatin1Char(' ') + QLatin1String(units[unit]);
}

/** Finder's long dates: "Today at 1:30 PM", "Sunday, October 18, 2026 at 1:30 PM". */
QString finderDate(const QDateTime &time)
{
    const QLocale locale;
    const QString clock = locale.toString(time.time(), QLocale::ShortFormat);
    const QDate day = time.date();
    if (day == QDate::currentDate()) {
        return i18nc("@info Finder date", "Today at %1", clock);
    }
    if (day == QDate::currentDate().addDays(-1)) {
        return i18nc("@info Finder date", "Yesterday at %1", clock);
    }
    return i18nc("@info Finder date: day, time", "%1 at %2", locale.toString(day, QStringLiteral("dddd, MMMM d, yyyy")), clock);
}

/** A file name broken over up to @p maxLines lines of @p width (at word breaks if it can), the last elided. */
QString wrapName(const QString &name, const QFont &font, int width, int maxLines)
{
    QTextLayout layout(name, font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    layout.beginLayout();
    QStringList lines;
    for (QTextLine line = layout.createLine(); line.isValid(); line = layout.createLine()) {
        line.setLineWidth(width);
        if (lines.size() == maxLines - 1) {
            // Last line: everything left, elided in the middle.
            lines.append(QFontMetrics(font).elidedText(name.mid(line.textStart()), Qt::ElideMiddle, width));
            break;
        }
        lines.append(name.mid(line.textStart(), line.textLength()));
    }
    layout.endLayout();
    return lines.join(QLatin1Char('\n'));
}
}

FinderColumnView::FinderColumnView(QWidget *parent)
    : QColumnView(parent)
    , m_model(new KDirModel(this))
    , m_proxy(new KDirSortFilterProxyModel(this))
{
    // Created first: setModel() below already calls currentChanged(), which uses it.
    // The footer counts items, which arrive as folders list: update shortly after.
    m_statusTimer = new QTimer(this);
    m_statusTimer->setSingleShot(true);
    m_statusTimer->setInterval(120);
    connect(m_statusTimer, &QTimer::timeout, this, &FinderColumnView::currentFolderChanged);

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
    m_previewName->setWordWrap(false); // wrapName() breaks the lines
    // The preview column's scroll area paints the style's base color; keep it see-through.
    for (QWidget *w = m_preview->parentWidget(); w && w != this; w = w->parentWidget()) {
        w->setAutoFillBackground(false);
        if (auto *area = qobject_cast<QAbstractScrollArea *>(w)) {
            area->setFrameShape(QFrame::NoFrame);
            area->viewport()->setAutoFillBackground(false);
            area->setStyleSheet(QStringLiteral("QAbstractScrollArea { background: transparent; border: none; }"));
        }
    }
    connect(this, &QColumnView::updatePreviewWidget, this, &FinderColumnView::updatePreview);

    connect(this, &QAbstractItemView::activated, this, &FinderColumnView::openIndex);

    connect(m_proxy, &QAbstractItemModel::rowsInserted, m_statusTimer, qOverload<>(&QTimer::start));
    connect(m_proxy, &QAbstractItemModel::rowsRemoved, m_statusTimer, qOverload<>(&QTimer::start));
}

void FinderColumnView::setShowHiddenFiles(bool show)
{
    KDirLister *lister = m_model->dirLister();
    if (lister->showHiddenFiles() != show) {
        lister->setShowHiddenFiles(show);
        lister->emitChanges();
    }
}

void FinderColumnView::currentChanged(const QModelIndex &current, const QModelIndex &previous)
{
    QColumnView::currentChanged(current, previous);
    // The highlight follows the current item's path through every column.
    for (QAbstractItemView *column : findChildren<QAbstractItemView *>()) {
        column->viewport()->update();
    }
    if (m_statusTimer) {
        m_statusTimer->start();
    }
}

QUrl FinderColumnView::currentFolder() const
{
    // The folder of the active column: the one holding the current item.
    const QModelIndex current = currentIndex();
    if (!current.isValid() || !current.parent().isValid()) {
        return m_root;
    }
    return itemAt(current.parent()).url();
}

QString FinderColumnView::statusText() const
{
    const QModelIndex current = currentIndex();
    const QModelIndex folder = current.isValid() ? current.parent() : rootIndex();
    const int count = m_proxy->rowCount(folder);
    int selected = 0;
    for (const QModelIndex &index : selectionModel()->selectedIndexes()) {
        if (index.parent() == folder) {
            ++selected;
        }
    }
    if (selected > 0) {
        return i18nc("@info:status Finder", "%1 of %2 selected", selected, count);
    }
    return i18ncp("@info:status Finder", "1 item", "%1 items", count);
}

void FinderColumnView::showContextMenu(QAbstractItemView *column, const QPoint &pos)
{
    const QModelIndex clicked = column->indexAt(pos);
    QMenu menu(this);

    if (clicked.isValid()) {
        // Right-clicking an unselected item selects it alone, as in Finder.
        if (!selectionModel()->isSelected(clicked)) {
            setCurrentIndex(clicked);
        }
        KFileItemList items;
        QList<QUrl> files;
        for (const QModelIndex &index : selectionModel()->selectedIndexes()) {
            if (index.parent() == clicked.parent()) {
                const KFileItem item = itemAt(index);
                items.append(item);
                if (item.isLocalFile() && item.isFile()) {
                    files.append(item.url());
                }
            }
        }
        if (items.isEmpty()) {
            return;
        }

        menu.addAction(QIcon::fromTheme(QStringLiteral("document-open")), i18nc("@action:inmenu", "Open"), this, [this, clicked]() {
            openIndex(clicked);
        });
        auto *actions = new KFileItemActions(&menu);
        actions->setParentWidget(window());
        actions->setItemListProperties(KFileItemListProperties(items));
        actions->insertOpenWithActionsTo(nullptr, &menu, QStringList());
        if (!files.isEmpty() && files.size() == items.size()) {
            QMenu *share = menu.addMenu(QIcon::fromTheme(QStringLiteral("document-share")), i18nc("@action:inmenu Finder", "Share"));
            share->addAction(FinderAirDropView::icon(finderAccent()), i18nc("@action:inmenu Finder", "AirDrop…"), this, [this, files]() {
                FinderAirDrop::instance()->share(files, window());
            });
        }
        menu.addSeparator();
        menu.addAction(QIcon::fromTheme(QStringLiteral("document-properties")), i18nc("@action:inmenu Finder", "Get Info"), this, [this, items]() {
            KPropertiesDialog::showDialog(items, window(), false);
        });
        menu.addAction(QIcon::fromTheme(QStringLiteral("edit-rename")), i18nc("@action:inmenu", "Rename…"), this, [this, items]() {
            auto *dialog = new KIO::RenameFileDialog(items, window());
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->open();
        });
        menu.addAction(QIcon::fromTheme(QStringLiteral("user-trash")), i18nc("@action:inmenu", "Move to Trash"), this, [items]() {
            auto *job = new KIO::DeleteOrTrashJob(items.urlList(), KIO::AskUserActionInterface::Trash, KIO::AskUserActionInterface::DefaultConfirmation, nullptr);
            job->start();
        });
        menu.addSeparator();
        menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), i18nc("@action:inmenu", "Copy"), this, [items]() {
            auto *data = new QMimeData;
            QList<QUrl> local;
            for (const KFileItem &item : items) {
                local.append(item.mostLocalUrl());
            }
            KUrlMimeData::setUrls(items.urlList(), local, data);
            QApplication::clipboard()->setMimeData(data);
        });
        menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy-path")), i18nc("@action:inmenu", "Copy Location"), this, [items]() {
            QStringList paths;
            for (const KFileItem &item : items) {
                paths.append(item.isLocalFile() ? item.localPath() : item.url().toDisplayString());
            }
            QApplication::clipboard()->setText(paths.join(QLatin1Char('\n')));
        });
    } else {
        // Empty space: actions on the folder this column shows.
        const QModelIndex root = column->rootIndex();
        const KFileItem folder = root.isValid() ? itemAt(root) : m_model->dirLister()->rootItem();
        const QUrl url = root.isValid() ? folder.url() : m_root;
        menu.addAction(QIcon::fromTheme(QStringLiteral("folder-new")), i18nc("@action:inmenu", "New Folder…"), this, [this, url]() {
            bool ok = false;
            const QString name = QInputDialog::getText(window(), i18nc("@title:window", "New Folder"), i18nc("@label", "Name:"), QLineEdit::Normal,
                                                       i18nc("@item default folder name", "untitled folder"), &ok);
            if (ok && !name.trimmed().isEmpty()) {
                KIO::mkdir(QUrl(url.toString() + QLatin1Char('/') + name.trimmed()))->start();
            }
        });
        QAction *paste = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-paste")), i18nc("@action:inmenu", "Paste"), this, [this, url]() {
            KIO::PasteJob *job = KIO::paste(QApplication::clipboard()->mimeData(), url);
            job->setUiDelegate(KIO::createDefaultJobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, window()));
        });
        paste->setEnabled(QApplication::clipboard()->mimeData() && QApplication::clipboard()->mimeData()->hasUrls());
        menu.addSeparator();
        if (!folder.isNull()) {
            menu.addAction(QIcon::fromTheme(QStringLiteral("document-properties")), i18nc("@action:inmenu Finder", "Get Info"), this, [this, folder]() {
                KPropertiesDialog::showDialog(folder, window(), false);
            });
        }
    }
    menu.exec(column->viewport()->mapToGlobal(pos));
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
    column->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(column, &QWidget::customContextMenuRequested, this, [this, column](const QPoint &pos) {
        showContextMenu(column, pos);
    });
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
    // A real thumbnail where KDE can make one (images, videos, documents…).
    if (m_previewJob) {
        m_previewJob->kill();
    }
    if (!item.isDir()) {
        m_previewJob = KIO::filePreview(KFileItemList{item}, QSize(256, 256) * dpr);
        m_previewJob->setScaleType(KIO::PreviewJob::ScaledAndCached);
        connect(m_previewJob, &KIO::PreviewJob::gotPreview, this, [this, dpr](const KFileItem &, const QPixmap &preview) {
            QPixmap scaled = preview.scaled(QSize(220, 180) * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            scaled.setDevicePixelRatio(dpr);
            m_previewIcon->setPixmap(scaled);
        });
    }
    // Long names wrap (anywhere, if they have no spaces) instead of running off the edges.
    const int nameWidth = m_preview->width() - m_preview->layout()->contentsMargins().left() - m_preview->layout()->contentsMargins().right() - 8;
    m_previewName->setText(wrapName(item.text(), m_previewName->font(), std::max(120, nameWidth), 3));
    m_previewKind->setText(item.isDir() ? i18nc("@info Finder preview", "Folder")
                                        : i18nc("@info Finder preview: kind - size", "%1 – %2", item.mimeComment(), finderSize(item.size())));
    const QLocale locale;
    const auto row = [](const QString &label, const QString &value) {
        return QStringLiteral("<tr><td style='color: rgba(255,255,255,0.55); padding-right: 10px;'>%1</td><td>%2</td></tr>")
            .arg(label.toHtmlEscaped(), value.toHtmlEscaped());
    };
    QString html = QStringLiteral("<b>") + i18nc("@title Finder preview", "Information") + QStringLiteral("</b><table style='margin-top: 6px;'>");
    const QDateTime created = item.time(KFileItem::CreationTime);
    if (created.isValid()) {
        html += row(i18nc("@label Finder preview", "Created"), finderDate(created));
    }
    html += row(i18nc("@label Finder preview", "Modified"), finderDate(item.time(KFileItem::ModificationTime)));
    html += QStringLiteral("</table>");
    m_previewDetails->setText(html);
}

#include "moc_findercolumnview.cpp"
