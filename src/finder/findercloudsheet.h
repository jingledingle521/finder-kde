/*
 * SPDX-FileCopyrightText: 2026 Brhett
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef FINDERCLOUDSHEET_H
#define FINDERCLOUDSHEET_H

#include <QDialog>

class QLabel;
class QPushButton;
class QStackedWidget;

/**
 * @brief The sheet that connects Google Drive (or signs in again).
 *
 * Explains what connecting does, then opens Google's sign-in page in the
 * browser and waits for it, then waits for the drive to mount. Accepted once
 * ~/Google Drive is ready to open.
 */
class FinderCloudSheet : public QDialog
{
    Q_OBJECT

public:
    explicit FinderCloudSheet(QWidget *parent);
    ~FinderCloudSheet() override;

private:
    void signIn();
    void showError(const QString &error);

    QStackedWidget *m_pages;
    QPushButton *m_signInButton;
    QLabel *m_error;
    QLabel *m_settingUp;
    bool m_waitingForMount = false;
};

#endif
