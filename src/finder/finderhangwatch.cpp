/*
 * SPDX-FileCopyrightText: 2026 Brhett
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "finderhangwatch.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <mutex>
#include <thread>

#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

namespace
{
using Clock = std::chrono::steady_clock;

constexpr int BeatMs = 100; // how often the GUI thread checks in
constexpr int StallMs = 200; // a stall worth a line in the log
constexpr int HangMs = 500; // a hang: capture the stack
constexpr qint64 MaxLogBytes = 4 * 1024 * 1024;

std::atomic<qint64> lastBeat{0}; // ms since the epoch of Clock
std::mutex contextMutex;
QString context; // guarded by contextMutex
QString path;

qint64 nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

void append(const QByteArray &text)
{
    QFile file(path);
    if (file.size() > MaxLogBytes) {
        QFile::remove(path + QStringLiteral(".1"));
        QFile::rename(path, path + QStringLiteral(".1"));
    }
    if (file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        file.write(text);
    }
}

QByteArray stamp()
{
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")).toUtf8();
}

QByteArray readProc(const QByteArray &name)
{
    QFile file(QStringLiteral("/proc/self/task/%1/").arg(getpid()) + QString::fromLatin1(name));
    return file.open(QIODevice::ReadOnly) ? file.readAll().trimmed() : QByteArray();
}

// What the GUI thread is blocked in, from /proc. This works even when
// eu-stack can't attach: a thread waiting on a FUSE mount (Google Drive)
// can't be stopped until the mount answers.
QByteArray guiSyscall()
{
    const QList<QByteArray> fields = readProc("syscall").split(' ');
    QByteArray out = "    blocked in " + readProc("wchan");
    if (fields.size() < 4 || fields.first() == "running") {
        return out + '\n';
    }
    const long nr = fields[0].toLong();
    out += ", syscall " + fields[0];
    // Syscalls that take a path: which argument holds it (x86-64).
    int arg = 0;
    switch (nr) {
#ifdef __x86_64__
    case SYS_open:
    case SYS_stat:
    case SYS_lstat:
    case SYS_access:
    case SYS_readlink:
    case SYS_getxattr:
    case SYS_lgetxattr:
    case SYS_listxattr:
    case SYS_statfs:
        arg = 1;
        break;
#endif
    case SYS_openat:
    case SYS_newfstatat:
    case SYS_statx:
    case SYS_faccessat:
    case SYS_faccessat2:
    case SYS_readlinkat:
        arg = 2;
        break;
    default:
        break;
    }
    if (arg) {
        // The path is in our own memory; process_vm_readv fails cleanly
        // instead of crashing if the pointer is bad.
        char path[4096] = {};
        iovec local{path, sizeof(path) - 1};
        iovec remote{reinterpret_cast<void *>(fields[arg].toULongLong(nullptr, 16)), sizeof(path) - 1};
        if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) > 0) {
            out += " on \"" + QByteArray(path) + '"';
        }
    } else if (nr == SYS_read || nr == SYS_write || nr == SYS_fstat || nr == SYS_ioctl || nr == SYS_getdents64) {
        const QByteArray fd = QByteArray::number(fields[1].toLongLong(nullptr, 16));
        out += " on fd " + fd + " -> " + QFile::encodeName(QFile::symLinkTarget(QStringLiteral("/proc/self/fd/") + QString::fromLatin1(fd)));
    }
    return out + '\n';
}

// The GUI thread's stack (its thread id is the process id), from eu-stack.
// Every thread is stopped while eu-stack unwinds, for a fraction of a second.
QByteArray guiStack()
{
    const pid_t pid = getpid();
    // Into a file, not a pipe: eu-stack stops this thread too while it
    // unwinds, so a full pipe would never be drained. No debuginfod:
    // downloading symbols would freeze Finder for minutes.
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR", QDir::tempPath());
    const QByteArray outPath = QFile::encodeName(runtime + QStringLiteral("/finder-hang-%1.txt").arg(pid));
    // And a time limit: eu-stack waits forever for a thread it can't stop.
    const QByteArray command = "DEBUGINFOD_URLS= timeout -s KILL 5 eu-stack -m -p " + QByteArray::number(pid) + " > '" + outPath + "' 2>&1";
    const int status = std::system(command.constData());
    QFile file(QFile::decodeName(outPath));
    const QByteArray out = file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    file.remove();
    if (out.isEmpty()) {
        if (WIFEXITED(status) && WEXITSTATUS(status) == 128 + SIGKILL) {
            // eu-stack died with its SIGSTOP still pending; once the thread
            // came back, Finder would stop for good. SIGCONT discards it.
            kill(pid, SIGCONT);
            return "    (eu-stack couldn't stop the thread within 5 s)\n";
        }
        return status == 0 ? "    (eu-stack printed nothing)\n" : "    (eu-stack failed; is elfutils installed? sudo dnf install elfutils)\n";
    }

    // Keep just the GUI thread: from "TID <pid>:" up to the next "TID".
    const QByteArray header = "TID " + QByteArray::number(pid) + ":";
    QByteArray stack;
    bool inGui = false;
    for (const QByteArray &line : out.split('\n')) {
        if (line.startsWith("TID ")) {
            inGui = line.startsWith(header);
            continue;
        }
        if (inGui && !line.isEmpty()) {
            stack += "    " + line + '\n';
        }
    }
    return stack.isEmpty() ? "    " + out.trimmed().replace('\n', "\n    ") + '\n' : stack;
}

void watch()
{
    bool hung = false;
    qint64 hangStart = 0;
    qint64 nextSample = 0;
    int samples = 0;
    QByteArray stallContext;

    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(BeatMs / 2));
        const qint64 beat = lastBeat.load();
        const qint64 now = nowMs();
        const qint64 stalled = now - beat - BeatMs; // past the beat that was due

        if (!hung && stalled >= StallMs) {
            hung = true;
            hangStart = beat + BeatMs;
            nextSample = hangStart + HangMs;
            samples = 0;
            std::lock_guard lock(contextMutex);
            stallContext = context.toUtf8();
        }
        if (!hung) {
            continue;
        }

        if (beat + BeatMs > hangStart) {
            // The GUI thread is back.
            const qint64 length = beat - hangStart + BeatMs;
            if (samples == 0) {
                append(stamp() + "  stall of ~" + QByteArray::number(length) + " ms  [" + stallContext + "]\n");
            } else {
                append(stamp() + "  recovered after ~" + QByteArray::number(length) + " ms\n\n");
                qWarning().noquote() << "Finder: the window hung for" << length << "ms; stack in" << path;
            }
            hung = false;
            continue;
        }

        if (now >= nextSample && samples < 8) {
            if (samples == 0) {
                append(stamp() + "  HANG: the window stopped responding  [" + stallContext + "]\n");
            }
            ++samples;
            // Before eu-stack, which can take seconds and stops the thread.
            append("  after " + QByteArray::number(now - hangStart) + " ms:\n" + guiSyscall());
            // A thread waiting on a FUSE mount can't be stopped; don't try.
            if (readProc("wchan") != "request_wait_answer") {
                append(guiStack());
            }
            // Again at 2, 5, 10, 20… seconds: what changes is what it's waiting on.
            static const int gaps[] = {1500, 3000, 5000, 10000, 10000, 30000, 60000};
            nextSample = now + gaps[qMin(samples - 1, 6)];
        }
    }
}
}

QString FinderHangWatch::logPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + QStringLiteral("/finder/hangs.log");
}

void FinderHangWatch::note(const QString &text)
{
    std::lock_guard lock(contextMutex);
    context = text;
}

void FinderHangWatch::start()
{
    static bool started = false;
    if (started || qEnvironmentVariable("FINDER_HANGWATCH") == QLatin1String("0")) {
        return;
    }
    started = true;
    path = logPath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    // eu-stack is our child, not our parent: allow it to attach even where
    // Yama only lets parents trace (ptrace_scope 1).
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);

    lastBeat = nowMs();
    auto *heartbeat = new QTimer(QCoreApplication::instance());
    heartbeat->setTimerType(Qt::PreciseTimer);
    heartbeat->setInterval(BeatMs);
    QObject::connect(heartbeat, &QTimer::timeout, []() {
        lastBeat = nowMs();
    });
    heartbeat->start();

    append(stamp() + "  " + QFileInfo(QCoreApplication::applicationFilePath()).fileName().toUtf8() + " started (pid " + QByteArray::number(getpid()) + ")\n");
    std::thread(watch).detach();
}
