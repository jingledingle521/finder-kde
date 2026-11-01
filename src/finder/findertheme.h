#ifndef FINDERTHEME_H
#define FINDERTHEME_H

#include <QColor>
#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QPalette>
#include <QString>
#include <QWidget>

#include <functional>
#include <utility>

/**
 * Finder's colors, which follow the color scheme in use, light or dark. Every
 * color drawn in the window comes from here, other than the accent and those
 * that mean the same in both, such as the stoplights and warnings.
 *
 * Most are ink: text, glyphs, hover and selection fills and hairlines are white
 * over a dark window and black over a light one, at the same strength in both,
 * as AppKit's label, separator and fill colors are on macOS. Only the surfaces
 * beneath them differ.
 */
namespace FinderTheme
{
inline bool isDark()
{
    return QGuiApplication::palette().color(QPalette::Window).lightnessF() < 0.5;
}

inline QColor ink(int alpha)
{
    return isDark() ? QColor(255, 255, 255, alpha) : QColor(0, 0, 0, alpha);
}

/** Ink, for a stylesheet or rich text. */
inline QString inkCss(int alpha)
{
    return QStringLiteral("rgba(%1, %1, %1, %2)").arg(isDark() ? 255 : 0).arg(alpha);
}

/** The window's content: Golden Gate's dark content, slightly cool, or white. */
inline QColor content(int alpha = 255)
{
    return isDark() ? QColor(32, 32, 37, alpha) : QColor(255, 255, 255, alpha);
}

/** The glass sidebar, translucent: KWin blurs behind it. */
inline QColor sidebar()
{
    return isDark() ? QColor(44, 44, 48, 200) : QColor(236, 236, 238, 200);
}

/** A panel floating over the content, such as the slid-out sidebar. */
inline QColor panel()
{
    return isDark() ? QColor(40, 40, 44, 246) : QColor(246, 246, 248, 246);
}

/** A card over the content, such as an AirDrop transfer's. */
inline QColor card()
{
    return isDark() ? QColor(50, 50, 56, 245) : QColor(250, 250, 252, 245);
}

/** Warning text: macOS's yellow, which can't be read on white, or a dark amber. */
inline QColor warning()
{
    return isDark() ? QColor(0xff, 0xd6, 0x0a) : QColor(0x9e, 0x6a, 0x00);
}

/** A line between surfaces: black, faint on light ones, as AppKit's separator. */
inline QColor separator(int darkAlpha)
{
    return QColor(0, 0, 0, isDark() ? darkAlpha : 25);
}

/** A selection without keyboard focus, as AppKit's unemphasized one. */
inline QColor inactiveSelection()
{
    return ink(isDark() ? 46 : 35);
}

/** A sidebar icon while the window is in the background. */
inline QColor inactiveIcon()
{
    return isDark() ? QColor(160, 160, 165) : QColor(130, 130, 135);
}

/** The stoplights while the window is in the background. */
inline QColor inactiveStoplight()
{
    return isDark() ? QColor(80, 80, 84) : QColor(218, 218, 220);
}

/**
 * Sets a widget's stylesheet, made by @p make, and makes it again whenever the
 * color scheme changes, which a stylesheet can't follow by itself. Qt tells
 * only the application of a change to its palette, not each widget.
 */
inline void setStyleSheet(QWidget *widget, std::function<QString()> make)
{
    class Follower : public QObject
    {
    public:
        Follower(QWidget *widget, std::function<QString()> make)
            : QObject(widget)
            , m_widget(widget)
            , m_make(std::move(make))
        {
            QCoreApplication::instance()->installEventFilter(this);
        }

    protected:
        bool eventFilter(QObject *watched, QEvent *event) override
        {
            if (event->type() == QEvent::ApplicationPaletteChange && watched == QCoreApplication::instance()) {
                m_widget->setStyleSheet(m_make());
            }
            return false;
        }

    private:
        QWidget *m_widget;
        std::function<QString()> m_make;
    };
    widget->setStyleSheet(make());
    new Follower(widget, std::move(make));
}
}

#endif
