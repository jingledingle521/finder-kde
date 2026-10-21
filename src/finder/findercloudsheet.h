/*
 * SPDX-FileCopyrightText: 2026 Brhett
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef FINDERCLOUDSHEET_H
#define FINDERCLOUDSHEET_H

#include "findercloud.h"

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;

/**
 * @brief The sheet that connects Google Drive (or signs in again).
 *
 * Explains what connecting does, then opens Google's sign-in page in the
 * browser and waits for it, then waits for the drive to mount. Accepted once
 * the drive is ready to open.
 */
class FinderCloudSheet : public QDialog
{
    Q_OBJECT

public:
    FinderCloudSheet(FinderCloud *cloud, QWidget *parent);
    ~FinderCloudSheet() override;

    /** The sheet that connects @p cloud: this one, or iCloud's. */
    static QDialog *create(FinderCloud *cloud, QWidget *parent);

private:
    void signIn();
    void showError(const QString &error);

    FinderCloud *m_cloud;
    QStackedWidget *m_pages;
    QPushButton *m_signInButton;
    QLabel *m_error;
    QLabel *m_settingUp;
    bool m_waitingForMount = false;
};

/**
 * @brief The sheet that connects iCloud Drive (or signs in again).
 *
 * Asks for the Apple ID and password, then for the code Apple shows on the
 * user's trusted devices (or texts to a trusted number), then waits for the
 * drive to mount. Accepted once ~/iCloud Drive is ready to open.
 */
class FinderICloudSheet : public QDialog
{
    Q_OBJECT

public:
    FinderICloudSheet(FinderCloud *cloud, QWidget *parent);
    ~FinderICloudSheet() override;

private:
    void signIn();
    void sendCode(const QString &code);
    void showStep(const FinderCloud::Step &step);
    void showError(const QString &error);
    void setBusy(bool busy);

    FinderCloud *m_cloud;
    QStackedWidget *m_pages;
    QLineEdit *m_appleId;
    QLineEdit *m_password;
    QPushButton *m_signInButton;
    QLabel *m_error;
    QLabel *m_codePrompt;
    QLineEdit *m_code;
    QPushButton *m_verifyButton;
    QPushButton *m_textButton;
    QLabel *m_codeError;
    QVBoxLayout *m_phones;
    QLabel *m_settingUp;
    bool m_signingIn = false; // between the first request and Done or Error
    bool m_waitingForMount = false;
};

#endif
