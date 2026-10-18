/*
 * SPDX-FileCopyrightText: 2026 Brhett
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef FINDERCOLUMNVIEW_H
#define FINDERCOLUMNVIEW_H

#include <QColumnView>
#include <QUrl>

class KDirModel;
class KDirSortFilterProxyModel;
class KFileItem;
class QLabel;
class QTimer;

/**
 * @brief Finder's column view (Miller columns).
 *
 * Each column lists a folder; selecting a folder opens its contents in the
 * next column, and selecting a file shows a preview column with its icon and
 * details. Dolphin has no such mode, so Finder lays this view over the active
 * Dolphin view while the column mode is on.
 */
class FinderColumnView : public QColumnView
{
    Q_OBJECT

public:
    explicit FinderColumnView(QWidget *parent = nullptr);

    /** Shows @p url in the first column. */
    void setRootUrl(const QUrl &url);
    QUrl rootUrl() const;

    /** Lists dot-files too (follows Dolphin's "Show Hidden Files"). */
    void setShowHiddenFiles(bool show);

    /** The folder the user is working in (the active column) and the footer text for it. */
    QUrl currentFolder() const;
    QString statusText() const;

Q_SIGNALS:
    /** A folder was double-clicked: the window should go there. */
    void folderActivated(const QUrl &url);
    /** The active column or its selection changed: new title and footer. */
    void currentFolderChanged();

protected:
    QAbstractItemView *createColumn(const QModelIndex &rootIndex) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void currentChanged(const QModelIndex &current, const QModelIndex &previous) override;

private:
    KFileItem itemAt(const QModelIndex &index) const;
    void openIndex(const QModelIndex &index);
    void updatePreview(const QModelIndex &index);
    /** Finder's context menu for the items (or the empty space) under @p pos in @p column. */
    void showContextMenu(QAbstractItemView *column, const QPoint &pos);

    KDirModel *m_model;
    KDirSortFilterProxyModel *m_proxy;
    QUrl m_root;
    QTimer *m_statusTimer; // coalesces footer updates while folders list

    QWidget *m_preview;
    QLabel *m_previewIcon;
    QLabel *m_previewName;
    QLabel *m_previewKind;
    QLabel *m_previewDetails;
};

#endif
