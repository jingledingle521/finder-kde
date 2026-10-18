/*
 * SPDX-FileCopyrightText: 2026 Brhett
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "finderfiledialog.h"
#include "finder/finderchrome.h"
#include "finder/findersidebar.h"
#include "views/dolphinview.h"

#include <KFilePlacesModel>
#include <KLocalizedString>
#include <KUrlNavigator>

#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeDatabase>
#include <QPushButton>
#include <QScreen>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWidgetAction>

namespace
{
QAction *toolbarAction(QToolBar *toolBar, const QString &name, const QString &icon, const QString &text)
{
    auto *action = new QAction(QIcon::fromTheme(icon), text, toolBar);
    action->setObjectName(name); // FinderChrome groups buttons into capsules by name
    toolBar->addAction(action);
    return action;
}
}

FinderFileDialog::FinderFileDialog(const Options &options)
    : QMainWindow(nullptr)
    , m_options(options)
    , m_places(new KFilePlacesModel(this))
{
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(960, 600);

    // Toolbar: back/forward, the folder title (over a hidden path field), icon/list views.
    auto *toolBar = new QToolBar(this);
    toolBar->setObjectName(QStringLiteral("finderChooserToolBar"));
    toolBar->setMovable(false);
    toolBar->setIconSize(QSize(22, 22));
    addToolBar(toolBar);
    m_back = toolbarAction(toolBar, QStringLiteral("go_back"), QStringLiteral("go-previous"), i18nc("@action", "Back"));
    m_forward = toolbarAction(toolBar, QStringLiteral("go_forward"), QStringLiteral("go-next"), i18nc("@action", "Forward"));
    m_navigator = new KUrlNavigator(m_places, m_options.folder, toolBar);
    auto *navigatorAction = new QWidgetAction(toolBar);
    navigatorAction->setObjectName(QStringLiteral("url_navigators"));
    navigatorAction->setDefaultWidget(m_navigator);
    toolBar->addAction(navigatorAction);
    m_icons = toolbarAction(toolBar, QStringLiteral("icons"), QStringLiteral("view-list-icons"), i18nc("@action", "Icons"));
    m_details = toolbarAction(toolBar, QStringLiteral("details"), QStringLiteral("view-list-tree"), i18nc("@action", "List"));
    m_icons->setCheckable(true);
    m_details->setCheckable(true);
    connect(m_back, &QAction::triggered, m_navigator, &KUrlNavigator::goBack);
    connect(m_forward, &QAction::triggered, m_navigator, &KUrlNavigator::goForward);

    // Sidebar, as in Finder (without AirDrop).
    m_sidebar = new FinderSidebar(m_places);
    m_sidebar->setAirDropShown(false);
    auto *dock = new QDockWidget(this);
    dock->setObjectName(QStringLiteral("chooserSidebar"));
    dock->setTitleBarWidget(new QWidget(dock));
    dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    dock->setWidget(m_sidebar);
    dock->setMinimumWidth(190);
    addDockWidget(Qt::LeftDockWidgetArea, dock);
    connect(m_sidebar, &FinderSidebar::placeActivated, this, &FinderFileDialog::setFolder);
    connect(m_sidebar, &FinderSidebar::tabRequested, this, &FinderFileDialog::setFolder);

    // The files, in Dolphin's view (Finder's list and icon styling).
    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_view = new DolphinView(m_options.folder, central);
    m_view->setActive(true);
    layout->addWidget(m_view, 1);

    // Bottom bar: Save As, file type, Cancel and Open/Save.
    auto *bar = new QWidget(central);
    bar->setObjectName(QStringLiteral("chooserBar"));
    auto *row = new QHBoxLayout(bar);
    row->setContentsMargins(16, 10, 16, 12);
    row->setSpacing(10);
    if (m_options.mode == Save) {
        auto *label = new QLabel(i18nc("@label Finder file chooser", "Save As:"), bar);
        row->addWidget(label);
        m_name = new QLineEdit(m_options.currentName, bar);
        m_name->setMinimumWidth(260);
        row->addWidget(m_name, 2);
        connect(m_name, &QLineEdit::textChanged, this, &FinderFileDialog::updateAcceptButton);
        connect(m_name, &QLineEdit::returnPressed, this, &FinderFileDialog::accept);
    }
    if (!m_options.filters.isEmpty() && !m_options.directory) {
        m_filter = new QComboBox(bar);
        for (const PortalFilter &filter : std::as_const(m_options.filters)) {
            m_filter->addItem(filter.name);
        }
        m_filter->setCurrentIndex(qBound(0, m_options.currentFilter, m_options.filters.size() - 1));
        connect(m_filter, &QComboBox::currentIndexChanged, this, &FinderFileDialog::applyFilter);
        row->addWidget(m_filter);
    }
    row->addStretch(1);
    auto *cancel = new QPushButton(i18nc("@action:button", "Cancel"), bar);
    cancel->setObjectName(QStringLiteral("chooserCancel"));
    connect(cancel, &QPushButton::clicked, this, &FinderFileDialog::cancel);
    row->addWidget(cancel);
    QString acceptText = m_options.acceptLabel;
    acceptText.remove(QLatin1Char('_')); // GTK mnemonics
    if (acceptText.isEmpty()) {
        acceptText = m_options.mode == Save ? i18nc("@action:button", "Save")
            : m_options.directory           ? i18nc("@action:button", "Choose")
                                            : i18nc("@action:button", "Open");
    }
    m_accept = new QPushButton(acceptText, bar);
    m_accept->setObjectName(QStringLiteral("chooserAccept"));
    m_accept->setDefault(true);
    connect(m_accept, &QPushButton::clicked, this, &FinderFileDialog::accept);
    row->addWidget(m_accept);
    bar->setStyleSheet(QStringLiteral(
        "#chooserBar { border-top: 1px solid rgba(255,255,255,24); }"
        "QLineEdit { background: rgba(255,255,255,14); border: 1px solid rgba(255,255,255,30); border-radius: 7px; padding: 4px 8px; }"
        "QLineEdit:focus { border: 1px solid #0a84ff; }"
        "QPushButton { border: none; border-radius: 7px; padding: 6px 18px; min-width: 64px; background: rgba(255,255,255,30); color: white; }"
        "QPushButton:hover { background: rgba(255,255,255,44); }"
        "QPushButton#chooserAccept { background: #0a84ff; }"
        "QPushButton#chooserAccept:hover { background: #3a9bff; }"
        "QPushButton#chooserAccept:disabled { background: rgba(10,132,255,90); color: rgba(255,255,255,120); }"));
    layout->addWidget(bar);
    setCentralWidget(central);

    // Finder's window chrome: frameless rounded window, stoplights, glass sidebar,
    // capsule toolbar, and the folder title in place of the path.
    auto *chrome = new FinderChrome(this);
    chrome->setToolBar(toolBar);
    chrome->setSidebar(dock);

    // Following the folder: navigator ↔ view ↔ sidebar ↔ title.
    connect(m_navigator, &KUrlNavigator::urlChanged, this, [this](const QUrl &url) {
        if (m_view->url() != url) {
            m_view->setUrl(url);
        }
        m_sidebar->setUrl(url);
        const QString name = url.fileName();
        setWindowTitle(name.isEmpty() ? url.toDisplayString(QUrl::PreferLocalFile) : name);
        m_back->setEnabled(m_navigator->historyIndex() + 1 < m_navigator->historySize());
        m_forward->setEnabled(m_navigator->historyIndex() > 0);
        updateAcceptButton();
    });
    connect(m_view, &DolphinView::itemActivated, this, [this](const KFileItem &item) {
        if (item.isDir()) {
            setFolder(item.url());
        } else if (!m_options.directory) {
            accept();
        }
    });
    connect(m_view, &DolphinView::itemsActivated, this, [this]() {
        if (!m_options.directory) {
            accept();
        }
    });
    connect(m_view, &DolphinView::selectionChanged, this, [this](const KFileItemList &selection) {
        // Saving: picking an existing file takes its name.
        if (m_name && selection.count() == 1 && !selection.first().isDir()) {
            m_name->setText(selection.first().name());
        }
        updateAcceptButton();
    });
    auto syncMode = [this]() {
        m_icons->setChecked(m_view->viewMode() == DolphinView::IconsView);
        m_details->setChecked(m_view->viewMode() == DolphinView::DetailsView);
    };
    connect(m_view, &DolphinView::modeChanged, this, syncMode);
    connect(m_icons, &QAction::triggered, this, [this, syncMode]() {
        m_view->setViewMode(DolphinView::IconsView);
        syncMode();
    });
    connect(m_details, &QAction::triggered, this, [this, syncMode]() {
        m_view->setViewMode(DolphinView::DetailsView);
        syncMode();
    });

    // Initial state.
    QString title = m_options.title;
    title.remove(QLatin1Char('_'));
    setFolder(m_options.folder);
    m_sidebar->setUrl(m_options.folder);
    setWindowTitle(m_options.folder.fileName().isEmpty() ? m_options.folder.toDisplayString(QUrl::PreferLocalFile) : m_options.folder.fileName());
    syncMode();
    applyFilter(m_filter ? m_filter->currentIndex() : -1);
    m_back->setEnabled(false);
    m_forward->setEnabled(false);
    updateAcceptButton();
    if (!title.isEmpty()) {
        setAccessibleName(title);
    }
}

const FinderFileDialog::Options &FinderFileDialog::options() const
{
    return m_options;
}

void FinderFileDialog::present()
{
    if (QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect area = screen->availableGeometry();
        move(area.center() - rect().center());
    }
    show();
    raise();
    activateWindow();
    if (m_name) {
        // Select the name without its extension, as Finder does.
        m_name->setFocus();
        const int dot = m_name->text().lastIndexOf(QLatin1Char('.'));
        m_name->setSelection(0, dot > 0 ? dot : m_name->text().size());
    } else {
        m_view->setFocus();
    }
}

void FinderFileDialog::setFolder(const QUrl &url)
{
    if (m_navigator->locationUrl() != url) {
        m_navigator->setLocationUrl(url);
    }
    if (m_view->url() != url) {
        m_view->setUrl(url);
    }
}

void FinderFileDialog::applyFilter(int index)
{
    if (m_options.directory) {
        m_view->setMimeTypeFilters({QStringLiteral("inode/directory")});
        return;
    }
    if (index < 0 || index >= m_options.filters.size()) {
        m_view->setMimeTypeFilters({});
        return;
    }
    // Filters come as globs and MIME types; Dolphin's view filters by MIME
    // type, so map globs ("*.png") to the types they stand for. Folders stay
    // visible so you can still browse.
    QMimeDatabase db;
    QStringList types{QStringLiteral("inode/directory")};
    for (const PortalFilterRule &rule : m_options.filters.at(index).rules) {
        if (rule.type == 1) {
            types.append(rule.pattern);
            continue;
        }
        if (rule.pattern == QLatin1String("*") || rule.pattern == QLatin1String("*.*")) {
            m_view->setMimeTypeFilters({}); // everything
            return;
        }
        const QList<QMimeType> matches = db.mimeTypesForFileName(QStringLiteral("file") + rule.pattern.mid(rule.pattern.indexOf(QLatin1Char('.'))));
        if (matches.isEmpty()) {
            m_view->setMimeTypeFilters({}); // a glob no type covers: don't hide anything
            return;
        }
        for (const QMimeType &type : matches) {
            types.append(type.name());
        }
    }
    m_view->setMimeTypeFilters(types);
}

QList<QUrl> FinderFileDialog::chosenFiles() const
{
    QList<QUrl> urls;
    const KFileItemList selected = m_view->selectedItems();
    for (const KFileItem &item : selected) {
        if (m_options.directory ? item.isDir() : !item.isDir()) {
            urls.append(item.url());
        }
    }
    if (!m_options.multiple && urls.size() > 1) {
        urls = {urls.first()};
    }
    return urls;
}

void FinderFileDialog::updateAcceptButton()
{
    if (m_options.mode == Save) {
        m_accept->setEnabled(m_name && !m_name->text().trimmed().isEmpty());
    } else if (m_options.directory) {
        m_accept->setEnabled(true); // the selected folder, or the one shown
    } else {
        // Open is also how you enter a selected folder.
        const KFileItemList selected = m_view->selectedItems();
        m_accept->setEnabled(!selected.isEmpty());
    }
}

void FinderFileDialog::accept()
{
    if (m_options.mode == Save) {
        const QString name = m_name->text().trimmed();
        if (name.isEmpty()) {
            return;
        }
        const QUrl target = QUrl::fromLocalFile(QDir(m_view->url().toLocalFile()).filePath(name));
        if (QFileInfo::exists(target.toLocalFile())) {
            const auto answer = QMessageBox::question(this,
                                                      i18nc("@title:window", "Replace File?"),
                                                      i18nc("@info", "“%1” already exists. Do you want to replace it?", name),
                                                      QMessageBox::Cancel | QMessageBox::Yes,
                                                      QMessageBox::Cancel);
            if (answer != QMessageBox::Yes) {
                return;
            }
        }
        finish(true, {target});
        return;
    }
    if (m_options.directory) {
        const QList<QUrl> folders = chosenFiles();
        finish(true, folders.isEmpty() ? QList<QUrl>{m_view->url()} : folders);
        return;
    }
    const QList<QUrl> files = chosenFiles();
    if (!files.isEmpty()) {
        finish(true, files);
        return;
    }
    // Only a folder selected: Open goes into it.
    const KFileItemList selected = m_view->selectedItems();
    if (selected.count() == 1 && selected.first().isDir()) {
        setFolder(selected.first().url());
    }
}

void FinderFileDialog::cancel()
{
    finish(false, {});
}

void FinderFileDialog::finish(bool accepted, const QList<QUrl> &urls)
{
    if (m_done) {
        return;
    }
    m_done = true;
    hide();
    Q_EMIT finished(accepted, urls, m_filter ? m_filter->currentIndex() : -1);
}

void FinderFileDialog::closeEvent(QCloseEvent *event)
{
    // The red stoplight cancels.
    event->ignore();
    cancel();
}

void FinderFileDialog::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        cancel();
        return;
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && m_accept->isEnabled()) {
        accept();
        return;
    }
    QMainWindow::keyPressEvent(event);
}

#include "moc_finderfiledialog.cpp"
