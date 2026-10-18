/*
 * SPDX-FileCopyrightText: 2026 Brhett
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef FINDERTRANSFERS_H
#define FINDERTRANSFERS_H

#include "finderairdropservice.h"

#include <QHash>
#include <QWidget>

class QVBoxLayout;

/**
 * @brief AirDrop progress cards, floating in the bottom-right of the window.
 *
 * One card per transfer: what is moving and to or from whom, a progress bar
 * with bytes, and a button to cancel a send (or, once done, to show the
 * received file). Finished cards fade away after a few seconds.
 */
class FinderTransfersView : public QWidget
{
    Q_OBJECT

public:
    explicit FinderTransfersView(QWidget *parent);

    /** Bottom-right corner (in the parent's coordinates) the cards sit against. */
    void setAnchor(const QPoint &bottomRight);

private:
    void showTransfer(const FinderTransfer &transfer);
    void relayout();

    QVBoxLayout *m_layout;
    QHash<int, QWidget *> m_cards;
    QPoint m_anchor;
};

#endif
