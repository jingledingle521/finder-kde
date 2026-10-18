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
class QAction;
class QLabel;
class QPainter;
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
    void updateTitle();
    void updateTitleColor();
    void updateLead();
    void paintCapsules(QPainter &p);
    void watchNavigators();
    void updateNavigatorVisibility();
    void updateWindowEffects();
    void updateShadow();
    Qt::Edges edgesAt(const QPointF &pos) const;
    bool isMaximized() const;
    int radius() const;

    QMainWindow *m_window;
    QPointer<QToolBar> m_toolBar;
    QPointer<QWidget> m_sidebar;
    QPointer<QWidget> m_stoplights;
    QPointer<QLabel> m_title;
    QPointer<QWidget> m_lead; // spacer from the stoplights to the content edge
    QPointer<QWidget> m_navigator; // the hidden breadcrumb
    QPointer<QAction> m_navigatorAction;
    KWindowShadow *m_shadow = nullptr;
    Qt::Edges m_cursorEdges;
};

#endif
