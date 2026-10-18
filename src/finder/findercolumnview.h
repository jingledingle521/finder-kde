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

Q_SIGNALS:
    /** A folder was double-clicked: the window should go there. */
    void folderActivated(const QUrl &url);

protected:
    QAbstractItemView *createColumn(const QModelIndex &rootIndex) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    KFileItem itemAt(const QModelIndex &index) const;
    void openIndex(const QModelIndex &index);
    void updatePreview(const QModelIndex &index);

    KDirModel *m_model;
    KDirSortFilterProxyModel *m_proxy;
    QUrl m_root;

    QWidget *m_preview;
    QLabel *m_previewIcon;
    QLabel *m_previewName;
    QLabel *m_previewKind;
    QLabel *m_previewDetails;
};

#endif
