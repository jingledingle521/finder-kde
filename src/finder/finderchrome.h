/*
 * SPDX-FileCopyrightText: 2026 Brhett
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef FINDERCHROME_H
#define FINDERCHROME_H

#include <QIcon>
#include <QObject>
#include <QPointer>
#include <QRegion>
#include <QUrl>

class FinderAirDropView;
class FinderColumnView;
class FinderSidebar;
class KWindowShadow;
class QMainWindow;
class QAction;
class QLabel;
class QPainter;
class QToolBar;
class QToolButton;
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
    QToolButton *createMoreButton(QToolBar *toolBar);
    static QIcon groupIcon();
    static QIcon columnsIcon();
    /** Finder's column view on or off (laid over the active Dolphin view). */
    void setColumnMode(bool on);
    void attachColumns();
    void leaveColumnMode();
    /** Shows the AirDrop panel over the view; @p files are offered for sending. */
    void showAirDrop(const QList<QUrl> &files);
    void hideAirDrop();
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
    QAction *m_columnsAction = nullptr;
    QPointer<FinderColumnView> m_columns;
    QPointer<FinderAirDropView> m_airDrop;
    QPointer<FinderSidebar> m_finderSidebar;
    bool m_columnMode = false;
    KWindowShadow *m_shadow = nullptr;
    QRegion m_appliedBlur; // last blur region sent to KWin
    bool m_blurApplied = false;
    bool m_surfaceReady = false; // effects sent for the current native surface
    Qt::Edges m_cursorEdges;
};

#endif
