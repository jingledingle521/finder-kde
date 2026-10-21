/*
 * SPDX-FileCopyrightText: 2026 Brhett
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef FINDERACCENT_H
#define FINDERACCENT_H

#include <KConfigGroup>
#include <KSharedConfig>

#include <QColor>
#include <QGuiApplication>
#include <QPalette>

/**
 * The accent color picked in System Settings, which Finder uses where macOS
 * uses its accent color: selections, sidebar icons, focus rings.
 *
 * The palette only carries KDE's selection color, a darker shade derived from
 * the accent, so the accent itself is read from kdeglobals. It's read again
 * whenever the palette changes, which is what changing the accent does.
 */
inline QColor finderAccent()
{
    static QColor accent;
    static QRgb seenHighlight = 0;
    const QColor highlight = QGuiApplication::palette().color(QPalette::Highlight);
    if (!accent.isValid() || highlight.rgba() != seenHighlight) {
        seenHighlight = highlight.rgba();
        KSharedConfigPtr globals = KSharedConfig::openConfig(QStringLiteral("kdeglobals"));
        globals->reparseConfiguration();
        accent = KConfigGroup(globals, QStringLiteral("General")).readEntry("AccentColor", QColor());
        if (!accent.isValid()) {
            accent = QGuiApplication::palette().color(QPalette::Accent);
        }
    }
    return accent;
}

#endif
