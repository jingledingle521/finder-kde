/*
 * SPDX-FileCopyrightText: 2026 Brhett
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "finderchrome.h"
#include "finderairdrop.h"
#include "findercolumnview.h"
#include "finderairdropservice.h"
#include "findersidebar.h"
#include "findercloud.h"
#include "findertransfers.h"

#include "dolphinmainwindow.h"
#include "dolphinviewcontainer.h"
#include "dolphintabwidget.h"
#include "statusbar/dolphinstatusbar.h"
#include "views/dolphinview.h"

#include <KConfigGroup>
#include <KFileItem>
#include <KSharedConfig>

#include <KUrlComboBox>
#include <KUrlNavigator>
#include <QFocusEvent>
#include <QLineEdit>
#include <KWindowEffects>
#include <KWindowShadow>
#include <KActionCollection>
#include <KLocalizedString>
#include <KXmlGuiWindow>
#include <QMenu>

#include <QApplication>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QtMath>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPropertyAnimation>
#include <QVBoxLayout>
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
 * The sidebar slid out over the content while it's hidden: a floating dark
 * glass panel with rounded corners and a soft shadow.
 */
class SidebarPeek : public QWidget
{
public:
    static constexpr int Shadow = 12; // room around the panel for its shadow
    static constexpr qreal Radius = 12;

    explicit SidebarPeek(QWidget *parent)
        : QWidget(parent)
    {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(Shadow, Shadow + 8, Shadow, Shadow + 6);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF panel = QRectF(rect()).adjusted(Shadow, Shadow, -Shadow, -Shadow);
        for (int i = Shadow; i > 0; i -= 2) {
            QPainterPath shadow;
            shadow.addRoundedRect(panel.adjusted(-i, -i + 3, i, i + 3), Radius + i, Radius + i);
            p.fillPath(shadow, QColor(0, 0, 0, 7));
        }
        QPainterPath path;
        path.addRoundedRect(panel, Radius, Radius);
        p.fillPath(path, QColor(40, 40, 44, 246));
        p.setPen(QPen(QColor(255, 255, 255, 30), 1));
        p.drawPath(path);
    }
};

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
        setFixedSize(qCeil(LeftMargin + 3 * Diameter + 2 * Gap + RightMargin), qCeil(Diameter + 8));
        setToolTip(QString());
    }

protected:
    // Sized like the window decorations' (MacSequoia draws ~22 px
    // circles 35 px apart at 1.5×), so all windows' stoplights match.
    static constexpr qreal Diameter = 14.5;
    static constexpr qreal Gap = 8.8;
    static constexpr int LeftMargin = 8;
    static constexpr int RightMargin = 14;

    QRectF circle(int i) const
    {
        const qreal y = (height() - Diameter) / 2.0 - 3; // above centre, as the user preferred
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
            const qreal s = Diameter / 12; // glyphs were drawn for 12 px circles
            const qreal k = 2.6 * s;
            if (i == 0) {
                p.drawLine(m + QPointF(-k, -k), m + QPointF(k, k));
                p.drawLine(m + QPointF(-k, k), m + QPointF(k, -k));
            } else if (i == 1) {
                p.drawLine(m + QPointF(-k - 0.4, 0), m + QPointF(k + 0.4, 0));
            } else {
                // Zoom: two small triangles pointing apart, as on macOS.
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(0, 0, 0, 150));
                QPolygonF a{m + QPointF(-3.2, -3.2) * s, m + QPointF(1.2, -3.2) * s, m + QPointF(-3.2, 1.2) * s};
                QPolygonF b{m + QPointF(3.2, 3.2) * s, m + QPointF(-1.2, 3.2) * s, m + QPointF(3.2, -1.2) * s};
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
                                         "QToolButton[finderOff=\"true\"]:checked { background: transparent; }"
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
        if (name == QLatin1String("icons") || name == QLatin1String("group_by") || name == QLatin1String("toggle_search")) {
            toolBar->insertWidget(action, gap(CapsuleGap + 2 * CapsulePad));
        }
        if (name == QLatin1String("group_by")) {
            // Finder's grouping control opens its menu on click.
            if (auto *button = qobject_cast<QToolButton *>(toolBar->widgetForAction(action))) {
                button->setPopupMode(QToolButton::InstantPopup);
                button->setToolButtonStyle(Qt::ToolButtonIconOnly);
                button->setIcon(groupIcon());
                button->setMinimumWidth(52);
            }
        }
        if (name == QLatin1String("toggle_search")) {
            // Finder's "…" action menu, between grouping and search.
            QAction *more = toolBar->insertWidget(action, createMoreButton(toolBar));
            more->setObjectName(QStringLiteral("finder_more"));
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

    // The rest (column view, AirDrop, transfer cards, tabs) belongs to the
    // Finder window itself, not to other windows using this chrome (the file
    // chooser).
    if (!qobject_cast<DolphinMainWindow *>(m_window)) {
        return;
    }

    // Finder's column view: a fourth view button after the list view.
    if (!m_columnsAction) {
        m_columnsAction = new QAction(columnsIcon(), i18nc("@action:intoolbar Finder view mode", "Columns"), this);
        m_columnsAction->setObjectName(QStringLiteral("finder_columns"));
        m_columnsAction->setCheckable(true);
        m_columnsAction->setToolTip(i18nc("@info:tooltip", "View as columns"));
        connect(m_columnsAction, &QAction::triggered, this, [this]() {
            setColumnMode(true);
        });
        // Go › AirDrop, with Finder's shortcut (⌘⇧R there, Ctrl+Shift+R here).
        if (auto *xmlWindow = qobject_cast<KXmlGuiWindow *>(m_window)) {
            QAction *airDrop = xmlWindow->actionCollection()->addAction(QStringLiteral("finder_airdrop"));
            airDrop->setText(i18nc("@action Finder", "AirDrop"));
            airDrop->setIcon(FinderAirDropView::icon(QColor(255, 255, 255, 225)));
            KActionCollection::setDefaultShortcut(airDrop, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R));
            connect(airDrop, &QAction::triggered, this, [this]() {
                showAirDrop({});
            });
        }
        // Switching tabs: the sidebar, title and overlays follow the new tab.
        if (auto *tabs = m_window->findChild<DolphinTabWidget *>()) {
            connect(tabs, &DolphinTabWidget::activeViewChanged, this, [this](DolphinViewContainer *container) {
                hideAirDrop();
                if (m_finderSidebar && container) {
                    m_finderSidebar->setUrl(container->url());
                }
                updateTitle();
                placeTransfers();
                if (m_columnMode) {
                    QTimer::singleShot(0, this, &FinderChrome::attachColumns);
                }
            });
        }
        connect(qobject_cast<DolphinMainWindow *>(m_window), &DolphinMainWindow::urlChanged, this, [this]() {
            hideAirDrop();
            if (m_columnMode) {
                QTimer::singleShot(0, this, &FinderChrome::attachColumns);
            }
        });
        m_columnMode = KSharedConfig::openConfig()->group(QStringLiteral("Finder")).readEntry("ColumnView", false);
    }
    QAction *after = nullptr;
    for (int i = 0; i < actions.size(); ++i) {
        const QString name = actions.at(i)->objectName();
        if (name == QLatin1String("icons") || name == QLatin1String("compact") || name == QLatin1String("details")) {
            // Picking another view mode leaves the column view.
            connect(actions.at(i), &QAction::triggered, this, &FinderChrome::leaveColumnMode, Qt::UniqueConnection);
        }
    }
    // Directly after the list-view button (before the gap to the next capsule).
    const QList<QAction *> current = toolBar->actions();
    for (int i = 0; i < current.size(); ++i) {
        if (current.at(i)->objectName() == QLatin1String("details") && i + 1 < current.size()) {
            after = current.at(i + 1);
        }
    }
    toolBar->insertAction(after, m_columnsAction);
    if (auto *button = qobject_cast<QToolButton *>(toolBar->widgetForAction(m_columnsAction))) {
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    }
    QTimer::singleShot(0, this, [this]() {
        placeTransfers();
        setColumnMode(m_columnMode);
    });
}

QIcon FinderChrome::columnsIcon()
{
    // Finder's column-view glyph: a rounded box split into three panes.
    QIcon icon;
    for (const qreal dpr : {1.0, 1.5, 2.0}) {
        QPixmap pm(QSize(22, 22) * dpr);
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(22.0 / 16.0, 22.0 / 16.0);
        p.setPen(QPen(QColor(255, 255, 255, 225), 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(1.5, 3.0, 13, 10), 2, 2);
        p.drawLine(QPointF(5.8, 3.0), QPointF(5.8, 13.0));
        p.drawLine(QPointF(10.2, 3.0), QPointF(10.2, 13.0));
        p.end();
        icon.addPixmap(pm);
    }
    return icon;
}

void FinderChrome::showAirDrop(const QList<QUrl> &files)
{
    auto *window = qobject_cast<DolphinMainWindow *>(m_window);
    DolphinViewContainer *container = window ? window->activeViewContainer() : nullptr;
    if (!container) {
        return;
    }
    DolphinView *view = container->view();
    if (!m_airDrop) {
        m_airDrop = new FinderAirDropView(view);
    }
    if (m_airDrop->parentWidget() != view) {
        m_airDrop->setParent(view);
    }
    view->removeEventFilter(this);
    view->installEventFilter(this);
    m_airDrop->setGeometry(view->rect());
    m_airDrop->setPendingFiles(files);
    m_airDrop->show();
    m_airDrop->raise();
    if (m_title) {
        m_title->setText(i18nc("@title Finder window", "AirDrop"));
    }
    if (m_finderSidebar) {
        m_finderSidebar->setUrl(FinderSidebar::AirDropUrl());
    }
}

void FinderChrome::hideAirDrop()
{
    if (!m_airDrop || !m_airDrop->isVisible()) {
        return;
    }
    m_airDrop->hide();
    updateTitle();
    auto *window = qobject_cast<DolphinMainWindow *>(m_window);
    DolphinViewContainer *container = window ? window->activeViewContainer() : nullptr;
    if (container && m_finderSidebar) {
        m_finderSidebar->setUrl(container->url());
    }
}

void FinderChrome::placeTransfers()
{
    if (!qobject_cast<DolphinMainWindow *>(m_window)) {
        return; // only Finder's own window shows AirDrop transfers
    }
    if (!m_transfers) {
        // Creating it also starts FinderAirDrop watching for incoming transfers.
        m_transfers = new FinderTransfersView(m_window);
    }
    auto *window = qobject_cast<DolphinMainWindow *>(m_window);
    DolphinViewContainer *container = window ? window->activeViewContainer() : nullptr;
    if (!container) {
        return;
    }
    // Against the file view's bottom-right corner, clear of the footer.
    const QWidget *view = container->view();
    m_transfers->setAnchor(view->mapTo(m_window, QPoint(view->width(), view->height())) - QPoint(16, 16));
}

void FinderChrome::leaveColumnMode()
{
    setColumnMode(false);
}

void FinderChrome::setColumnMode(bool on)
{
    m_columnMode = on;
    KConfigGroup group = KSharedConfig::openConfig()->group(QStringLiteral("Finder"));
    if (group.readEntry("ColumnView", false) != on) {
        group.writeEntry("ColumnView", on);
        group.sync();
    }
    if (m_columnsAction) {
        m_columnsAction->setChecked(on);
    }
    // Dolphin's own view mode stays checked underneath; hide its highlight.
    if (m_toolBar) {
        for (QAction *action : m_toolBar->actions()) {
            const QString name = action->objectName();
            if (name == QLatin1String("icons") || name == QLatin1String("compact") || name == QLatin1String("details")) {
                if (QWidget *button = m_toolBar->widgetForAction(action)) {
                    button->setProperty("finderOff", on);
                    button->style()->unpolish(button);
                    button->style()->polish(button);
                    button->update();
                }
            }
        }
        m_toolBar->update();
    }
    if (on) {
        attachColumns();
    } else if (m_columns) {
        m_columns->hide();
        updateTitle();
        if (auto *window = qobject_cast<DolphinMainWindow *>(m_window)) {
            if (DolphinViewContainer *container = window->activeViewContainer()) {
                container->view()->setFocus();
                // Hand the footer back to Dolphin's view.
                if (auto *statusBar = container->findChild<DolphinStatusBar *>()) {
                    statusBar->setUrl(container->url());
                }
                container->view()->requestStatusBarText();
            }
        }
    }
}

void FinderChrome::updateColumnStatus()
{
    if (!m_columnMode || !m_columns || !m_columns->isVisible()) {
        return;
    }
    const QUrl folder = m_columns->currentFolder();
    // The title names the folder being worked in; the first column keeps the window's title.
    if (folder == m_columns->rootUrl()) {
        updateTitle();
    } else if (m_title) {
        m_title->setText(folder.fileName().isEmpty() ? folder.toDisplayString(QUrl::PreferLocalFile) : folder.fileName());
    }
    if (auto *window = qobject_cast<DolphinMainWindow *>(m_window)) {
        if (DolphinViewContainer *container = window->activeViewContainer()) {
            if (auto *statusBar = container->findChild<DolphinStatusBar *>()) {
                statusBar->setUrl(folder);
                statusBar->setDefaultText(m_columns->statusText());
            }
        }
    }
}

void FinderChrome::attachColumns()
{
    auto *window = qobject_cast<DolphinMainWindow *>(m_window);
    DolphinViewContainer *container = window ? window->activeViewContainer() : nullptr;
    if (!container || !m_columnMode) {
        return;
    }
    DolphinView *view = container->view();
    if (!m_columns) {
        m_columns = new FinderColumnView(view);
        connect(m_columns, &FinderColumnView::folderActivated, this, [this](const QUrl &url) {
            if (auto *window = qobject_cast<DolphinMainWindow *>(m_window)) {
                if (DolphinViewContainer *container = window->activeViewContainer()) {
                    container->setUrl(url);
                }
            }
        });
        connect(m_columns, &FinderColumnView::currentFolderChanged, this, &FinderChrome::updateColumnStatus);
        // Hidden files follow View › Show Hidden Files.
        if (QAction *hidden = window->actionCollection()->action(QStringLiteral("show_hidden_files"))) {
            m_columns->setShowHiddenFiles(hidden->isChecked());
            connect(hidden, &QAction::toggled, m_columns, &FinderColumnView::setShowHiddenFiles);
        }
    }
    if (m_columns->parentWidget() != view) {
        m_columns->setParent(view);
    }
    view->removeEventFilter(this);
    view->installEventFilter(this);
    m_columns->setGeometry(view->rect());
    m_columns->setRootUrl(container->url());
    m_columns->show();
    m_columns->raise();
    m_columns->setFocus();
}

QIcon FinderChrome::groupIcon()
{
    // Finder's grouping control: a small grid of items and a chevron.
    QIcon icon;
    for (const qreal dpr : {1.0, 1.5, 2.0}) {
        // Drawn on a 16 px grid, sized like the toolbar's 22 px icons.
        QPixmap pm(QSize(22, 22) * dpr);
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(22.0 / 16.0, 22.0 / 16.0);
        const QColor ink(255, 255, 255, 225);
        p.setPen(QPen(ink, 1.0));
        p.setBrush(Qt::NoBrush);
        for (int row = 0; row < 2; ++row) {
            for (int col = 0; col < 2; ++col) {
                p.drawRoundedRect(QRectF(0.8 + col * 5.6, 2.8 + row * 5.6, 4.0, 4.0), 1.1, 1.1);
            }
        }
        p.setPen(QPen(ink, 1.1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolyline(QPolygonF{QPointF(12.2, 7.0), QPointF(13.8, 8.6), QPointF(15.4, 7.0)});
        p.end();
        icon.addPixmap(pm);
    }
    return icon;
}

QToolButton *FinderChrome::createMoreButton(QToolBar *toolBar)
{
    auto *button = new QToolButton(toolBar);
    // Three horizontal dots, drawn so they don't depend on the icon theme.
    QIcon dots;
    for (const qreal dpr : {1.0, 1.5, 2.0}) {
        QPixmap pm(QSize(16, 16) * dpr);
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 225));
        for (const qreal x : {3.0, 8.0, 13.0}) {
            p.drawEllipse(QPointF(x, 8), 1.6, 1.6);
        }
        p.end();
        dots.addPixmap(pm);
    }
    button->setIcon(dots);
    button->setToolTip(i18nc("@info:tooltip Finder toolbar", "Show actions for the selected items"));
    button->setPopupMode(QToolButton::InstantPopup);
    auto *menu = new QMenu(button);
    button->setMenu(menu);
    connect(menu, &QMenu::aboutToShow, this, [this, menu]() {
        // Rebuilt each time so it follows the current selection.
        menu->clear();
        auto *window = qobject_cast<KXmlGuiWindow *>(m_window);
        if (!window) {
            return;
        }
        KActionCollection *ac = window->actionCollection();
        // Finder's Share › AirDrop: send the selected files to a nearby device.
        menu->addAction(FinderAirDropView::icon(QColor(255, 255, 255, 225)), i18nc("@action:inmenu Finder", "AirDrop…"), this, [this]() {
            QList<QUrl> files;
            if (auto *window = qobject_cast<DolphinMainWindow *>(m_window)) {
                if (DolphinViewContainer *container = window->activeViewContainer()) {
                    const KFileItemList selected = container->view()->selectedItems();
                    for (const KFileItem &item : selected) {
                        if (item.isLocalFile() && !item.isDir()) {
                            files.append(item.url());
                        }
                    }
                }
            }
            // With files selected, share them in place (Finder's share sheet);
            // otherwise open the AirDrop window.
            if (files.isEmpty()) {
                showAirDrop({});
            } else {
                FinderAirDrop::instance()->share(files, m_window);
            }
        });
        for (FinderCloud *drive : FinderCloud::all()) {
            if (m_finderSidebar && !drive->hasAccount()) {
                menu->addAction(QIcon::fromTheme(drive->iconName()), i18nc("@action:inmenu Finder, %1 is e.g. iCloud Drive", "Connect %1…", drive->name()), this,
                                [this, drive]() {
                                    if (m_finderSidebar) {
                                        m_finderSidebar->connectCloud(drive->provider());
                                    }
                                });
            }
        }
        menu->addSeparator();
        const QList<QStringList> sections = {
            {QStringLiteral("create_dir"), QStringLiteral("new_menu")},
            {QStringLiteral("properties"), QStringLiteral("renamefile"), QStringLiteral("duplicate"), QStringLiteral("movetotrash")},
            {QStringLiteral("edit_copy"), QStringLiteral("copy_location"), QStringLiteral("edit_paste")},
            {QStringLiteral("sort"), QStringLiteral("show_hidden_files"), QStringLiteral("open_terminal_here")},
        };
        for (const QStringList &section : sections) {
            if (!menu->isEmpty()) {
                menu->addSeparator();
            }
            for (const QString &name : section) {
                if (QAction *action = ac->action(name); action && action->isVisible()) {
                    menu->addAction(action);
                }
            }
        }
    });
    return button;
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
    // Use the sidebar's names for places Dolphin titles differently.
    if (auto *window = qobject_cast<DolphinMainWindow *>(m_window)) {
        if (DolphinViewContainer *container = window->activeViewContainer()) {
            if (container->url().scheme() == QLatin1String("recentlyused")) {
                text = i18nc("@title Finder window", "Recents");
            }
        }
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
        {QStringLiteral("icons"), QStringLiteral("compact"), QStringLiteral("details"), QStringLiteral("finder_columns")},
        {QStringLiteral("group_by")},
        {QStringLiteral("finder_more")},
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
    if (sidebar && !m_finderSidebar) {
        m_finderSidebar = sidebar->findChild<FinderSidebar *>();
        if (m_finderSidebar) {
            connect(m_finderSidebar, &FinderSidebar::airDropRequested, this, [this]() {
                showAirDrop({});
            });
            // Picking any place (even the folder already shown) closes AirDrop.
            connect(m_finderSidebar, &FinderSidebar::placeActivated, this, &FinderChrome::hideAirDrop);
            connect(m_finderSidebar, &FinderSidebar::tabRequested, this, &FinderChrome::hideAirDrop);
        }
    }
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
            placeTransfers();
            hidePeek(); // its size follows the window's
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
            updatePeekWatch();
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
            updatePeekWatch();
            m_window->update();
            if (m_toolBar) {
                m_toolBar->update();
            }
        }
        return false;
    }

    if (event->type() == QEvent::Resize && watched->isWidgetType()) {
        // Keep the overlays (column view, AirDrop) covering the view.
        auto *view = static_cast<QWidget *>(watched);
        if (m_columns && view == m_columns->parentWidget()) {
            m_columns->setGeometry(view->rect());
        }
        if (m_airDrop && view == m_airDrop->parentWidget()) {
            m_airDrop->setGeometry(view->rect());
        }
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
        case QEvent::PlatformSurface:
            // A new native surface has none of our effects yet.
            m_surfaceReady = false;
            m_appliedBlur = QRegion();
            m_blurApplied = false;
            break;
        case QEvent::Expose:
            // The Wayland surface exists only once the window is exposed, so
            // apply the effects then, but only once per surface: Expose fires
            // often, and re-sending them each time floods the compositor.
            if (!m_surfaceReady && m_window->windowHandle()->isExposed()) {
                m_surfaceReady = true;
                QTimer::singleShot(0, this, [this]() {
                    updateWindowEffects();
                    updateShadow();
                });
            }
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
    // The window repaints on every hover and scroll in the (transparent) file
    // view, so don't walk the whole widget tree each time; look again every
    // half second, which catches new tabs and split views.
    if (!m_statusBarsAge.isValid() || m_statusBarsAge.elapsed() > 500) {
        m_statusBars.clear();
        for (QWidget *child : m_window->findChildren<QWidget *>()) {
            if (qstrcmp(child->metaObject()->className(), "DolphinStatusBar") == 0) {
                m_statusBars.append(child);
            }
        }
        m_statusBarsAge.start();
    }
    for (const QPointer<QWidget> &child : std::as_const(m_statusBars)) {
        if (child && child->isVisible()) {
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
    if (m_blurApplied && blur == m_appliedBlur) {
        return; // unchanged; don't bother KWin
    }
    KWindowEffects::enableBlurBehind(handle, !blur.isEmpty(), blur);
    m_appliedBlur = blur;
    m_blurApplied = true;
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
    if (m_shadow->isCreated() && m_shadow->window() == handle) {
        return; // already attached
    }
    if (m_shadow->isCreated()) {
        m_shadow->destroy();
    }
    m_shadow->setWindow(handle);
    m_shadow->create();
}

void FinderChrome::updatePeekWatch()
{
    const bool watch = m_sidebar && !m_sidebar->isVisible() && m_finderSidebar && m_window->isActiveWindow();
    if (!watch) {
        if (m_peekTimer) {
            m_peekTimer->stop();
        }
        if (m_peek && m_peek->isVisible()) {
            // Sidebar shown for good, or the window left: no animation.
            m_peekSlide->stop();
            m_peek->hide();
        }
        return;
    }
    if (!m_peekTimer) {
        m_peekTimer = new QTimer(this);
        m_peekTimer->setInterval(50);
        connect(m_peekTimer, &QTimer::timeout, this, &FinderChrome::peekTick);
    }
    m_peekDwell = 0;
    m_peekTimer->start();
}

void FinderChrome::peekTick()
{
    // Polled rather than tracked with events: the pointer is over the views,
    // not the window, and the window's edges belong to resizing.
    const int interval = m_peekTimer->interval();
    const QPoint pos = m_window->mapFromGlobal(QCursor::pos());
    const bool inWindow = m_window->underMouse() && m_window->rect().contains(pos);
    const bool shown = m_peek && m_peek->isVisible() && !m_peekClosing;
    if (!shown) {
        const int top = m_toolBar ? m_toolBar->mapTo(m_window, QPoint(0, m_toolBar->height())).y() : 0;
        if (inWindow && pos.x() <= 14 && pos.y() > top && QApplication::mouseButtons() == Qt::NoButton) {
            // A short rest on the edge, so passing over it doesn't open it.
            m_peekDwell += interval;
            if (m_peekDwell >= 150) {
                m_peekDwell = 0;
                showPeek();
            }
        } else {
            m_peekDwell = 0;
        }
        return;
    }
    // Stay out while it's in use: under the pointer (with some slack to the
    // right), a menu or dialog of its own open, or a button held.
    const bool keep = QApplication::activePopupWidget() || QApplication::activeModalWidget() || QApplication::mouseButtons() != Qt::NoButton
        || (inWindow && m_peek->geometry().adjusted(0, 0, 24, 0).contains(pos));
    if (keep) {
        m_peekAway = 0;
    } else if ((m_peekAway += interval) >= 400) {
        hidePeek();
    }
}

void FinderChrome::showPeek()
{
    if (!m_finderSidebar) {
        return;
    }
    if (!m_peek) {
        m_peek = new SidebarPeek(m_window);
        m_peekSidebar = new FinderSidebar(m_finderSidebar->placesModel(), m_peek);
        m_peekSidebar->setAirDropShown(m_finderSidebar->isAirDropShown());
        m_peek->layout()->addWidget(m_peekSidebar);
        // Picks go through the docked sidebar, which the window listens to,
        // and put the slid-out one away.
        connect(m_peekSidebar, &FinderSidebar::placeActivated, m_finderSidebar, &FinderSidebar::placeActivated);
        connect(m_peekSidebar, &FinderSidebar::tabRequested, m_finderSidebar, &FinderSidebar::tabRequested);
        connect(m_peekSidebar, &FinderSidebar::airDropRequested, m_finderSidebar, &FinderSidebar::airDropRequested);
        for (auto signal : {&FinderSidebar::placeActivated, &FinderSidebar::tabRequested}) {
            connect(m_peekSidebar, signal, this, &FinderChrome::hidePeek);
        }
        connect(m_peekSidebar, &FinderSidebar::airDropRequested, this, &FinderChrome::hidePeek);
        m_peekSlide = new QPropertyAnimation(m_peek, "pos", this);
        m_peekSlide->setDuration(220);
        m_peekSlide->setEasingCurve(QEasingCurve::OutCubic);
        connect(m_peekSlide, &QPropertyAnimation::finished, this, [this]() {
            if (m_peekClosing && m_peek) {
                m_peek->hide();
            }
        });
    }
    m_peekSidebar->setUrl(m_finderSidebar->url());

    // Below the toolbar, as wide as the sidebar was, down to the bottom.
    constexpr int shadow = SidebarPeek::Shadow;
    const int top = m_toolBar ? m_toolBar->mapTo(m_window, QPoint(0, m_toolBar->height())).y() : 8;
    const int width = std::max(200, m_sidebar ? m_sidebar->width() : 0) + 2 * shadow;
    const QRect target(8 - shadow, top - shadow, width, m_window->height() - top - 8 + 2 * shadow);
    const QPoint offscreen(-width, target.y());

    m_peekClosing = false;
    m_peekAway = 0;
    m_peekSlide->stop();
    if (!m_peek->isVisible()) {
        m_peek->setGeometry(QRect(offscreen, target.size()));
        m_peek->show();
    } else {
        m_peek->resize(target.size());
    }
    m_peek->raise();
    m_peekSlide->setStartValue(m_peek->pos());
    m_peekSlide->setEndValue(target.topLeft());
    m_peekSlide->start();
}

void FinderChrome::hidePeek()
{
    if (!m_peek || !m_peek->isVisible() || m_peekClosing) {
        return;
    }
    m_peekClosing = true;
    m_peekSlide->stop();
    m_peekSlide->setStartValue(m_peek->pos());
    m_peekSlide->setEndValue(QPoint(-m_peek->width(), m_peek->y()));
    m_peekSlide->start();
}

