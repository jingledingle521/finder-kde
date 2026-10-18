/*
 * SPDX-FileCopyrightText: 2014 Emmanuel Pescosta <emmanuelpescosta099@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "dolphintabbar.h"
#include "dolphin_generalsettings.h"
#include <KLocalizedString>

#include <QDragEnterEvent>
#include <QInputDialog>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>

class PreventFocusWhileHidden : public QObject
{
public:
    PreventFocusWhileHidden(QObject *parent)
        : QObject(parent){};

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override
    {
        switch (ev->type()) {
        case QEvent::Hide:
            static_cast<QWidget *>(obj)->setFocusPolicy(Qt::NoFocus);
            return false;
        case QEvent::Show:
            static_cast<QWidget *>(obj)->setFocusPolicy(Qt::TabFocus);
            return false;
        default:
            return false;
        }
    };
};

DolphinTabBar::DolphinTabBar(QWidget *parent)
    : QTabBar(parent)
    , m_autoActivationIndex(-1)
{
    setAcceptDrops(true);
    setSelectionBehaviorOnRemove(QTabBar::SelectPreviousTab);
    setMovable(true);
    setTabsClosable(true);
    setMouseTracking(true); // Finder: hover shows the tab's ×
    setDrawBase(false);

    setFocusPolicy(Qt::NoFocus);
    installEventFilter(new PreventFocusWhileHidden(this));

    m_autoActivationTimer = new QTimer(this);
    m_autoActivationTimer->setSingleShot(true);
    m_autoActivationTimer->setInterval(800);
    connect(m_autoActivationTimer, &QTimer::timeout, this, &DolphinTabBar::slotAutoActivationTimeout);
    connect(GeneralSettings::self(), &GeneralSettings::tabBarChanged, this, &DolphinTabBar::slotTabBarChanged);

    QTimer::singleShot(0, this, &DolphinTabBar::slotTabBarChanged);
}

QSize DolphinTabBar::tabSizeHint(int index) const
{
    // Finder: tabs share the bar's width equally, at a fixed height.
    if (count() > 0) {
        return QSize(qMax(60, width() / count()), 34);
    }
    if (GeneralSettings::tabStyle() == GeneralSettings::EnumTabStyle::FixedSize) {
        QSize defaultSize = QTabBar::tabSizeHint(index);
        defaultSize.setWidth(225);
        return defaultSize;
    } else if (GeneralSettings::tabStyle() == GeneralSettings::EnumTabStyle::FullWidth && count() > 0) {
        QSize defaultSize = QTabBar::tabSizeHint(index);
        defaultSize.setWidth(qMax(25, width() / count()));
        return defaultSize;
    }
    return QTabBar::tabSizeHint(index);
}

QSize DolphinTabBar::minimumSizeHint() const
{
    // Finder: tabs size to the bar (see tabSizeHint), so the bar must never
    // ask for their sum, or the window keeps growing.
    QSize s = QTabBar::minimumSizeHint();
    s.setWidth(0);
    return s;
}

void DolphinTabBar::dragEnterEvent(QDragEnterEvent *event)
{
    const QMimeData *mimeData = event->mimeData();
    const int index = tabAt(event->position().toPoint());

    if (mimeData->hasUrls()) {
        event->acceptProposedAction();
        updateAutoActivationTimer(index);
    }

    QTabBar::dragEnterEvent(event);
}

void DolphinTabBar::dragLeaveEvent(QDragLeaveEvent *event)
{
    updateAutoActivationTimer(-1);

    QTabBar::dragLeaveEvent(event);
}

void DolphinTabBar::dragMoveEvent(QDragMoveEvent *event)
{
    const QMimeData *mimeData = event->mimeData();
    const int index = tabAt(event->position().toPoint());

    if (mimeData->hasUrls()) {
        Q_EMIT tabDragMoveEvent(index, event);
        updateAutoActivationTimer(index);
    }

    QTabBar::dragMoveEvent(event);
}

void DolphinTabBar::dropEvent(QDropEvent *event)
{
    // Disable the auto activation timer
    updateAutoActivationTimer(-1);

    const QMimeData *mimeData = event->mimeData();
    const int index = tabAt(event->position().toPoint());

    if (mimeData->hasUrls()) {
        Q_EMIT tabDropEvent(index, event);
    }

    QTabBar::dropEvent(event);
}

QRect DolphinTabBar::closeRect(int index) const
{
    const QRect tab = tabRect(index);
    return QRect(tab.left() + 12, tab.center().y() - 8, 16, 16);
}

void DolphinTabBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // The strip: a faint rounded track the tabs sit in.
    const QRectF track = QRectF(rect()).adjusted(8, 3, -4, -3);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 12));
    p.drawRoundedRect(track, track.height() / 2, track.height() / 2);

    const int current = currentIndex();
    for (int i = 0; i < count(); ++i) {
        const QRectF tab = QRectF(tabRect(i)).intersected(track).adjusted(2, 2, -2, -2);
        const bool selected = i == current;
        const bool hovered = i == m_hoveredTab || i == m_autoActivationIndex;
        if (selected || hovered) {
            p.setPen(Qt::NoPen);
            p.setBrush(selected ? QColor(255, 255, 255, 34) : QColor(255, 255, 255, 14));
            p.drawRoundedRect(tab, tab.height() / 2, tab.height() / 2);
        } else if (i + 1 < count() && i + 1 != current && i != current) {
            // Thin divider between two unselected tabs.
            p.fillRect(QRectF(tab.right() + 1.5, tab.top() + 7, 1, tab.height() - 14), QColor(255, 255, 255, 30));
        }

        // Title, centered; the × takes its place on the left while hovered.
        p.setPen(selected ? QColor(255, 255, 255, 235) : QColor(255, 255, 255, 150));
        p.setFont(font());
        const QRectF textRect = tab.adjusted(30, 0, -30, 0);
        p.drawText(textRect, Qt::AlignCenter, fontMetrics().elidedText(tabText(i).replace(QLatin1String("&&"), QLatin1String("&")), Qt::ElideMiddle, int(textRect.width())));

        if (hovered && count() > 1) {
            const QRectF x = QRectF(closeRect(i));
            if (m_pressedClose == i) {
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(255, 255, 255, 40));
                p.drawEllipse(x);
            }
            p.setPen(QPen(QColor(255, 255, 255, 200), 1.4, Qt::SolidLine, Qt::RoundCap));
            const QPointF c = x.center();
            p.drawLine(c + QPointF(-3.5, -3.5), c + QPointF(3.5, 3.5));
            p.drawLine(c + QPointF(-3.5, 3.5), c + QPointF(3.5, -3.5));
        }
    }
}

void DolphinTabBar::mouseMoveEvent(QMouseEvent *event)
{
    const int index = tabAt(event->pos());
    if (index != m_hoveredTab) {
        m_hoveredTab = index;
        update();
    }
    QTabBar::mouseMoveEvent(event);
}

void DolphinTabBar::leaveEvent(QEvent *event)
{
    m_hoveredTab = -1;
    m_pressedClose = -1;
    update();
    QTabBar::leaveEvent(event);
}

void DolphinTabBar::mousePressEvent(QMouseEvent *event)
{
    const int index = tabAt(event->pos());

    if (index >= 0 && event->button() == Qt::LeftButton && count() > 1 && closeRect(index).contains(event->pos())) {
        m_pressedClose = index; // closes on release, if still over the ×
        update();
        return;
    }

    if (index >= 0 && event->button() == Qt::MiddleButton) {
        m_tabToBeClosedOnMiddleMouseButtonRelease = index;
        return;
    }

    QTabBar::mousePressEvent(event);
}

void DolphinTabBar::mouseReleaseEvent(QMouseEvent *event)
{
    const int index = tabAt(event->pos());

    if (m_pressedClose >= 0 && event->button() == Qt::LeftButton) {
        const int pressed = m_pressedClose;
        m_pressedClose = -1;
        update();
        if (index == pressed && closeRect(index).contains(event->pos())) {
            Q_EMIT tabCloseRequested(index);
        }
        return;
    }

    if (index >= 0 && index == m_tabToBeClosedOnMiddleMouseButtonRelease && event->button() == Qt::MiddleButton) {
        // Mouse middle click on a tab closes this tab.
        Q_EMIT tabCloseRequested(index);
        return;
    }

    QTabBar::mouseReleaseEvent(event);
}

void DolphinTabBar::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->buttons() & Qt::LeftButton) {
        int index = tabAt(event->pos());

        if (index < 0) {
            // empty tabbar area case
            index = currentIndex();
        }
        // Double left click on the tabbar opens a new activated tab
        // with the url from the doubleclicked tab or currentTab otherwise.
        Q_EMIT openNewActivatedTab(index);
    }

    QTabBar::mouseDoubleClickEvent(event);
}

void DolphinTabBar::contextMenuEvent(QContextMenuEvent *event)
{
    const int index = tabAt(event->pos());

    if (index >= 0) {
        // Tab context menu
        QMenu menu(this);

        QAction *newTabAction = menu.addAction(QIcon::fromTheme(QStringLiteral("tab-new")), i18nc("@action:inmenu", "New Tab"));
        QAction *detachTabAction = menu.addAction(QIcon::fromTheme(QStringLiteral("tab-detach")), i18nc("@action:inmenu", "Detach Tab"));
        menu.addSeparator();
        QAction *renameTabAction = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-rename")), i18nc("@action:inmenu", "Rename Tab"));
        menu.addSeparator();
        QAction *closeOtherTabsAction = menu.addAction(QIcon::fromTheme(QStringLiteral("tab-close-other")), i18nc("@action:inmenu", "Close Other Tabs"));
        QAction *closeTabsToLeftAction = menu.addAction(QIcon::fromTheme(QStringLiteral("go-previous")), i18nc("@action:inmenu", "Close Tabs to the Left"));
        closeTabsToLeftAction->setEnabled(index > 0);
        QAction *closeTabsToRightAction = menu.addAction(QIcon::fromTheme(QStringLiteral("go-next")), i18nc("@action:inmenu", "Close Tabs to the Right"));
        closeTabsToRightAction->setEnabled(index < count() - 1);
        QAction *closeTabAction = menu.addAction(QIcon::fromTheme(QStringLiteral("tab-close")), i18nc("@action:inmenu", "Close Tab"));

        QAction *selectedAction = menu.exec(event->globalPos());
        if (selectedAction == newTabAction) {
            Q_EMIT openNewActivatedTab(index);
        } else if (selectedAction == detachTabAction) {
            Q_EMIT tabDetachRequested(index);
        } else if (selectedAction == closeOtherTabsAction) {
            const int tabCount = count();
            for (int i = 0; i < index; i++) {
                Q_EMIT tabCloseRequested(0);
            }
            for (int i = index + 1; i < tabCount; i++) {
                Q_EMIT tabCloseRequested(1);
            }
        } else if (selectedAction == closeTabsToLeftAction) {
            for (int i = 0; i < index; i++) {
                Q_EMIT tabCloseRequested(0);
            }
        } else if (selectedAction == closeTabsToRightAction) {
            const int tabCount = count();
            for (int i = index + 1; i < tabCount; i++) {
                Q_EMIT tabCloseRequested(index + 1);
            }
        } else if (selectedAction == closeTabAction) {
            Q_EMIT tabCloseRequested(index);
        } else if (selectedAction == renameTabAction) {
            bool renamed = false;
            const QString tabNewName = QInputDialog::getText(this, i18nc("@title:window for text input", "Rename Tab"), i18n("New tab name:"), QLineEdit::Normal, tabText(index), &renamed);

            if (renamed) {
                Q_EMIT tabRenamed(index, tabNewName);
            }
        }

        return;
    }

    QTabBar::contextMenuEvent(event);
}

void DolphinTabBar::slotTabBarChanged()
{
    // Finder: always full width, with our own × (not Qt's close buttons);
    // the tab style and close-button settings don't apply.
    setTabsClosable(false);
    setExpanding(true);
    setUsesScrollButtons(false);
    updateGeometry();
}

void DolphinTabBar::slotAutoActivationTimeout()
{
    if (m_autoActivationIndex >= 0) {
        setCurrentIndex(m_autoActivationIndex);
        updateAutoActivationTimer(-1);
    }
}

void DolphinTabBar::updateAutoActivationTimer(const int index)
{
    if (m_autoActivationIndex != index) {
        m_autoActivationIndex = index;

        if (m_autoActivationIndex < 0) {
            m_autoActivationTimer->stop();
        } else {
            m_autoActivationTimer->start();
        }
    }
}

#include "moc_dolphintabbar.cpp"
