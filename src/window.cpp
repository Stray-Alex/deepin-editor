/* -*- Mode: C++; indent-tabs-mode: nil; tab-width: 4 -*-
 * -*- coding: utf-8 -*-
 *
 * Copyright (C) 2011 ~ 2018 Deepin, Inc.
 *
 * Author:     Wang Yong <wangyong@deepin.com>
 * Maintainer: Rekols    <rekols@foxmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "window.h"
#include "toolbar.h"
#include "danchors.h"
#include "dthememanager.h"
#include "dtoast.h"
#include "utils.h"

#include <DSettingsGroup>
#include <DSettings>
#include <DSettingsOption>
#include <DTitlebar>
#include <QApplication>
#include <QPrintDialog>
#include <QPrintPreviewDialog>
#include <QPrinter>
#include <QScreen>
#include <QStyleFactory>

#ifdef DTKWIDGET_CLASS_DFileDialog
#include <DFileDialog>
#else
#include <QFileDialog>
#endif

DWM_USE_NAMESPACE

Window::Window(DMainWindow *parent)
    : DMainWindow(parent),
      m_centralWidget(new QWidget),
      m_editorWidget(new QStackedWidget),
      m_centralLayout(new QVBoxLayout(m_centralWidget)),
      m_tabbar(new Tabbar),
      m_jumpLineBar(new JumpLineBar(this)),
      m_replaceBar(new ReplaceBar),
      m_themePanel(new ThemePanel(this)),
      m_findBar(new FindBar),
      m_settings(new Settings(this)),
      m_windowManager(new DWindowManager),
      m_menu(new QMenu),
      m_titlebarStyleSheet(titlebar()->styleSheet())
{
    m_blankFileDir = QDir(QStandardPaths::standardLocations(QStandardPaths::DataLocation).first()).filePath("blank-files");
    m_themePath = m_settings->settings->option("advance.editor.theme")->value().toString();
    m_rootSaveDBus = new DBusDaemon::dbus("com.deepin.editor.daemon", "/", QDBusConnection::systemBus(), this);
    m_windowShowFlag = true;

    // Init.
    installEventFilter(this);
    setAcceptDrops(true);

    // Apply qss theme.
    Utils::applyQss(this, "main.qss");
    loadTheme(m_themePath);

    // Init settings.
    connect(m_settings, &Settings::adjustFont, this, &Window::updateFont);
    connect(m_settings, &Settings::adjustFontSize, this, &Window::updateFontSize);
    connect(m_settings, &Settings::adjustTabSpaceNumber, this, &Window::updateTabSpaceNumber);

    // Init layout and editor.
    m_centralLayout->setMargin(0);
    m_centralLayout->setSpacing(0);

    m_centralLayout->addWidget(m_editorWidget);
    setWindowIcon(QIcon::fromTheme("deepin-editor"));
    setCentralWidget(m_centralWidget);

    // Init titlebar.
    if (titlebar()) {
        initTitlebar();
    }

    // Init window state with config.
    // Below code must before this->titlebar()->setMenu, otherwise main menu can't display pre-build-in menu items by dtk.
    const QString &windowState = m_settings->settings->option("advance.window.windowstate")->value().toString();

    // window minimum size.
    setMinimumSize(600, 400);

    // resize window size.
    QScreen *screen = QGuiApplication::primaryScreen();
    QRect screenGeometry = screen->geometry();

    resize(QSize(screenGeometry.width() * m_settings->settings->option("advance.window.window_width")->value().toDouble(),
                 screenGeometry.height() * m_settings->settings->option("advance.window.window_height")->value().toDouble()));
    show();

    // init window state.
    if (windowState == "window_maximum") {
        showMaximized();
    } else if (windowState == "fullscreen") {
        showFullScreen();
    }

    // Init find bar.
    connect(m_findBar, &FindBar::findNext, this, &Window::handleFindNext, Qt::QueuedConnection);
    connect(m_findBar, &FindBar::findPrev, this, &Window::handleFindPrev, Qt::QueuedConnection);
    connect(m_findBar, &FindBar::removeSearchKeyword, this, &Window::handleRemoveSearchKeyword, Qt::QueuedConnection);
    connect(m_findBar, &FindBar::updateSearchKeyword, this, [=] (QString file, QString keyword) {
        handleUpdateSearchKeyword(m_findBar, file, keyword);
    });

    // Init replace bar.
    connect(m_replaceBar, &ReplaceBar::removeSearchKeyword, this, &Window::handleRemoveSearchKeyword, Qt::QueuedConnection);
    connect(m_replaceBar, &ReplaceBar::replaceAll, this, &Window::handleReplaceAll, Qt::QueuedConnection);
    connect(m_replaceBar, &ReplaceBar::replaceNext, this, &Window::handleReplaceNext, Qt::QueuedConnection);
    connect(m_replaceBar, &ReplaceBar::replaceRest, this, &Window::handleReplaceRest, Qt::QueuedConnection);
    connect(m_replaceBar, &ReplaceBar::replaceSkip, this, &Window::handleReplaceSkip, Qt::QueuedConnection);
    connect(m_replaceBar, &ReplaceBar::updateSearchKeyword, this, [=] (QString file, QString keyword) {
        handleUpdateSearchKeyword(m_replaceBar, file, keyword);
    });

    // Init jump line bar.
    QTimer::singleShot(0, m_jumpLineBar, SLOT(hide()));

    connect(m_jumpLineBar, &JumpLineBar::jumpToLine, this, &Window::handleJumpLineBarJumpToLine, Qt::QueuedConnection);
    connect(m_jumpLineBar, &JumpLineBar::backToPosition, this, &Window::handleBackToPosition, Qt::QueuedConnection);
    connect(m_jumpLineBar, &JumpLineBar::lostFocusExit, this, &Window::handleJumpLineBarExit, Qt::QueuedConnection);

    // Make jump line bar pop at top-right of editor.
    DAnchorsBase::setAnchor(m_jumpLineBar, Qt::AnchorTop, m_centralWidget, Qt::AnchorTop);
    DAnchorsBase::setAnchor(m_jumpLineBar, Qt::AnchorRight, m_centralWidget, Qt::AnchorRight);

    // Init theme panel.
    DAnchorsBase::setAnchor(m_themePanel, Qt::AnchorTop, m_centralWidget, Qt::AnchorTop);
    DAnchorsBase::setAnchor(m_themePanel, Qt::AnchorBottom, m_centralWidget, Qt::AnchorBottom);
    DAnchorsBase::setAnchor(m_themePanel, Qt::AnchorRight, m_centralWidget, Qt::AnchorRight);

    // for the first time open the need be init.
    m_themePanel->setSelectionTheme(m_themePath);

    connect(m_themePanel, &ThemePanel::themeChanged, this, &Window::themeChanged);
    connect(this, &Window::requestDragEnterEvent, this, &Window::dragEnterEvent);
    connect(this, &Window::requestDropEvent, this, &Window::dropEvent);
}

Window::~Window()
{
    // We don't need clean pointers because application has exit here.
}

void Window::initTitlebar()
{
    QAction *newWindowAction(new QAction(tr("New window"), this));
    QAction *newTabAction(new QAction(tr("New tab"), this));
    QAction *openFileAction(new QAction(tr("Open file"), this));
    QAction *saveAction(new QAction(tr("Save"), this));
    QAction *saveAsAction(new QAction(tr("Save as"), this));
    QAction *printAction(new QAction(tr("Print"), this));
    QAction *switchThemeAction(new QAction(tr("Switch theme"), this));
    QAction *settingAction(new QAction(tr("Settings"), this));

    m_menu->addAction(newWindowAction);
    m_menu->addAction(newTabAction);
    m_menu->addAction(openFileAction);
    m_menu->addSeparator();
    m_menu->addAction(saveAction);
    m_menu->addAction(saveAsAction);
    m_menu->addAction(printAction);
    m_menu->addAction(switchThemeAction);
    m_menu->addSeparator();
    m_menu->addAction(settingAction);

    m_menu->setStyle(QStyleFactory::create("dlight"));
    m_menu->setMinimumWidth(150);

    ToolBar *toolBar = new ToolBar;
    toolBar->setTabbar(m_tabbar);

    titlebar()->setCustomWidget(toolBar, Qt::AlignVCenter, false);
    titlebar()->setAutoHideOnFullscreen(true);
    titlebar()->setSeparatorVisible(true);
    titlebar()->setFixedHeight(40);
    titlebar()->setMenu(m_menu);

    connect(m_tabbar, &DTabBar::tabBarDoubleClicked, titlebar(), &DTitlebar::doubleClicked, Qt::QueuedConnection);

    connect(m_tabbar, &Tabbar::closeTabs, this, &Window::handleTabsClosed, Qt::QueuedConnection);
    connect(m_tabbar, &Tabbar::requestHistorySaved, this, [=] (const QString &filePath) {
        if (QFileInfo(filePath).dir().absolutePath() == m_blankFileDir) {
            return;
        }

        if (!m_closeFileHistory.contains(filePath)) {
            m_closeFileHistory << filePath;
        }
    });

    connect(m_tabbar, &DTabBar::tabCloseRequested, this, &Window::handleTabCloseRequested, Qt::QueuedConnection);
    connect(m_tabbar, &DTabBar::tabAddRequested, this, static_cast<void (Window::*)()>(&Window::addBlankTab), Qt::QueuedConnection);
    connect(m_tabbar, &DTabBar::currentChanged, this, &Window::handleCurrentChanged, Qt::QueuedConnection);

    connect(newWindowAction, &QAction::triggered, this, &Window::newWindow);
    connect(newTabAction, &QAction::triggered, this, [=] () { addBlankTab(); });
    connect(openFileAction, &QAction::triggered, this, &Window::openFile);
    connect(saveAction, &QAction::triggered, this, &Window::saveFile);
    connect(saveAsAction, &QAction::triggered, this, &Window::saveAsFile);
    connect(printAction, &QAction::triggered, this, &Window::popupPrintDialog);
    connect(settingAction, &QAction::triggered, this, &Window::popupSettingsDialog);
    connect(switchThemeAction, &QAction::triggered, m_themePanel, &ThemePanel::popup);
    connect(m_themePanel, &ThemePanel::popupFinished, [=] { m_themePanel->setSelectionTheme(m_themePath); });
}

int Window::getTabIndex(const QString &file)
{
    return m_tabbar->indexOf(file);
}

void Window::activeTab(int index)
{
    DMainWindow::activateWindow();
    m_tabbar->setCurrentIndex(index);
}

void Window::addTab(const QString &filepath, bool activeTab)
{
    // check whether it is an editable file thround mimeType.
    if (Utils::isMimeTypeSupport(filepath)) {
        // check if have permission to read the file.
        QFile file(filepath);
        if (!file.open(QIODevice::ReadOnly)) {
            showNotify(QString(tr("You do not have permission to open %1")).arg(filepath));
            return;
        }

        if (m_tabbar->indexOf(filepath) == -1) {
            m_tabbar->addTab(filepath, QFileInfo(filepath).fileName());

            if (!m_wrappers.contains(filepath)) {
                EditWrapper *wrapper = createEditor();
                wrapper->openFile(filepath);

                m_wrappers[filepath] = wrapper;

                showNewEditor(wrapper);
            }
        }

        // Activate window.
        activateWindow();

        // Active tab if activeTab is true.
        if (activeTab) {
            int tabIndex = m_tabbar->indexOf(filepath);
            if (tabIndex != -1) {
                m_tabbar->setCurrentIndex(tabIndex);
            }
        }
    } else {
        showNotify(tr("Invalid file: %1").arg(QFileInfo(filepath).fileName()));
    }
}

void Window::addTabWithWrapper(EditWrapper *wrapper, const QString &filepath, const QString &tabName, int index)
{
    if (index == -1) {
        index = m_tabbar->currentIndex() + 1;
    }

    // wrapper may be from anther window pointer.
    // reconnect signal.
    connect(wrapper->textEditor(), &DTextEdit::clickFindAction, this, &Window::popupFindBar, Qt::QueuedConnection);
    connect(wrapper->textEditor(), &DTextEdit::clickReplaceAction, this, &Window::popupReplaceBar, Qt::QueuedConnection);
    connect(wrapper->textEditor(), &DTextEdit::clickJumpLineAction, this, &Window::popupJumpLineBar, Qt::QueuedConnection);
    connect(wrapper->textEditor(), &DTextEdit::clickFullscreenAction, this, &Window::toggleFullscreen, Qt::QueuedConnection);
    connect(wrapper->textEditor(), &DTextEdit::popupNotify, this, &Window::showNotify, Qt::QueuedConnection);
    connect(wrapper->textEditor(), &DTextEdit::pressEsc, this, &Window::removeBottomWidget, Qt::QueuedConnection);

    // add wrapper to this window.
    m_tabbar->addTabWithIndex(index, filepath, tabName);
    m_wrappers[filepath] = wrapper;
    wrapper->updatePath(filepath);

    showNewEditor(wrapper);
}

void Window::closeTab()
{
    const QString &filePath = m_tabbar->currentPath();
    const bool isBlankFile = QFileInfo(filePath).dir().absolutePath() == m_blankFileDir;
    EditWrapper *wrapper = m_wrappers.value(filePath);

    if (!wrapper)  {
        return;
    }

    // this property holds whether the document has been modified by the user
    bool isModified = wrapper->textEditor()->document()->isModified();

    // document has been modified or unsaved draft document.
    // need to prompt whether to save.
    if (isModified || (isBlankFile && !wrapper->textEditor()->toPlainText().isEmpty())) {
        DDialog *dialog = createSaveFileDialog(tr("Save File"), tr("Do you want to save this file?"));

        connect(dialog, &DDialog::buttonClicked, this, [=] (int index) {
            dialog->hide();

            // don't save.
            if (index == 1) {
                m_tabbar->closeCurrentTab();

                // delete the draft document.
                if (isBlankFile) {
                    QFile(filePath).remove();
                }

                removeWrapper(filePath, true);
            }
            else if (index == 2) {
                // may to press CANEL button in the save dialog.
                if (saveFile()) {
                    m_tabbar->closeCurrentTab();
                    removeWrapper(filePath, true);
                }
            }

            focusActiveEditor();
        });

        dialog->exec();
    } else {
        // record last close path.
        m_closeFileHistory << m_tabbar->currentPath();

        // close tab directly, because all file is save automatically.
        m_tabbar->closeCurrentTab();

        // remove blank file.
        if (isBlankFile) {
            QFile::remove(filePath);
        }

        removeWrapper(filePath, true);
        focusActiveEditor();
    }
}

void Window::restoreTab()
{
    if (m_closeFileHistory.size() > 0) {
        addTab(m_closeFileHistory.takeLast()) ;
    }
}

EditWrapper* Window::createEditor()
{
    EditWrapper *wrapper = new EditWrapper();
    wrapper->textEditor()->setThemeWithPath(m_themePath);
    wrapper->textEditor()->setSettings(m_settings);
    wrapper->textEditor()->setTabSpaceNumber(m_settings->settings->option("advance.editor.tabspacenumber")->value().toInt());
    wrapper->textEditor()->setFontFamily(m_settings->settings->option("base.font.family")->value().toString());
    wrapper->textEditor()->setModified(false);
    setFontSizeWithConfig(wrapper);

    connect(wrapper->textEditor(), &DTextEdit::clickFindAction, this, &Window::popupFindBar, Qt::QueuedConnection);
    connect(wrapper->textEditor(), &DTextEdit::clickReplaceAction, this, &Window::popupReplaceBar, Qt::QueuedConnection);
    connect(wrapper->textEditor(), &DTextEdit::clickJumpLineAction, this, &Window::popupJumpLineBar, Qt::QueuedConnection);
    connect(wrapper->textEditor(), &DTextEdit::clickFullscreenAction, this, &Window::toggleFullscreen, Qt::QueuedConnection);
    connect(wrapper->textEditor(), &DTextEdit::popupNotify, this, &Window::showNotify, Qt::QueuedConnection);
    connect(wrapper->textEditor(), &DTextEdit::pressEsc, this, &Window::removeBottomWidget, Qt::QueuedConnection);

    return wrapper;
}

EditWrapper* Window::currentWrapper()
{
    return m_wrappers.value(m_tabbar->currentPath());
}

EditWrapper* Window::wrapper(const QString &filePath)
{
    return m_wrappers.value(filePath);
}

DTextEdit* Window::getTextEditor(const QString &filepath)
{
    return m_wrappers.value(filepath)->textEditor();
}

void Window::focusActiveEditor()
{
    if (m_tabbar->count() > 0) {
        currentWrapper()->textEditor()->setFocus();
    }
}

void Window::removeWrapper(const QString &filePath, bool isDelete)
{
    if (m_wrappers.contains(filePath)) {
        EditWrapper *wrapper = m_wrappers.value(filePath);

        m_editorWidget->removeWidget(wrapper);
        m_wrappers.remove(filePath);

        if (isDelete) {
            wrapper->deleteLater();
        }

        // remove all signals on this connection.
        disconnect(wrapper->textEditor(), 0, this, 0);
    }

    // Exit window after close all tabs.
    if (m_wrappers.isEmpty()) {
        DMainWindow::close();
    }
}

void Window::openFile()
{
    QFileDialog dialog;
    dialog.setFileMode(QFileDialog::ExistingFiles);
    dialog.setAcceptMode(QFileDialog::AcceptOpen);

    // read history directory.
    const QString historyDir = m_settings->settings->option("advance.editor.file_dialog_dir")->value().toString();
    if (!historyDir.isEmpty()) {
        dialog.setDirectory(historyDir);
    }

    const int mode = dialog.exec();

    // save the directory string.
    m_settings->settings->option("advance.editor.file_dialog_dir")->setValue(dialog.directoryUrl().toLocalFile());

    if (mode != QDialog::Accepted) {
        return;
    }

    for (const QString &file : dialog.selectedFiles()) {
        addTab(file);
    }
}

void Window::saveAs(const QString &filepath)
{
    if (!m_wrappers.contains(filepath)) {
        return;
    }

#ifdef DTKWIDGET_CLASS_DFileDialog
    DFileDialog dialog(this, tr("Save File"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.addComboBox(tr("Encoding"), getEncodeList());
    dialog.addComboBox(tr("Line Endings"), QStringList() << "Linux" << "Windows" << "Mac OS");
    dialog.setDirectory(QDir::homePath());
    dialog.selectFile(m_tabbar->textAt(m_tabbar->indexOf(filepath)) + ".txt");

    int mode = dialog.exec();
    if (mode == QDialog::Accepted) {
        const QString encode = dialog.getComboBoxValue(tr("Encoding"));
        const QString newline = dialog.getComboBoxValue(tr("Line Endings"));
        const QString newpath = dialog.selectedFiles().value(0);

        if (!filepath.isEmpty()) {
            EditWrapper *wrapper = m_wrappers[filepath];
            wrapper->updatePath(newpath);
            wrapper->saveFile(encode, newline);
        }
    }
#else
    QString fileName = m_tabbar->textAt(m_tabbar->indexOf(filepath) + ".txt");
    QFileDialog dialog(this, tr("Save File"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDirectory(QDir::homePath());
    dialog.selectFile(fileName + ".txt");

    int mode = dialog.exec();

    if (mode == QDialog::Accepted) {
        const QString newpath = dialog.selectedFiles().first();
        if (wrapper.contains(newpath)) {
            EditWrapper *wrapper = m_wrappers[filepath];
            wrapper->updatePath(newpath);
            wrapper->saveFile("UTF-8", "Linux");
        }
    }
#endif
}

const QString Window::getSaveFilePath(QString &encode, QString &newline)
{
    encode = "UTF-8";
    newline = "Linux";

#ifdef DTKWIDGET_CLASS_DFileDialog
    DFileDialog dialog(this, tr("Save File"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.addComboBox(tr("Encoding"), getEncodeList());
    dialog.addComboBox(tr("Line Endings"), QStringList() << "Linux" << "Windows" << "Mac OS");

    if (QFileInfo(m_tabbar->currentPath()).dir().absolutePath() != m_blankFileDir) {
        dialog.setDirectory(QFileInfo(m_tabbar->currentPath()).dir());
        dialog.selectFile(QFileInfo(m_tabbar->currentPath()).fileName());
    } else {
        dialog.setDirectory(QDir::homePath());
        dialog.selectFile(m_tabbar->currentName() + ".txt");
    }

    if (dialog.exec() == QDialog::Accepted) {
        encode = dialog.getComboBoxValue(tr("Encoding"));
        newline = dialog.getComboBoxValue(tr("Line Endings"));

        return dialog.selectedFiles().value(0);
    } else {
        return "";
    }
#else
    return QFileDialog::getSaveFileName(this, tr("Save File"), QDir(QDir::homePath()).filePath("Blank Document.txt"));
#endif
}

void Window::displayShortcuts()
{
    QRect rect = window()->geometry();
    QPoint pos(rect.x() + rect.width() / 2,
               rect.y() + rect.height() / 2);

    QStringList windowKeymaps;
    windowKeymaps << "addblanktab" << "newwindow" << "savefile"
                  << "saveasfile" << "selectnexttab" << "selectprevtab"
                  << "closetab" << "closeothertabs" << "restoretab"
                  << "openfile" << "incrementfontsize" << "decrementfontsize"
                  << "resetfontsize" << "togglefullscreen" << "find" << "replace"
                  << "jumptoline" << "saveposition" << "restoreposition"
                  << "escape" << "displayshortcuts" << "print";

    QJsonObject shortcutObj;
    QJsonArray jsonGroups;

    QJsonObject windowJsonGroup;
    windowJsonGroup.insert("groupName", QObject::tr("Window"));
    QJsonArray windowJsonItems;

    for (const QString &keymap : windowKeymaps) {
        auto option = m_settings->settings->group("shortcuts.window")->option(QString("shortcuts.window.%1").arg(keymap));
        QJsonObject jsonItem;
        jsonItem.insert("name", QObject::tr(option->name().toUtf8().data()));
        jsonItem.insert("value", option->value().toString().replace("Meta", "Super"));
        windowJsonItems.append(jsonItem);
    }

    windowJsonGroup.insert("groupItems", windowJsonItems);
    jsonGroups.append(windowJsonGroup);

    QStringList editorKeymaps;
    editorKeymaps << "indentline" << "backindentline" << "forwardchar"
                  << "backwardchar" << "forwardword" << "backwardword"
                  << "nextline" << "prevline" << "newline" << "opennewlineabove"
                  << "opennewlinebelow" << "duplicateline" << "killline"
                  << "killcurrentline" << "swaplineup" << "swaplinedown"
                  << "scrolllineup" << "scrolllinedown" << "scrollup"
                  << "scrolldown" << "movetoendofline" << "movetostartofline"
                  << "movetoend" << "movetostart" << "movetolineindentation"
                  << "upcaseword" << "downcaseword" << "capitalizeword"
                  << "killbackwardword" << "killforwardword" << "forwardpair"
                  << "backwardpair" << "selectall" << "copy" << "cut"
                  << "paste" << "transposechar" << "setmark" << "exchangemark"
                  << "copylines" << "cutlines" << "joinlines" << "togglereadonlymode"
                  << "togglecomment" << "undo" << "redo";

    QJsonObject editorJsonGroup;
    editorJsonGroup.insert("groupName", tr("Editor"));
    QJsonArray editorJsonItems;

    for (const QString &keymap : editorKeymaps) {
        auto option = m_settings->settings->group("shortcuts.editor")->option(QString("shortcuts.editor.%1").arg(keymap));
        QJsonObject jsonItem;
        jsonItem.insert("name", QObject::tr(option->name().toUtf8().data()));
        jsonItem.insert("value", option->value().toString().replace("Meta", "Super"));
        editorJsonItems.append(jsonItem);
    }
    editorJsonGroup.insert("groupItems", editorJsonItems);
    jsonGroups.append(editorJsonGroup);

    shortcutObj.insert("shortcut", jsonGroups);

    QJsonDocument doc(shortcutObj);

    QStringList shortcutString;
    QString param1 = "-j=" + QString(doc.toJson().data());
    QString param2 = "-p=" + QString::number(pos.x()) + "," + QString::number(pos.y());
    shortcutString << param1 << param2;

    QProcess* shortcutViewProcess = new QProcess();
    shortcutViewProcess->startDetached("deepin-shortcut-viewer", shortcutString);

    connect(shortcutViewProcess, SIGNAL(finished(int)), shortcutViewProcess, SLOT(deleteLater()));
}

bool Window::saveFile()
{
    const QString &currentPath = m_tabbar->currentPath();
    const QString &currentDir = QFileInfo(currentPath).absolutePath();
    bool isBlankFile = QFileInfo(currentPath).dir().absolutePath() == m_blankFileDir;

    // file not finish loadding cannot be saved
    // otherwise you will save the content of the empty.
    if (!m_wrappers[currentPath]->isLoadFinished() && !isBlankFile) {
        showNotify(tr("File cannot be saved when loading"));
        return false;
    }

    // save blank file.
    if (isBlankFile) {
        QString encode, newline;
        QString filepath = getSaveFilePath(encode, newline);

        if (!filepath.isEmpty()) {
            const QString tabPath = m_tabbar->currentPath();
            saveFileAsAnotherPath(tabPath, filepath, encode, newline, true);
            return true;
        } else {
            return false;
        }
    }
    // save root file.
    else if (!m_wrappers[currentPath]->isWritable()) {
        showNotify(QString(tr("You do not have permission to save %1")).arg(m_tabbar->currentName()));
        return false;

        const QString content = getTextEditor(currentPath)->toPlainText();
        bool saveResult = m_rootSaveDBus->saveFile(currentPath.toUtf8(), content.toUtf8(),
                                                   m_wrappers[currentPath]->fileEncode());

        if (saveResult) {
            getTextEditor(currentPath)->setModified(false);
            showNotify(QString("Saved root file %1").arg(m_tabbar->currentName()));
        } else {
            showNotify(QString("Save root file %1 failed.").arg(m_tabbar->currentName()));
        }

        return saveResult;
    }
    // save normal file.
    else {
        bool success = m_wrappers.value(m_tabbar->currentPath())->saveFile();

        if (!success) {
            DDialog *dialog = createSaveFileDialog(tr("Unable to save the file"), tr("Do you want to save as another?"));

            connect(dialog, &DDialog::buttonClicked, this, [=] (int index) {
                dialog->hide();

                if (index == 2) {
                    saveAsFile();
                }
            });

            dialog->exec();
        } else {
            showNotify(tr("Saved successfully"));
        }

        return true;
    }
}

void Window::saveAsFile()
{
    QString encode, newline;
    QString filepath = getSaveFilePath(encode, newline);
    QString tabPath = m_tabbar->currentPath();

    if (filepath != "" && filepath != tabPath) {
        saveFileAsAnotherPath(tabPath, filepath, encode, newline, false);
    } else if (filepath == tabPath) {
        m_wrappers.value(filepath)->saveFile(encode, newline);
    }
}

void Window::saveFileAsAnotherPath(const QString &fromPath, const QString &toPath, const QString &encode, const QString &newline, bool deleteOldFile)
{
    if (deleteOldFile) {
        QFile(fromPath).remove();
    }

    m_tabbar->updateTab(m_tabbar->currentIndex(), toPath, QFileInfo(toPath).fileName());

    m_wrappers[toPath] = m_wrappers.take(fromPath);

    m_wrappers[toPath]->updatePath(toPath);
    m_wrappers[toPath]->saveFile(encode, newline);

    currentWrapper()->textEditor()->loadHighlighter();
}

void Window::decrementFontSize()
{
    int size = std::max(m_fontSize - 1, m_settings->minFontSize);
    m_settings->settings->option("base.font.size")->setValue(size);
}

void Window::incrementFontSize()
{
    int size = std::min(m_fontSize + 1, m_settings->maxFontSize);
    m_settings->settings->option("base.font.size")->setValue(size);
}

void Window::resetFontSize()
{
    m_settings->settings->option("base.font.size")->setValue(m_settings->defaultFontSize);
}

void Window::setFontSizeWithConfig(EditWrapper *wrapper)
{
    int size = m_settings->settings->option("base.font.size")->value().toInt();
    wrapper->textEditor()->setFontSize(size);

    m_fontSize = size;
}

void Window::popupFindBar()
{
    if (m_findBar->isVisible()) {
        if (m_findBar->isFocus()) {
            m_wrappers.value(m_tabbar->currentPath())->textEditor()->setFocus();
        } else {
            m_findBar->focus();
        }
    } else {
        addBottomWidget(m_findBar);

        QString tabPath = m_tabbar->currentPath();
        EditWrapper *wrapper = currentWrapper();
        QString text = wrapper->textEditor()->textCursor().selectedText();
        int row = wrapper->textEditor()->getCurrentLine();
        int column = wrapper->textEditor()->getCurrentColumn();
        int scrollOffset = wrapper->textEditor()->getScrollOffset();

        m_findBar->activeInput(text, tabPath, row, column, scrollOffset);

        QTimer::singleShot(10, this, [=] { m_findBar->focus(); });
    }
}

void Window::popupReplaceBar()
{
    if (m_replaceBar->isVisible()) {
        if (m_replaceBar->isFocus()) {
            m_wrappers.value(m_tabbar->currentPath())->textEditor()->setFocus();
        } else {
            m_replaceBar->focus();
        }
    } else {
        addBottomWidget(m_replaceBar);

        QString tabPath = m_tabbar->currentPath();
        EditWrapper *wrapper = currentWrapper();
        QString text = wrapper->textEditor()->textCursor().selectedText();
        int row = wrapper->textEditor()->getCurrentLine();
        int column = wrapper->textEditor()->getCurrentColumn();
        int scrollOffset = wrapper->textEditor()->getScrollOffset();

        m_replaceBar->activeInput(text, tabPath, row, column, scrollOffset);

        QTimer::singleShot(10, this, [=] { m_replaceBar->focus(); });
    }
}

void Window::popupJumpLineBar()
{
    if (m_jumpLineBar->isVisible()) {
        if (m_jumpLineBar->isFocus()) {
            QTimer::singleShot(0, m_wrappers.value(m_tabbar->currentPath())->textEditor(), SLOT(setFocus()));
        } else {
            m_jumpLineBar->focus();
        }
    } else {
        QString tabPath = m_tabbar->currentPath();
        EditWrapper *wrapper = currentWrapper();
        QString text = wrapper->textEditor()->textCursor().selectedText();
        int row = wrapper->textEditor()->getCurrentLine();
        int column = wrapper->textEditor()->getCurrentColumn();
        int count = wrapper->textEditor()->blockCount();
        int scrollOffset = wrapper->textEditor()->getScrollOffset();

        m_jumpLineBar->activeInput(tabPath, row, column, count, scrollOffset);
    }
}

void Window::toggleFullscreen()
{
    if (isFullScreen()) {
        showNormal();
    }  else {
        showFullScreen();
    }
}

const QStringList Window::getEncodeList()
{
    QStringList encodeList;

    for (int mib : QTextCodec::availableMibs()) {
        QTextCodec *codec = QTextCodec::codecForMib(mib);
        QString encodeName = QString(codec->name()).toUpper();

        if (encodeName != "UTF-8" && !encodeList.contains(encodeName)) {
            encodeList.append(encodeName);
        }
    }

    encodeList.sort();
    encodeList.prepend("UTF-8");

    return encodeList;
}

void Window::remberPositionSave()
{
    EditWrapper *wrapper = currentWrapper();

    m_remberPositionFilePath = m_tabbar->currentPath();
    m_remberPositionRow = wrapper->textEditor()->getCurrentLine();
    m_remberPositionColumn = wrapper->textEditor()->getCurrentColumn();
    m_remberPositionScrollOffset = wrapper->textEditor()->getScrollOffset();
}

void Window::remberPositionRestore()
{
    if (m_remberPositionFilePath.isEmpty()) {
        return;
    }

    if (m_wrappers.contains(m_remberPositionFilePath)) {
        const QString &filePath = m_remberPositionFilePath;
        const int &scrollOffset = m_remberPositionScrollOffset;
        const int &row = m_remberPositionRow;
        const int &column = m_remberPositionColumn;

        activeTab(m_tabbar->indexOf(m_remberPositionFilePath));
        m_wrappers.value(filePath)->textEditor()->scrollToLine(scrollOffset, row, column);
    }
}

void Window::updateFont(const QString &fontName)
{
    for (EditWrapper *wrapper : m_wrappers.values()) {
        wrapper->textEditor()->setFontFamily(fontName);
    }
}

void Window::updateFontSize(int size)
{
    for (EditWrapper *wrapper : m_wrappers.values()) {
        wrapper->textEditor()->setFontSize(size);
    }

    m_fontSize = size;
}

void Window::updateTabSpaceNumber(int number)
{
    for (EditWrapper *wrapper : m_wrappers.values()) {
        wrapper->textEditor()->setTabSpaceNumber(number);
    }
}

void Window::resizeEvent(QResizeEvent*)
{
    if (m_windowShowFlag) {
        QStringList states = m_windowManager->getWindowStates(winId());
        if (!states.contains("_NET_WM_STATE_MAXIMIZED_VERT")) {
            QScreen *screen = QGuiApplication::primaryScreen();
            QRect screenGeometry = screen->geometry();
            m_settings->settings->option("advance.window.window_width")->setValue(rect().width() * 1.0 / screenGeometry.width());
            m_settings->settings->option("advance.window.window_height")->setValue(rect().height() * 1.0 / screenGeometry.height());
        }

        DAnchorsBase::setAnchor(m_themePanel, Qt::AnchorTop, m_centralWidget, Qt::AnchorTop);
        DAnchorsBase::setAnchor(m_themePanel, Qt::AnchorBottom, m_centralWidget, Qt::AnchorBottom);
        DAnchorsBase::setAnchor(m_themePanel, Qt::AnchorRight, m_centralWidget, Qt::AnchorRight);
    }
}

void Window::closeEvent(QCloseEvent *e)
{
    e->ignore();

    QList<EditWrapper *> needSaveList;
    for (EditWrapper *wrapper : m_wrappers) {
        // save all the draft documents.
        if (QFileInfo(wrapper->textEditor()->filepath).dir().absolutePath() == m_blankFileDir) {
            wrapper->saveFile();
            continue;
        }

        if (wrapper->textEditor()->document()->isModified()) {
            needSaveList << wrapper;
        }
    }

    if (!needSaveList.isEmpty()) {
        DDialog *dialog = createSaveFileDialog(tr("Save File"), tr("Do you want to save all the files?"));

        connect(dialog, &DDialog::buttonClicked, this, [=] (int index) {
            dialog->hide();

            if (index == 2) {
                // save all the files.
                for (EditWrapper *wrapper : needSaveList) {
                    wrapper->saveFile();
                }
            }
        });

        const int mode = dialog->exec();
        if (mode == -1 || mode == 0) {
            return;
        }
    }

    // save all draft documents.
    QDir blankDir(m_blankFileDir);
    QFileInfoList blankFiles = blankDir.entryInfoList(QDir::Files);

    // clear blank files that have no content.
    for (const QFileInfo &blankFile : blankFiles) {
        QFile file(blankFile.absoluteFilePath());

        if (!file.open(QFile::ReadOnly)) {
            continue;
        }

        if (file.readAll().simplified().isEmpty()) {
            file.remove();
        }

        file.close();
    }

    e->accept();
    emit close();
}

void Window::keyPressEvent(QKeyEvent *keyEvent)
{
    QString key = Utils::getKeyshortcut(keyEvent);

    if (key == Utils::getKeyshortcutFromKeymap(m_settings, "window", "addblanktab")) {
        addBlankTab();
    } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "window", "newwindow")) {
        emit newWindow();
    } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "window", "savefile")) {
        saveFile();
    } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "window", "saveasfile")) {
        saveAsFile();
    } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "window", "selectnexttab")) {
        m_tabbar->nextTab();
    } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "window", "selectprevtab")) {
        m_tabbar->previousTab();
    } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "window", "closetab")) {
        closeTab();
    } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "window", "restoretab")) {
        restoreTab();
    } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "window", "closeothertabs")) {
        m_tabbar->closeOtherTabs();
    } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "window", "openfile")) {
        openFile();
    } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "window", "incrementfontsize")) {
        incrementFontSize();
    } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "window", "decrementfontsize")) {
        decrementFontSize();
    } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "window", "resetfontsize")) {
        resetFontSize();
    } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "window", "togglefullscreen")) {
        toggleFullscreen();
    } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "window", "find")) {
        popupFindBar();
    } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "window", "replace")) {
        popupReplaceBar();
    } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "window", "jumptoline")) {
        popupJumpLineBar();
    } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "window", "saveposition")) {
        remberPositionSave();
    } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "window", "restoreposition")) {
        remberPositionRestore();
    } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "window", "escape")) {
        removeBottomWidget();
    } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "window", "displayshortcuts")) {
        displayShortcuts();
    } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "window", "print")) {
        popupPrintDialog();
    } else {
        // Post event to window widget if match Alt+0 ~ Alt+9
        QRegularExpression re("^Alt\\+\\d");
        QRegularExpressionMatch match = re.match(key);
        if (match.hasMatch()) {
            auto tabIndex = key.replace("Alt+", "").toInt();
            if (tabIndex == 9) {
                if (m_tabbar->count() > 1) {
                    activeTab(m_tabbar->count() - 1);
                }
            } else {
                if (tabIndex <= m_tabbar->count()) {
                    activeTab(tabIndex - 1);
                }
            }
        }
    }
}

int Window::getBlankFileIndex()
{
    // get blank tab index list.
    QList<int> tabIndexes;

    // tabFiles.size()
    for (int i = 0; i < m_tabbar->count(); ++i) {
        // find all the blank tab index number.
        if (QFileInfo(m_tabbar->fileAt(i)).dir().absolutePath() == m_blankFileDir) {
            const QString tabText = m_tabbar->textAt(i);
            QRegularExpression reg("(\\d+)");
            QRegularExpressionMatch match = reg.match(tabText);

            tabIndexes << match.captured(1).toInt();
        }
    }
    std::sort(tabIndexes.begin(), tabIndexes.end());

    // Return 1 if no blank file exists.
    if (tabIndexes.size() == 0) {
        return 1;
    }

    // Return first mismatch index as new blank file index.
    for (int j = 0; j < tabIndexes.size(); j++) {
        if (tabIndexes[j] != j + 1) {
            return j + 1;
        }
    }

    // Last, return biggest index as blank file index.
    return tabIndexes.size() + 1;
}

void Window::addBlankTab(const QString &blankFile)
{
    QString blankTabPath;

    if (blankFile.isEmpty()) {
        const QString &fileName = QString("blank_file_%1").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd_hh-mm-ss-zzz"));
        blankTabPath = QDir(m_blankFileDir).filePath(fileName);

        if (!Utils::fileExists(blankTabPath)) {
            QDir().mkpath(m_blankFileDir);

            if (QFile(blankTabPath).open(QIODevice::ReadWrite)) {
                qDebug() << "Create blank file: " << blankTabPath;
            } else {
                qDebug() << "Can't create blank file: " << blankTabPath;
            }
        }

    } else {
        blankTabPath = blankFile;
    }

    int blankFileIndex = getBlankFileIndex();

    m_tabbar->addTab(blankTabPath, tr("Blank document %1").arg(blankFileIndex));
    EditWrapper *wrapper = createEditor();
    wrapper->updatePath(blankTabPath);

    if (!blankFile.isEmpty() && Utils::fileExists(blankFile)) {
        wrapper->openFile(blankFile);
    }

    m_wrappers[blankTabPath] = wrapper;
    showNewEditor(wrapper);
}

void Window::handleTabCloseRequested(int index)
{
    activeTab(index);
    closeTab();
}

void Window::handleTabsClosed(const QStringList &tabList)
{
    if (tabList.isEmpty()) {
        return;
    }

    QList<EditWrapper *> needSaveList;
    for (const QString &path : tabList) {
        if (m_wrappers.contains(path)) {
            EditWrapper *wrapper = m_wrappers.value(path);
            bool isBlankFile = QFileInfo(path).dir().absolutePath() == m_blankFileDir;
            bool isContentEmpty = wrapper->textEditor()->toPlainText().isEmpty();
            bool isModified = wrapper->textEditor()->document()->isModified();

            if ( (isBlankFile && !isContentEmpty) ||
                 (!isBlankFile && isModified)) {
                needSaveList << wrapper;
            }
        }
    }

    // popup save file dialog.
    if (!needSaveList.isEmpty()) {
        DDialog *dialog = createSaveFileDialog(tr("Save File"), tr("Do you want to save all the files?"));

        connect(dialog, &DDialog::buttonClicked, this, [&] (int index) {
            dialog->hide();

            // 1: don't save.
            // 2: save
            if (index == 1) {
                // need delete all draft documents.
                for (EditWrapper *wrapper : needSaveList) {
                    if (QFileInfo(wrapper->textEditor()->filepath).dir().absolutePath() == m_blankFileDir) {
                        QFile::remove(wrapper->textEditor()->filepath);
                    }
                }

            } else if (index == 2) {
                for (EditWrapper *wrapper : needSaveList) {
                    QString path = wrapper->textEditor()->filepath;
                    if (QFileInfo(path).dir().absolutePath() == m_blankFileDir) {
                        saveAs(path);
                        QFile::remove(path);
                    } else {
                        wrapper->saveFile();
                    }
                }
            }
        });

        int mode = dialog->exec();
        // click cancel button.
        if (mode == -1 || mode == 0) {
            return;
        }
    }

    // close tabs.
    for (const QString &path : tabList) {
        if (m_wrappers.contains(path)) {
            m_tabbar->closeTab(m_tabbar->indexOf(path));
        }
    }
}

void Window::handleCurrentChanged(const int &index)
{
    if (m_findBar->isVisible()) {
        m_findBar->hide();
    }

    if (m_replaceBar->isVisible()) {
        m_replaceBar->hide();
    }

    for (auto wrapper : m_wrappers.values()) {
        wrapper->textEditor()->removeKeywords();
    }

    const QString &filepath = m_tabbar->fileAt(index);

    if (m_wrappers.contains(filepath)) {
        EditWrapper *wrapper = m_wrappers.value(filepath);
        wrapper->textEditor()->setFocus();
        m_editorWidget->setCurrentWidget(wrapper);
    }
}

void Window::handleJumpLineBarExit()
{
    QTimer::singleShot(0, currentWrapper()->textEditor(), SLOT(setFocus()));
}

void Window::handleJumpLineBarJumpToLine(const QString &filepath, int line, bool focusEditor)
{
    if (m_wrappers.contains(filepath)) {
        getTextEditor(filepath)->jumpToLine(line, true);

        if (focusEditor) {
            QTimer::singleShot(0, getTextEditor(filepath), SLOT(setFocus()));
        }
    }
}

void Window::handleBackToPosition(const QString &file, int row, int column, int scrollOffset)
{
    if (m_wrappers.contains(file)) {
        m_wrappers.value(file)->textEditor()->scrollToLine(scrollOffset, row, column);

        QTimer::singleShot(0, m_wrappers.value(file)->textEditor(), SLOT(setFocus()));
    }
}

void Window::handleFindNext()
{
    EditWrapper *wrapper = currentWrapper();

    wrapper->textEditor()->saveMarkStatus();
    wrapper->textEditor()->updateCursorKeywordSelection(wrapper->textEditor()->getPosition(), true);
    wrapper->textEditor()->renderAllSelections();
    wrapper->textEditor()->restoreMarkStatus();
}

void Window::handleFindPrev()
{
    EditWrapper *wrapper = currentWrapper();

    wrapper->textEditor()->saveMarkStatus();
    wrapper->textEditor()->updateCursorKeywordSelection(wrapper->textEditor()->getPosition(), false);
    wrapper->textEditor()->renderAllSelections();
    wrapper->textEditor()->restoreMarkStatus();
}

void Window::handleReplaceAll(const QString &replaceText, const QString &withText)
{
    EditWrapper *wrapper = currentWrapper();

    wrapper->textEditor()->replaceAll(replaceText, withText);
}

void Window::handleReplaceNext(const QString &replaceText, const QString &withText)
{
    EditWrapper *wrapper = currentWrapper();

    wrapper->textEditor()->replaceNext(replaceText, withText);
}

void Window::handleReplaceRest(const QString &replaceText, const QString &withText)
{
    EditWrapper *wrapper = currentWrapper();

    wrapper->textEditor()->replaceRest(replaceText, withText);
}

void Window::handleReplaceSkip()
{
    EditWrapper *wrapper = currentWrapper();

    wrapper->textEditor()->updateCursorKeywordSelection(wrapper->textEditor()->getPosition(), true);
    wrapper->textEditor()->renderAllSelections();
}

void Window::handleRemoveSearchKeyword()
{
    currentWrapper()->textEditor()->removeKeywords();
}

void Window::handleUpdateSearchKeyword(QWidget *widget, const QString &file, const QString &keyword)
{
    if (file == m_tabbar->currentPath() && m_wrappers.contains(file)) {
        // Highlight keyword in text editor.
        m_wrappers.value(file)->textEditor()->highlightKeyword(keyword, m_wrappers.value(file)->textEditor()->getPosition());

        // Update input widget warning status along with keyword match situation.
        bool findKeyword = m_wrappers.value(file)->textEditor()->findKeywordForward(keyword);
        bool emptyKeyword = keyword.trimmed().isEmpty();

        auto *findBarWidget = qobject_cast<FindBar*>(widget);
        if (findBarWidget != nullptr) {
            if (emptyKeyword) {
                findBarWidget->setMismatchAlert(false);
            } else {
                findBarWidget->setMismatchAlert(!findKeyword);
            }
        } else {
            auto *replaceBarWidget = qobject_cast<ReplaceBar*>(widget);
            if (replaceBarWidget != nullptr) {
                if (emptyKeyword) {
                    replaceBarWidget->setMismatchAlert(false);
                } else {
                    replaceBarWidget->setMismatchAlert(!findKeyword);
                }
            }
        }
    }
}

void Window::addBottomWidget(QWidget *widget)
{
    if (m_centralLayout->count() >= 2) {
        removeBottomWidget();
    }

    m_centralLayout->addWidget(widget);
}

void Window::removeBottomWidget()
{
    auto item = m_centralLayout->takeAt(1);

    if (item) {
        item->widget()->hide();
    }
}

void Window::removeActiveBlankTab(bool needSaveBefore)
{
    QString blankFile = m_tabbar->currentPath();

    if (needSaveBefore) {
        if (!saveFile()) {
            // Do nothing if need save but last user not select save file anyway.
            return;
        }

        // Record last close path.
        m_closeFileHistory << m_tabbar->currentPath();
    }

    // Close current tab.
    m_tabbar->closeCurrentTab();

    // Remove blank file from blank file directory.
    QFile(blankFile).remove();
}

void Window::removeActiveReadonlyTab()
{
    QString tabPath = m_tabbar->currentPath();
    QString realpath = QFileInfo(tabPath).fileName().replace(m_readonlySeparator, QDir().separator());

    m_closeFileHistory << realpath;
    m_tabbar->closeCurrentTab();
    focusActiveEditor();

    QFile(tabPath).remove();
}

void Window::showNewEditor(EditWrapper *wrapper)
{
    m_editorWidget->addWidget(wrapper);
    m_editorWidget->setCurrentWidget(wrapper);
}

void Window::showNotify(const QString &message)
{
    Utils::toast(message, this);
}

DDialog* Window::createSaveFileDialog(QString title, QString content)
{
    DDialog *dialog = new DDialog(title, content, this);
    dialog->setWindowFlags(dialog->windowFlags() | Qt::WindowStaysOnTopHint);
    dialog->setIcon(QIcon(Utils::getQrcPath("logo_48.svg")));
    dialog->addButton(QString(tr("Cancel")), false, DDialog::ButtonNormal);
    dialog->addButton(QString(tr("Discard")), false, DDialog::ButtonNormal);
    dialog->addButton(QString(tr("Save")), true, DDialog::ButtonNormal);

    return dialog;
}

void Window::popupSettingsDialog()
{
    DSettingsDialog *dialog = new DSettingsDialog(this);

    dialog->setProperty("_d_dtk_theme", "dark");
    dialog->setProperty("_d_QSSFilename", "DSettingsDialog");
    DThemeManager::instance()->registerWidget(dialog);

    dialog->updateSettings(m_settings->settings);
    m_settings->dtkThemeWorkaround(dialog, "dlight");

    dialog->exec();
    delete dialog;
    m_settings->settings->sync();
}

void Window::popupPrintDialog()
{
    QPrinter printer(QPrinter::HighResolution);
    QPrintPreviewDialog preview(&printer, this);

    DTextEdit *wrapper = currentWrapper()->textEditor();
    const QString &filePath = wrapper->filepath;
    const QString &fileDir = QFileInfo(filePath).dir().absolutePath();

    if (fileDir == m_blankFileDir) {
        printer.setOutputFileName(QString("%1/%2.pdf").arg(QDir::homePath(), m_tabbar->currentName()));
    } else {
        printer.setOutputFileName(QString("%1/%2.pdf").arg(fileDir, QFileInfo(filePath).baseName()));
    }

    printer.setOutputFormat(QPrinter::PdfFormat);

    connect(&preview, &QPrintPreviewDialog::paintRequested, this, [=] (QPrinter *printer) {
        currentWrapper()->textEditor()->print(printer);
    });

    preview.exec();
}

void Window::changeTitlebarBackground(const QString &color)
{
    titlebar()->setStyleSheet(QString("%1"
                                      "Dtk--Widget--DTitlebar {"
                                      "background: %2;"
                                      "}").arg(m_titlebarStyleSheet).arg(color));

    m_tabbar->setTabActiveColor(m_tabbarActiveColor);
}

void Window::changeTitlebarBackground(const QString &startColor, const QString &endColor)
{
    titlebar()->setStyleSheet(QString("%1"
                                      "Dtk--Widget--DTitlebar {"
                                      "background: qlineargradient(x1:0 y1:0, x2:0 y2:1,"
                                      "stop:0 rgba%2,  stop:1 rgba%3);"
                                      "}").arg(m_titlebarStyleSheet).arg(startColor).arg(endColor));

     m_tabbar->setTabActiveColor(m_tabbarActiveColor);
}

void Window::loadTheme(const QString &path)
{
    m_themePath = path;

    QVariantMap jsonMap = Utils::getThemeMapFromPath(path);
    const QString &backgroundColor = jsonMap["editor-colors"].toMap()["background-color"].toString();
    m_tabbarActiveColor = jsonMap["app-colors"].toMap()["tab-active"].toString();

    const QString &tabbarStartColor = jsonMap["app-colors"].toMap()["tab-background-start-color"].toString();
    const QString &tabbarEndColor = jsonMap["app-colors"].toMap()["tab-background-end-color"].toString();

    if (QColor(backgroundColor).lightness() < 128) {
        DThemeManager::instance()->setTheme("dark");
    } else {
        DThemeManager::instance()->setTheme("light");
    }

    changeTitlebarBackground(tabbarStartColor, tabbarEndColor);

    for (EditWrapper *wrapper : m_wrappers.values()) {
        wrapper->textEditor()->setThemeWithPath(path);
    }

    // set background.
    QPalette palette = this->palette();
    palette.setColor(QPalette::Background, QColor(backgroundColor));
    setPalette(palette);

    m_themePanel->setBackground(backgroundColor);
    m_jumpLineBar->setBackground(backgroundColor);
    m_replaceBar->setBackground(backgroundColor);
    m_findBar->setBackground(backgroundColor);
    m_tabbar->setBackground(tabbarStartColor, tabbarEndColor);
    m_tabbar->setDNDColor(jsonMap["app-colors"].toMap()["tab-dnd-start"].toString(), jsonMap["app-colors"].toMap()["tab-dnd-end"].toString());

    const QString &frameSelectedColor = jsonMap["app-colors"].toMap()["themebar-frame-selected"].toString();
    const QString &frameNormalColor = jsonMap["app-colors"].toMap()["themebar-frame-normal"].toString();
    m_themePanel->setFrameColor(frameSelectedColor, frameNormalColor);
    m_settings->settings->option("advance.editor.theme")->setValue(path);
}

void Window::dragEnterEvent(QDragEnterEvent *event)
{
    // Accept drag event if mime type is url.
    event->accept();
}

void Window::dropEvent(QDropEvent* event)
{
    const QMimeData* mimeData = event->mimeData();

    if (mimeData->hasUrls()) {
        for (auto url : mimeData->urls()) {
            addTab(url.toLocalFile(), true);
        }
    }
}

bool Window::eventFilter(QObject *, QEvent *event)
{
    // Try hide word completion window when window start to move or size change.
    if (m_windowShowFlag && (event->type() == QEvent::MouseMove || event->type() == QEvent::WindowStateChange)) {
    }

    return false;
}

void Window::addBlankTab()
{
    addBlankTab("");
}
