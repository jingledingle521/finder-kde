/*
 * SPDX-FileCopyrightText: 2026 Brhett
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef FINDERCHROME_H
#define FINDERCHROME_H

#include <QObject>
#include <QPointer>

class KWindowShadow;
class QMainWindow;
class QToolBar;
class QWidget;

/**
 * @brief macOS Golden Gate style window chrome for Finder.
 *
 * Replaces the window manager's title bar: the window is frameless and
 * translucent, paints its own rounded background (with a glass sidebar that
 * KWin blurs), asks KWin for a native shadow, lets the user move the window by
 * dragging empty toolbar space and resize it from its edges, and puts the
 * stoplights (close, minimize, zoom) at the start of the toolbar.
 */
class FinderChrome : public QObject
{
    Q_OBJECT

public:
    explicit FinderChrome(QMainWindow *window);
    ~FinderChrome() override;

    /** The toolbar that acts as the title bar. Call after the GUI is built. */
    void setToolBar(QToolBar *toolBar);

    /** The sidebar dock, whose area gets the glass treatment. */
    void setSidebar(QWidget *sidebar);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void paintBackground();
    void updateWindowEffects();
    void updateShadow();
    Qt::Edges edgesAt(const QPointF &pos) const;
    bool isMaximized() const;
    int radius() const;

    QMainWindow *m_window;
    QPointer<QToolBar> m_toolBar;
    QPointer<QWidget> m_sidebar;
    QPointer<QWidget> m_stoplights;
    KWindowShadow *m_shadow = nullptr;
    Qt::Edges m_cursorEdges;
};

#endif
