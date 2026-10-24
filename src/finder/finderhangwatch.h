/*
 * SPDX-FileCopyrightText: 2026 Brhett
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef FINDERHANGWATCH_H
#define FINDERHANGWATCH_H

#include <QString>

/**
 * @brief Logs where Finder's GUI thread is stuck when the window stops responding.
 *
 * A watchdog thread expects a heartbeat from the GUI thread ten times a
 * second. Stalls of about 300 ms or more get a line in the log. Once a stall
 * passes half a second, the GUI thread's stack is captured with eu-stack
 * (elfutils), and again while the hang goes on, so a long hang shows what
 * it's waiting on.
 *
 * The log is $XDG_STATE_HOME/finder/hangs.log (~/.local/state/finder/hangs.log).
 * Set FINDER_HANGWATCH=0 to turn it off.
 */
namespace FinderHangWatch
{
/** Starts the watchdog. Call once, from the GUI thread, after QApplication exists. */
void start();

/** What Finder is showing (e.g. the folder), written with each hang. GUI thread only. */
void note(const QString &context);

/** The log file. */
QString logPath();
}

#endif
