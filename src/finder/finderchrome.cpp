/*
 * SPDX-FileCopyrightText: 2026 Brhett
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "finderchrome.h"

#include <KUrlComboBox>
#include <KUrlNavigator>
#include <QFocusEvent>
#include <QLineEdit>
#include <KWindowEffects>
#include <KWindowShadow>

#include <QApplication>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolBar>
#include <QWindow>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
constexpr int WindowRadius = 12; // macOS window corner radius
constexpr int ResizeBorder = 6; // grab zone along the edges, inside the window
constexpr int ShadowSize = 32; // how far the shadow reaches outside the window

const QColor ContentColor(32, 32, 37); // Golden Gate dark content, slightly cool
const QColor SidebarColor(44, 44, 48, 200); // translucent: KWin blurs behind it
const QColor HairlineDark(0, 0, 0, 90);
const QColor HairlineLight(255, 255, 255, 18);
const QColor CapsuleFill(255, 255, 255, 20); // glass capsules behind toolbar buttons
const QColor CapsuleRim(255, 255, 255, 26);
constexpr int CapsuleHeight = 34;
constexpr int CapsulePad = 4; // between a capsule's rim and the buttons inside
constexpr int CapsuleGap = 10; // between neighboring capsules

// Box-blurs the alpha channel of a premultiplied ARGB image (three passes of
// a box blur approximate a Gaussian).
void blurAlpha(QImage &image, int radius)
{
    const int w = image.width();
    const int h = image.height();
    std::vector<int> alpha(w * h);
    for (int y = 0; y < h; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            alpha[y * w + x] = qAlpha(line[x]);
        }
    }
    std::vector<int> tmp(w * h);
    auto pass = [&](bool horizontal) {
        const int len = horizontal ? w : h;
        const int lines = horizontal ? h : w;
        for (int l = 0; l < lines; ++l) {
            auto at = [&](int i) -> int & {
                return horizontal ? alpha[l * w + i] : alpha[i * w + l];
            };
            auto out = [&](int i) -> int & {
                return horizontal ? tmp[l * w + i] : tmp[i * w + l];
            };
            int sum = 0;
            for (int i = -radius; i <= radius; ++i) {
                sum += at(std::clamp(i, 0, len - 1));
            }
            for (int i = 0; i < len; ++i) {
                out(i) = sum / (2 * radius + 1);
                sum += at(std::clamp(i + radius + 1, 0, len - 1)) - at(std::clamp(i - radius, 0, len - 1));
            }
        }
        alpha.swap(tmp);
    };
    for (int i = 0; i < 3; ++i) {
        pass(true);
        pass(false);
    }
    for (int y = 0; y < h; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const int a = alpha[y * w + x];
            line[x] = qRgba(0, 0, 0, a);
        }
    }
}

QPainterPath roundedPath(const QRectF &rect, qreal radius)
{
    QPainterPath path;
    path.addRoundedRect(rect, radius, radius);
    return path;
}

/**
 * The macOS stoplights: close, minimize and zoom. Glyphs appear while the
 * pointer is over the group; they turn gray when the window is inactive.
 */
class Stoplights : public QWidget
{
public:
    explicit Stoplights(QMainWindow *window)
        : QWidget(window)
        , m_window(window)
    {
        setMouseTracking(true);
        setAttribute(Qt::WA_Hover);
        setFixedSize(LeftMargin + 3 * Diameter + 2 * Gap + RightMargin, Diameter + 8);
        setToolTip(QString());
    }

protected:
    static constexpr int Diameter = 12;
    static constexpr int Gap = 8;
    static constexpr int LeftMargin = 8;
    static constexpr int RightMargin = 14;

    QRectF circle(int i) const
    {
        const qreal y = (height() - Diameter) / 2.0;
        return QRectF(LeftMargin + i * (Diameter + Gap), y, Diameter, Diameter);
    }

    int buttonAt(const QPointF &pos) const
    {
        for (int i = 0; i < 3; ++i) {
            if (circle(i).adjusted(-Gap / 2, -4, Gap / 2, 4).contains(pos)) {
                return i;
            }
        }
        return -1;
    }

    void paintEvent(QPaintEvent *) override
    {
        static const QColor colors[] = {QColor(0xff, 0x5f, 0x57), QColor(0xfe, 0xbc, 0x2e), QColor(0x28, 0xc8, 0x40)};
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const bool active = m_window->isActiveWindow() || m_hover;
        for (int i = 0; i < 3; ++i) {
            const QRectF c = circle(i);
            QColor fill = active ? colors[i] : QColor(80, 80, 84);
            if (m_pressed == i) {
                fill = fill.darker(125);
            }
            p.setPen(QPen(QColor(0, 0, 0, 50), 0.5));
            p.setBrush(fill);
            p.drawEllipse(c);
            if (!m_hover) {
                continue;
            }
            // Glyphs, drawn rather than taken from a font so they stay crisp.
            p.setPen(QPen(QColor(0, 0, 0, 150), 1.2, Qt::SolidLine, Qt::RoundCap));
            const QPointF m = c.center();
            const qreal k = 2.6;
            if (i == 0) {
                p.drawLine(m + QPointF(-k, -k), m + QPointF(k, k));
                p.drawLine(m + QPointF(-k, k), m + QPointF(k, -k));
            } else if (i == 1) {
                p.drawLine(m + QPointF(-k - 0.4, 0), m + QPointF(k + 0.4, 0));
            } else {
                // Zoom: two small triangles pointing apart, as on macOS.
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(0, 0, 0, 150));
                QPolygonF a{m + QPointF(-3.2, -3.2), m + QPointF(1.2, -3.2), m + QPointF(-3.2, 1.2)};
                QPolygonF b{m + QPointF(3.2, 3.2), m + QPointF(-1.2, 3.2), m + QPointF(3.2, -1.2)};
                p.drawPolygon(a);
                p.drawPolygon(b);
            }
        }
    }

    bool event(QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::HoverEnter:
        case QEvent::HoverMove:
            if (!m_hover) {
                m_hover = true;
                update();
            }
            break;
        case QEvent::HoverLeave:
            m_hover = false;
            m_pressed = -1;
            update();
            break;
        default:
            break;
        }
        return QWidget::event(event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            m_pressed = buttonAt(event->position());
            update();
        }
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        const int button = buttonAt(event->position());
        const int pressed = m_pressed;
        m_pressed = -1;
        update();
        if (event->button() != Qt::LeftButton || button != pressed || button < 0) {
            return;
        }
        if (button == 0) {
            m_window->close();
        } else if (button == 1) {
            m_window->showMinimized();
        } else if (m_window->isMaximized()) {
            m_window->showNormal();
        } else {
            m_window->showMaximized();
        }
    }

private:
    QMainWindow *m_window;
    bool m_hover = false;
    int m_pressed = -1;
};
}

FinderChrome::FinderChrome(QMainWindow *window)
    : QObject(window)
    , m_window(window)
{
    window->setWindowFlag(Qt::FramelessWindowHint, true);
    window->setAttribute(Qt::WA_TranslucentBackground, true);

    // Views and lists draw on transparent backgrounds so the window's own
    // rounded background (and the glass sidebar) shows through.
    QPalette pal = window->palette();
    pal.setColor(QPalette::Base, Qt::transparent);
    window->setPalette(pal);

    window->installEventFilter(this);

}

FinderChrome::~FinderChrome() = default;

void FinderChrome::setToolBar(QToolBar *toolBar)
{
    if (m_toolBar == toolBar) {
        return;
    }
    if (m_toolBar) {
        m_toolBar->removeEventFilter(this);
    }
    m_toolBar = toolBar;
    if (!toolBar) {
        return;
    }
    toolBar->setMovable(false);
    toolBar->installEventFilter(this);
    // Let the window's own background (and rounded corners) show through.
    toolBar->setAutoFillBackground(false);
    toolBar->setStyleSheet(QStringLiteral("QToolBar { background: transparent; border: none; }"
                                         "QToolButton { background: transparent; border: none; border-radius: 13px;"
                                         " padding: 0px; margin: 8px 0px; min-width: 38px; min-height: 26px; max-height: 26px; }"
                                         "QToolButton:hover { background: rgba(255, 255, 255, 20); }"
                                         "QToolButton:checked, QToolButton:pressed { background: rgba(255, 255, 255, 38); }"
                                         "QToolButton::menu-indicator { image: none; width: 0; }"));
    if (!m_stoplights) {
        m_stoplights = new Stoplights(m_window);
    }
    // The stoplights lead the toolbar, like the title bar on macOS.
    const QList<QAction *> actions = toolBar->actions();
    toolBar->insertWidget(actions.isEmpty() ? nullptr : actions.first(), m_stoplights);
    // The stoplights sit over the sidebar; the rest of the toolbar starts
    // where the content does.
    m_lead = new QWidget(toolBar);
    m_lead->setAttribute(Qt::WA_TransparentForMouseEvents);
    toolBar->insertWidget(actions.isEmpty() ? nullptr : actions.first(), m_lead);
    updateLead();

    QAction *navigators = nullptr;
    for (QAction *action : actions) {
        const QString name = action->objectName();
        if (name == QLatin1String("go_back") || name == QLatin1String("go_forward")) {
            // Finder's back/forward have no menu arrow; holding shows history.
            if (auto *button = qobject_cast<QToolButton *>(toolBar->widgetForAction(action))) {
                button->setPopupMode(QToolButton::DelayedPopup);
            }
        } else if (name == QLatin1String("url_navigators")) {
            navigators = action;
        }
    }
    // Breathing room between capsules and at the right edge.
    auto gap = [toolBar](int width) {
        auto *w = new QWidget(toolBar);
        w->setFixedWidth(width);
        w->setAttribute(Qt::WA_TransparentForMouseEvents);
        return w;
    };
    for (QAction *action : actions) {
        const QString name = action->objectName();
        if (name == QLatin1String("icons") || name == QLatin1String("toggle_search")) {
            toolBar->insertWidget(action, gap(CapsuleGap + 2 * CapsulePad));
        }
        if (name == QLatin1String("go_back")) {
            toolBar->insertWidget(action, gap(CapsulePad));
        }
        if (name == QLatin1String("toggle_search")) {
            // Search is a round button on its own.
            if (QWidget *button = toolBar->widgetForAction(action)) {
                button->setFixedWidth(CapsuleHeight - 2 * CapsulePad);
            }
        }
    }
    toolBar->addWidget(gap(CapsulePad + 8));

    if (!navigators) {
        return;
    }

    // The folder name as a bold title in place of the breadcrumb. The
    // breadcrumb stays in the toolbar (hidden) so Dolphin doesn't move it
    // above the view, and comes back while a path is being typed (Ctrl+L or
    // a click on the title).
    m_navigator = toolBar->widgetForAction(navigators);
    m_navigatorAction = navigators;
    auto *title = new QLabel(toolBar);
    QFont font = title->font();
    font.setWeight(QFont::DemiBold);
    font.setPointSizeF(font.pointSizeF() * 1.3);
    title->setFont(font);
    title->setContentsMargins(12, 0, 12, 0);
    title->setCursor(Qt::IBeamCursor);
    title->installEventFilter(this);
    m_title = title;
    toolBar->insertWidget(navigators, title);

    auto *spacer = new QWidget(toolBar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    spacer->setAttribute(Qt::WA_TransparentForMouseEvents); // drags the window
    const int next = actions.indexOf(navigators) + 1;
    toolBar->insertWidget(next < actions.size() ? actions.at(next) : nullptr, spacer);

    connect(m_window, &QWidget::windowTitleChanged, this, &FinderChrome::updateTitle, Qt::UniqueConnection);
    updateTitle();
    watchNavigators();
}

void FinderChrome::updateTitle()
{
    if (!m_title) {
        return;
    }
    // Dolphin titles windows "Folder — App"; Finder shows just the folder.
    QString text = m_window->windowTitle();
    const int dash = text.lastIndexOf(QStringLiteral(" — "));
    if (dash > 0) {
        text = text.left(dash);
    }
    m_title->setText(text);
    updateTitleColor();
}

void FinderChrome::updateTitleColor()
{
    if (!m_title) {
        return;
    }
    // Bright when the window is active, gray when it's in the background.
    QPalette pal = m_title->palette();
    pal.setColor(QPalette::WindowText, m_window->isActiveWindow() ? QColor(255, 255, 255, 235) : QColor(255, 255, 255, 110));
    m_title->setPalette(pal);
}

void FinderChrome::updateLead()
{
    if (!m_lead || !m_stoplights || !m_toolBar) {
        return;
    }
    int width = 0;
    if (m_sidebar && m_sidebar->isVisible()) {
        const int sidebarRight = m_sidebar->mapTo(m_window, QPoint(m_sidebar->width(), 0)).x();
        const int stoplightsRight = m_toolBar->mapTo(m_window, QPoint(0, 0)).x() + m_toolBar->contentsMargins().left()
            + m_toolBar->layout()->contentsMargins().left() + m_stoplights->width();
        width = std::max(0, sidebarRight + 6 - stoplightsRight);
    }
    if (m_lead->width() != width) {
        m_lead->setFixedWidth(width);
    }
}

void FinderChrome::paintCapsules(QPainter &p)
{
    // Finder groups toolbar buttons into glass capsules: back/forward,
    // the view modes, and search.
    static const QList<QStringList> groups = {
        {QStringLiteral("go_back"), QStringLiteral("go_forward")},
        {QStringLiteral("icons"), QStringLiteral("compact"), QStringLiteral("details")},
        {QStringLiteral("toggle_search")},
    };
    p.setRenderHint(QPainter::Antialiasing);
    for (const QStringList &group : groups) {
        QRect box;
        QList<QRect> parts;
        for (QAction *action : m_toolBar->actions()) {
            if (!group.contains(action->objectName()) || !action->isVisible()) {
                continue;
            }
            QWidget *button = m_toolBar->widgetForAction(action);
            if (button && button->isVisible()) {
                parts.append(button->geometry());
                box = box.united(button->geometry());
            }
        }
        if (box.isEmpty()) {
            continue;
        }
        QRectF capsule(box);
        capsule.setTop(box.center().y() - CapsuleHeight / 2.0 + 0.5);
        capsule.setHeight(CapsuleHeight);
        capsule.adjust(-CapsulePad, 0, CapsulePad, 0);
        if (capsule.width() < capsule.height()) { // a lone round button
            capsule.adjust(-(capsule.height() - capsule.width()) / 2, 0, (capsule.height() - capsule.width()) / 2, 0);
        }
        p.setPen(QPen(CapsuleRim, 1));
        p.setBrush(CapsuleFill);
        const qreal r = capsule.height() / 2;
        p.drawRoundedRect(capsule.adjusted(0.5, 0.5, -0.5, -0.5), r, r);
        if (group.first() == QLatin1String("go_back") && parts.size() == 2) {
            // The thin divider between back and forward.
            const qreal x = (parts[0].right() + parts[1].left() + 1) / 2.0;
            p.setPen(QPen(CapsuleRim, 1));
            p.drawLine(QPointF(x, capsule.top() + 8), QPointF(x, capsule.bottom() - 8));
        }
    }
}

void FinderChrome::watchNavigators()
{
    if (!m_navigator) {
        return;
    }
    for (KUrlNavigator *navigator : m_navigator->findChildren<KUrlNavigator *>()) {
        connect(navigator, &KUrlNavigator::editableStateChanged, this, &FinderChrome::updateNavigatorVisibility, Qt::UniqueConnection);
        // Typing a path is momentary: back to the title once it's entered.
        connect(
            navigator,
            &KUrlNavigator::returnPressed,
            navigator,
            [navigator] {
                navigator->setUrlEditable(false);
            },
            Qt::QueuedConnection);
        navigator->editor()->lineEdit()->installEventFilter(this);
    }
    updateNavigatorVisibility();
}

void FinderChrome::updateNavigatorVisibility()
{
    if (!m_navigator || !m_title) {
        return;
    }
    bool editing = false;
    for (KUrlNavigator *navigator : m_navigator->findChildren<KUrlNavigator *>()) {
        editing = editing || navigator->isUrlEditable();
    }
    // Hiding the action (not the widget) keeps Dolphin from re-showing it.
    if (m_navigatorAction) {
        m_navigatorAction->setVisible(editing);
    }
    m_title->setVisible(!editing);
}

void FinderChrome::setSidebar(QWidget *sidebar)
{
    m_sidebar = sidebar;
    if (sidebar) {
        sidebar->installEventFilter(this);
        // Only the window paints the glass; the dock itself stays clear.
        sidebar->setAutoFillBackground(false);
        sidebar->setStyleSheet(QStringLiteral("QDockWidget { background: transparent; border: none; }"));
    }
    updateWindowEffects();
}

bool FinderChrome::isMaximized() const
{
    return m_window->windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen);
}

int FinderChrome::radius() const
{
    return isMaximized() ? 0 : WindowRadius;
}

Qt::Edges FinderChrome::edgesAt(const QPointF &pos) const
{
    Qt::Edges edges;
    if (isMaximized()) {
        return edges;
    }
    const int corner = WindowRadius;
    const QRectF r = m_window->rect();
    if (pos.x() < ResizeBorder) {
        edges |= Qt::LeftEdge;
    } else if (pos.x() > r.width() - ResizeBorder) {
        edges |= Qt::RightEdge;
    }
    if (pos.y() < ResizeBorder) {
        edges |= Qt::TopEdge;
    } else if (pos.y() > r.height() - ResizeBorder) {
        edges |= Qt::BottomEdge;
    }
    // Generous diagonal grab near the corners.
    if (edges && (edges == Qt::LeftEdge || edges == Qt::RightEdge)) {
        if (pos.y() < corner) {
            edges |= Qt::TopEdge;
        } else if (pos.y() > r.height() - corner) {
            edges |= Qt::BottomEdge;
        }
    } else if (edges && (edges == Qt::TopEdge || edges == Qt::BottomEdge)) {
        if (pos.x() < corner) {
            edges |= Qt::LeftEdge;
        } else if (pos.x() > r.width() - corner) {
            edges |= Qt::RightEdge;
        }
    }
    return edges;
}

static Qt::CursorShape cursorFor(Qt::Edges edges)
{
    if (edges == (Qt::TopEdge | Qt::LeftEdge) || edges == (Qt::BottomEdge | Qt::RightEdge)) {
        return Qt::SizeFDiagCursor;
    }
    if (edges == (Qt::TopEdge | Qt::RightEdge) || edges == (Qt::BottomEdge | Qt::LeftEdge)) {
        return Qt::SizeBDiagCursor;
    }
    if (edges & (Qt::LeftEdge | Qt::RightEdge)) {
        return Qt::SizeHorCursor;
    }
    return Qt::SizeVerCursor;
}

bool FinderChrome::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_window) {
        switch (event->type()) {
        case QEvent::Paint:
            // The window paints only our background; QMainWindow's own paint
            // would lay the style's window color over the glass.
            paintBackground();
            return true;
        case QEvent::ChildPolished:
            // KXMLGUI rebuilds toolbars (e.g. after loading saved settings);
            // move the stoplights to the new one.
            if (auto *toolBar = qobject_cast<QToolBar *>(static_cast<QChildEvent *>(event)->child())) {
                if (toolBar->objectName() == QLatin1String("mainToolBar") && toolBar != m_toolBar) {
                    QTimer::singleShot(0, this, [this, toolBar = QPointer<QToolBar>(toolBar)]() {
                        if (toolBar) {
                            setToolBar(toolBar);
                        }
                    });
                }
            }
            break;
        case QEvent::Show:
            if (QWindow *handle = m_window->windowHandle()) {
                handle->removeEventFilter(this);
                handle->installEventFilter(this);
            }
            updateWindowEffects();
            updateShadow();
            break;
        case QEvent::Resize:
            updateWindowEffects();
            break;
        case QEvent::WindowStateChange:
            updateWindowEffects();
            updateShadow();
            m_window->update();
            break;
        case QEvent::ActivationChange:
            if (m_stoplights) {
                m_stoplights->update();
            }
            updateTitleColor();
            break;
        default:
            break;
        }
        return false;
    }

    if (watched == m_sidebar) {
        if (event->type() == QEvent::Resize || event->type() == QEvent::Move || event->type() == QEvent::Show || event->type() == QEvent::Hide) {
            updateWindowEffects();
            updateLead();
            m_window->update();
            if (m_toolBar) {
                m_toolBar->update();
            }
        }
        return false;
    }

    if (watched == m_title && event->type() == QEvent::MouseButtonPress) {
        // Click the title to type a path.
        if (m_navigator) {
            if (KUrlNavigator *navigator = m_navigator->findChild<KUrlNavigator *>()) {
                navigator->setUrlEditable(true);
                updateNavigatorVisibility();
                navigator->setFocus();
            }
        }
        return true;
    }

    if (event->type() == QEvent::FocusOut && m_navigator && m_navigator->isAncestorOf(qobject_cast<QWidget *>(watched))) {
        auto *reason = static_cast<QFocusEvent *>(event);
        if (reason->reason() != Qt::PopupFocusReason) { // completion popups keep editing
            for (KUrlNavigator *navigator : m_navigator->findChildren<KUrlNavigator *>()) {
                navigator->setUrlEditable(false);
            }
        }
    }

    if (watched == m_toolBar) {
        if (event->type() == QEvent::Paint) {
            // Paint this strip of the window background ourselves (a
            // transparent toolbar doesn't reliably show the window's paint).
            QPainter p(m_toolBar);
            p.setRenderHint(QPainter::Antialiasing);
            const QRectF window(m_window->rect());
            const QPoint offset = m_toolBar->mapTo(m_window, QPoint(0, 0));
            p.translate(-offset);
            p.setClipPath(roundedPath(window, radius()));
            p.setCompositionMode(QPainter::CompositionMode_Source);
            const QRectF strip(offset, QSizeF(m_toolBar->size()));
            p.fillRect(strip, ContentColor);
            // The glass sidebar runs all the way up behind the stoplights.
            if (m_sidebar && m_sidebar->isVisible()) {
                const int right = m_sidebar->mapTo(m_window, QPoint(m_sidebar->width(), 0)).x();
                p.fillRect(QRectF(0, strip.top(), right, strip.height()), SidebarColor);
                p.setCompositionMode(QPainter::CompositionMode_SourceOver);
                p.fillRect(QRectF(right, strip.top(), 1, strip.height()), HairlineDark);
            }
            p.setCompositionMode(QPainter::CompositionMode_SourceOver);
            p.resetTransform();
            paintCapsules(p);
            return false; // the toolbar still draws its buttons
        }
        // Empty toolbar space works like a title bar.
        if (event->type() == QEvent::MouseButtonPress) {
            auto *e = static_cast<QMouseEvent *>(event);
            if (e->button() == Qt::LeftButton && !edgesAt(m_window->mapFromGlobal(e->globalPosition()))) {
                if (QWindow *handle = m_window->windowHandle()) {
                    handle->startSystemMove();
                    return true;
                }
            }
        } else if (event->type() == QEvent::MouseButtonDblClick) {
            isMaximized() ? m_window->showNormal() : m_window->showMaximized();
            return true;
        }
        return false;
    }

    if (watched == m_window->windowHandle()) {
        // Resizing from the window edges (there is no window frame to grab).
        switch (event->type()) {
        case QEvent::Expose:
        case QEvent::PlatformSurface:
            // The Wayland surface exists only once the window is exposed;
            // effects requested earlier may not have stuck.
            QTimer::singleShot(0, this, [this]() {
                updateWindowEffects();
                updateShadow();
            });
            break;
        case QEvent::MouseMove: {
            auto *e = static_cast<QMouseEvent *>(event);
            if (e->buttons() != Qt::NoButton) {
                break;
            }
            const Qt::Edges edges = edgesAt(e->position());
            if (edges != m_cursorEdges) {
                if (m_cursorEdges) {
                    QGuiApplication::restoreOverrideCursor();
                }
                if (edges) {
                    QGuiApplication::setOverrideCursor(cursorFor(edges));
                }
                m_cursorEdges = edges;
            }
            break;
        }
        case QEvent::Leave:
            if (m_cursorEdges) {
                QGuiApplication::restoreOverrideCursor();
                m_cursorEdges = {};
            }
            break;
        case QEvent::MouseButtonPress: {
            auto *e = static_cast<QMouseEvent *>(event);
            const Qt::Edges edges = edgesAt(e->position());
            if (e->button() == Qt::LeftButton && edges) {
                m_window->windowHandle()->startSystemResize(edges);
                return true;
            }
            break;
        }
        default:
            break;
        }
    }
    return false;
}

void FinderChrome::paintBackground()
{
    QPainter p(m_window);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(m_window->rect());
    const QPainterPath window = roundedPath(r, radius());

    p.setClipPath(window);
    p.fillRect(r, ContentColor);

    // Glass sidebar: a translucent tint KWin blurs behind; the vertical
    // hairline separates it from the content.
    if (m_sidebar && m_sidebar->isVisible()) {
        const QRect s(m_sidebar->mapTo(m_window, QPoint(0, 0)), m_sidebar->size());
        const QRectF strip(0, 0, s.right() + 1, r.height());
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(strip, SidebarColor);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.fillRect(QRectF(strip.right(), strip.top(), 1, strip.height()), HairlineDark);
    }


    // A hairline above Finder's footer (Dolphin's status bar).
    for (QWidget *child : m_window->findChildren<QWidget *>()) {
        if (child->isVisible() && qstrcmp(child->metaObject()->className(), "DolphinStatusBar") == 0) {
            const QPoint top = child->mapTo(m_window, QPoint(0, 0));
            p.fillRect(QRectF(top.x(), top.y(), child->width(), 1), QColor(0, 0, 0, 120));
        }
    }

    // Light inner edge around the whole window, like macOS's window border.
    p.setClipping(false);
    if (radius() > 0) {
        p.setPen(QPen(HairlineLight, 1));
        p.setBrush(Qt::NoBrush);
        p.drawPath(roundedPath(r.adjusted(0.5, 0.5, -0.5, -0.5), radius() - 0.5));
    }
}

void FinderChrome::updateWindowEffects()
{
    QWindow *handle = m_window->windowHandle();
    if (!handle) {
        return;
    }
    QRegion blur;
    if (m_sidebar && m_sidebar->isVisible()) {
        const QRect s(m_sidebar->mapTo(m_window, QPoint(0, 0)), m_sidebar->size());
        const QRect strip(0, 0, s.right() + 1, m_window->height());
        // Keep the blur inside the rounded corner.
        const QPolygon shape = roundedPath(QRectF(m_window->rect()), radius()).toFillPolygon().toPolygon();
        blur = QRegion(strip).intersected(QRegion(shape));
    }
    KWindowEffects::enableBlurBehind(handle, !blur.isEmpty(), blur);
}

void FinderChrome::updateShadow()
{
    QWindow *handle = m_window->windowHandle();
    if (!handle) {
        return;
    }
    if (isMaximized()) {
        if (m_shadow) {
            m_shadow->destroy();
        }
        return;
    }
    if (!m_shadow) {
        // Shadow tiles: a blurred rounded rectangle with the window's own shape
        // cut out, sliced into corners and 1 px edge strips.
        const int r = WindowRadius;
        const int pad = ShadowSize;
        const int size = 2 * pad + 2 * r + 1;
        QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        {
            QPainter p(&image);
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 120));
            p.drawPath(roundedPath(QRectF(pad, pad + 6, size - 2 * pad, size - 2 * pad), r));
        }
        blurAlpha(image, pad / 3);
        {
            QPainter p(&image);
            p.setRenderHint(QPainter::Antialiasing);
            p.setCompositionMode(QPainter::CompositionMode_Clear);
            p.setPen(Qt::NoPen);
            p.setBrush(Qt::black);
            p.drawPath(roundedPath(QRectF(pad, pad, size - 2 * pad, size - 2 * pad), r));
        }
        auto tile = [&](const QRect &rect) {
            auto t = KWindowShadowTile::Ptr::create();
            t->setImage(image.copy(rect));
            return t;
        };
        const int c = pad + r; // corner tile size
        m_shadow = new KWindowShadow(this);
        m_shadow->setTopLeftTile(tile(QRect(0, 0, c, c)));
        m_shadow->setTopTile(tile(QRect(c, 0, 1, pad)));
        m_shadow->setTopRightTile(tile(QRect(c + 1, 0, c, c)));
        m_shadow->setRightTile(tile(QRect(c + 1 + r, c, pad, 1)));
        m_shadow->setBottomRightTile(tile(QRect(c + 1, c + 1, c, c)));
        m_shadow->setBottomTile(tile(QRect(c, c + 1 + r, 1, pad)));
        m_shadow->setBottomLeftTile(tile(QRect(0, c + 1, c, c)));
        m_shadow->setLeftTile(tile(QRect(0, c, pad, 1)));
        m_shadow->setPadding(QMargins(pad, pad, pad, pad));
    }
    m_shadow->setWindow(handle);
    if (!m_shadow->isCreated()) {
        m_shadow->create();
    }
}
