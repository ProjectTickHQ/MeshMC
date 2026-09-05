/* SPDX-FileCopyrightText: 2026 Project Tick
 * SPDX-FileContributor: Project Tick
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright 2013-2021 MultiMC Contributors
 * Copyright (C) 2026 Project Tick
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "MeshMCPage.h"
#include "ui_MeshMCPage.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QDir>
#include <QTextCharFormat>

#include "updater/UpdateChecker.h"

#include "settings/SettingsObject.h"
#include <FileSystem.h>
#include "InstanceList.h"
#include "DesktopServices.h"
#include "Application.h"
#include "BuildConfig.h"

#include <QApplication>
#include <QProcess>

// FIXME: possibly move elsewhere
enum InstSortMode {
	// Sort alphabetically by name.
	Sort_Name,
	// Sort by which instance was launched most recently.
	Sort_LastLaunch
};

MeshMCPage::MeshMCPage(QWidget* parent)
	: QWidget(parent), ui(new Ui::MeshMCPage)
{
	ui->setupUi(this);
	auto origForeground =
		ui->fontPreview->palette().color(ui->fontPreview->foregroundRole());
	auto origBackground =
		ui->fontPreview->palette().color(ui->fontPreview->backgroundRole());
	m_colors.reset(new LogColorCache(origForeground, origBackground));

	ui->sortingModeGroup->setId(ui->sortByNameBtn, Sort_Name);
	ui->sortingModeGroup->setId(ui->sortLastLaunchedBtn, Sort_LastLaunch);

	defaultFormat = new QTextCharFormat(ui->fontPreview->currentCharFormat());

	m_languageModel = APPLICATION->translations();
	loadSettings();

	if (BuildConfig.UPDATER_ENABLED && UpdateChecker::isUpdaterSupported()) {
		// New updater: hide the legacy channel selector (no channel selection
		// in the new system).
		ui->updateChannelComboBox->setVisible(false);
		ui->updateChannelLabel->setVisible(false);
		ui->updateChannelDescLabel->setVisible(false);
	} else {
		ui->updateSettingsBox->setHidden(true);
	}
	connect(ui->fontSizeBox, qOverload<int>(&QSpinBox::valueChanged), this,
			&MeshMCPage::refreshFontPreview);
	connect(ui->consoleFont, &QFontComboBox::currentFontChanged, this,
			&MeshMCPage::refreshFontPreview);

	ui->migrateDataFolderMacBtn->setVisible(false);
}

MeshMCPage::~MeshMCPage()
{
	delete ui;
	delete defaultFormat;
}

bool MeshMCPage::apply()
{
	applySettings();
	return true;
}

bool MeshMCPage::confirmInstanceDirPath(const QString& rawDir,
									   const QString& cookedDir)
{
	if (FS::checkProblemticPathJava(QDir(cookedDir))) {
		QMessageBox warning;
		warning.setText(
			tr("You're trying to specify an instance folder which\'s path "
			   "contains at least one \'!\'. "
			   "Java is known to cause problems if that is the case, your "
			   "instances (probably) won't start!"));
		warning.setInformativeText(
			tr("Do you really want to use this path? "
			   "Selecting \"No\" will close this and not alter your "
			   "instance path."));
		warning.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
		return warning.exec() == QMessageBox::Yes;
	}

	/* A folder handed to us through a Flatpak portal is borrowed, not
	 * granted.
	 *
	 * The sandbox exposes a one-off pick under /run/user for the lifetime
	 * of the session. Store it as an instance folder and it works
	 * perfectly until the next restart, at which point the launcher can no
	 * longer see it - and the instances in it read as deleted. The raw path
	 * is what carries this evidence, which is why normalising happens
	 * separately: the cooked path no longer says where it came from.
	 */
	if (DesktopServices::isFlatpak() && rawDir.startsWith("/run/user")) {
		QMessageBox warning;
		warning.setText(
			tr("You're trying to specify an instance folder "
			   "which was granted temporarily via Flatpak.\n"
			   "This is known to cause problems. "
			   "After a restart the launcher might break, "
			   "because it will no longer have access to that directory.\n\n"
			   "Granting %1 access to it via Flatseal is recommended.")
				.arg(BuildConfig.MESHMC_DISPLAYNAME));
		warning.setInformativeText(tr("Do you want to proceed anyway?"));
		warning.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
		return warning.exec() == QMessageBox::Yes;
	}

	return true;
}

void MeshMCPage::on_instDirBrowseBtn_clicked()
{
	QString rawDir = QFileDialog::getExistingDirectory(
		this, tr("Instance Folder"), ui->instDirTextBox->text());

	// do not allow current dir - it's dirty. Do not allow dirs that don't exist
	if (!rawDir.isEmpty() && QDir(rawDir).exists()) {
		QString cookedDir = FS::NormalizePath(rawDir);
		if (confirmInstanceDirPath(rawDir, cookedDir)) {
			ui->instDirTextBox->setText(cookedDir);
		}
	}
}

QStringList MeshMCPage::additionalInstanceDirs() const
{
	QStringList dirs;
	for (int i = 0; i < ui->additionalInstDirsList->count(); i++) {
		dirs << ui->additionalInstDirsList->item(i)->text();
	}
	return dirs;
}

void MeshMCPage::on_addInstDirBtn_clicked()
{
	QString rawDir = QFileDialog::getExistingDirectory(
		this, tr("Additional Instance Folder"));
	if (rawDir.isEmpty() || !QDir(rawDir).exists()) {
		return;
	}

	QString cookedDir = FS::NormalizePath(rawDir);

	/* Refuse a folder that is already in play, primary or additional, and
	 * do it before asking anything else.
	 *
	 * Two entries for one folder is not merely redundant: discovery walks
	 * each configured root and keeps the first claim on an instance id, so
	 * a second pass over the same folder reports every instance in it as a
	 * duplicate and skips it. The list would look richer while instances
	 * went missing.
	 *
	 * Checked ahead of confirmInstanceDirPath() so that a folder being
	 * already known is the whole answer - there is no point warning about
	 * a '!' in a path we are about to refuse for a different reason.
	 *
	 * Matched with MatchFixedString, which ignores case: on a
	 * case-insensitive filesystem two spellings are one folder, and
	 * InstanceList would collapse them anyway - better to say so here,
	 * where there is somebody to tell.
	 */
	if (cookedDir == FS::NormalizePath(ui->instDirTextBox->text())) {
		QMessageBox::warning(
			this, tr("Duplicate directory"),
			tr("This is already your primary instance directory."));
		return;
	}
	if (!ui->additionalInstDirsList
			 ->findItems(cookedDir, Qt::MatchFixedString)
			 .isEmpty()) {
		QMessageBox::warning(this, tr("Duplicate directory"),
							 tr("This directory has already been added."));
		return;
	}

	if (!confirmInstanceDirPath(rawDir, cookedDir)) {
		return;
	}

	ui->additionalInstDirsList->addItem(cookedDir);
}

void MeshMCPage::on_removeInstDirBtn_clicked()
{
	/* Deletes the selection rather than one row by index. The list is
	 * single-selection today, so this removes exactly one item; written
	 * this way it keeps working if the list is ever allowed to take more,
	 * and it does nothing when nothing is selected. */
	qDeleteAll(ui->additionalInstDirsList->selectedItems());
}

void MeshMCPage::on_iconsDirBrowseBtn_clicked()
{
	QString raw_dir = QFileDialog::getExistingDirectory(
		this, tr("Icons Folder"), ui->iconsDirTextBox->text());

	// do not allow current dir - it's dirty. Do not allow dirs that don't exist
	if (!raw_dir.isEmpty() && QDir(raw_dir).exists()) {
		QString cooked_dir = FS::NormalizePath(raw_dir);
		ui->iconsDirTextBox->setText(cooked_dir);
	}
}
void MeshMCPage::on_skinsDirBrowseBtn_clicked()
{
	QString raw_dir = QFileDialog::getExistingDirectory(
		this, tr("Skins Folder"), ui->skinsDirTextBox->text());

	// do not allow current dir - it's dirty. Do not allow dirs that don't exist
	if (!raw_dir.isEmpty() && QDir(raw_dir).exists()) {
		QString cooked_dir = FS::NormalizePath(raw_dir);
		ui->skinsDirTextBox->setText(cooked_dir);
	}
}
void MeshMCPage::on_javaDirBrowseBtn_clicked()
{
	QString raw_dir = QFileDialog::getExistingDirectory(
		this, tr("Java Folder"), ui->javaDirTextBox->text());

	// do not allow current dir - it's dirty. Do not allow dirs that don't exist
	if (!raw_dir.isEmpty() && QDir(raw_dir).exists()) {
		QString cooked_dir = FS::NormalizePath(raw_dir);
		ui->javaDirTextBox->setText(cooked_dir);
	}
}
void MeshMCPage::on_modsDirBrowseBtn_clicked()
{
	QString raw_dir = QFileDialog::getExistingDirectory(
		this, tr("Mods Folder"), ui->modsDirTextBox->text());

	// do not allow current dir - it's dirty. Do not allow dirs that don't exist
	if (!raw_dir.isEmpty() && QDir(raw_dir).exists()) {
		QString cooked_dir = FS::NormalizePath(raw_dir);
		ui->modsDirTextBox->setText(cooked_dir);
	}
}
void MeshMCPage::on_migrateDataFolderMacBtn_clicked()
{
	QMessageBox::information(
		this, tr("Automatic macOS Migration"),
		tr("%1 now stores macOS data under your Library/Application Support "
		   "folder automatically.")
			.arg(BuildConfig.MESHMC_DISPLAYNAME));
}

void MeshMCPage::refreshUpdateChannelList()
{
	// No-op: the new updater does not use named channels.
}

void MeshMCPage::updateChannelSelectionChanged(int)
{
	// No-op.
}

void MeshMCPage::refreshUpdateChannelDesc()
{
	// No-op.
}

void MeshMCPage::applySettings()
{
	auto s = APPLICATION->settings();

	if (ui->resetNotificationsBtn->isChecked()) {
		s->set("ShownNotifications", QString());
	}

	// Updates
	s->set("AutoUpdate", ui->autoUpdateCheckBox->isChecked());
	// (UpdateChannel setting removed - the new updater always checks the stable
	// feed)

	// Instance backups
	s->set("BackupBeforeLaunch", ui->backupBeforeLaunchCheck->isChecked());

	/* Instance creation. The modpack prompt is stored inverted - the
	 * setting says what to *skip* - so that a launcher nobody has
	 * configured asks, which is the useful default and the behaviour
	 * that existed before it was optional. */
	s->set("SkipModpackUpdatePrompt",
		   !ui->modpackUpdatePromptCheck->isChecked());
	s->set("DownloadGameFilesDuringInstanceCreation",
		   ui->downloadGameFilesCheck->isChecked());

	// Console settings
	s->set("ShowConsole", ui->showConsoleCheck->isChecked());
	s->set("AutoCloseConsole", ui->autoCloseConsoleCheck->isChecked());
	s->set("ShowConsoleOnError", ui->showConsoleErrorCheck->isChecked());
	QString consoleFontFamily = ui->consoleFont->currentFont().family();
	s->set("ConsoleFont", consoleFontFamily);
	s->set("ConsoleFontSize", ui->fontSizeBox->value());
	s->set("ConsoleMaxLines", ui->lineLimitSpinBox->value());
	s->set("ConsoleOverflowStop",
		   ui->checkStopLogging->checkState() != Qt::Unchecked);

	// Folders
	// TODO: Offer to move instances to new instance folder.
	s->set("InstanceDir", ui->instDirTextBox->text());
	/* Encoded, not handed over as a list: the INI backend stringifies
	 * every value it writes, and a multi-element QStringList stringifies
	 * to nothing at all. InstanceList owns that detail. */
	s->set("AdditionalInstanceDirs",
		   InstanceList::encodeInstanceDirList(additionalInstanceDirs()));
	s->set("CentralModsDir", ui->modsDirTextBox->text());
	s->set("IconsDir", ui->iconsDirTextBox->text());
	s->set("SkinsDir", ui->skinsDirTextBox->text());
	s->set("JavaDir", ui->javaDirTextBox->text());

	auto sortMode = (InstSortMode)ui->sortingModeGroup->checkedId();
	switch (sortMode) {
		case Sort_LastLaunch:
			s->set("InstSortMode", "LastLaunch");
			break;
		case Sort_Name:
		default:
			s->set("InstSortMode", "Name");
			break;
	}
}
void MeshMCPage::loadSettings()
{
	auto s = APPLICATION->settings();
	// Updates
	ui->autoUpdateCheckBox->setChecked(s->get("AutoUpdate").toBool());
	// (no channel to read in the new updater system)

	// Instance backups
	ui->backupBeforeLaunchCheck->setChecked(
		s->get("BackupBeforeLaunch").toBool());

	// Instance creation
	ui->modpackUpdatePromptCheck->setChecked(
		!s->get("SkipModpackUpdatePrompt").toBool());
	ui->downloadGameFilesCheck->setChecked(
		s->get("DownloadGameFilesDuringInstanceCreation").toBool());

	// Console settings
	ui->showConsoleCheck->setChecked(s->get("ShowConsole").toBool());
	ui->autoCloseConsoleCheck->setChecked(s->get("AutoCloseConsole").toBool());
	ui->showConsoleErrorCheck->setChecked(
		s->get("ShowConsoleOnError").toBool());
	QString fontFamily = APPLICATION->settings()->get("ConsoleFont").toString();
	QFont consoleFont(fontFamily);
	ui->consoleFont->setCurrentFont(consoleFont);

	bool conversionOk = true;
	int fontSize =
		APPLICATION->settings()->get("ConsoleFontSize").toInt(&conversionOk);
	if (!conversionOk) {
		fontSize = 11;
	}
	ui->fontSizeBox->setValue(fontSize);
	refreshFontPreview();
	ui->lineLimitSpinBox->setValue(s->get("ConsoleMaxLines").toInt());
	ui->checkStopLogging->setChecked(s->get("ConsoleOverflowStop").toBool());

	// Folders
	ui->instDirTextBox->setText(s->get("InstanceDir").toString());
	ui->additionalInstDirsList->clear();
	ui->additionalInstDirsList->addItems(InstanceList::decodeInstanceDirList(
		s->get("AdditionalInstanceDirs")));
	ui->modsDirTextBox->setText(s->get("CentralModsDir").toString());
	ui->iconsDirTextBox->setText(s->get("IconsDir").toString());
	ui->skinsDirTextBox->setText(s->get("SkinsDir").toString());
	ui->javaDirTextBox->setText(s->get("JavaDir").toString());

	QString sortMode = s->get("InstSortMode").toString();

	if (sortMode == "LastLaunch") {
		ui->sortLastLaunchedBtn->setChecked(true);
	} else {
		ui->sortByNameBtn->setChecked(true);
	}
}

void MeshMCPage::refreshFontPreview()
{
	int fontSize = ui->fontSizeBox->value();
	QString fontFamily = ui->consoleFont->currentFont().family();
	ui->fontPreview->clear();
	defaultFormat->setFont(QFont(fontFamily, fontSize));
	{
		QTextCharFormat format(*defaultFormat);
		format.setForeground(m_colors->getFront(MessageLevel::Error));
		// append a paragraph/line
		auto workCursor = ui->fontPreview->textCursor();
		workCursor.movePosition(QTextCursor::End);
		workCursor.insertText(tr("[Something/ERROR] A spooky error!"), format);
		workCursor.insertBlock();
	}
	{
		QTextCharFormat format(*defaultFormat);
		format.setForeground(m_colors->getFront(MessageLevel::Message));
		// append a paragraph/line
		auto workCursor = ui->fontPreview->textCursor();
		workCursor.movePosition(QTextCursor::End);
		workCursor.insertText(tr("[Test/INFO] A harmless message..."), format);
		workCursor.insertBlock();
	}
	{
		QTextCharFormat format(*defaultFormat);
		format.setForeground(m_colors->getFront(MessageLevel::Warning));
		// append a paragraph/line
		auto workCursor = ui->fontPreview->textCursor();
		workCursor.movePosition(QTextCursor::End);
		workCursor.insertText(tr("[Something/WARN] A not so spooky warning."),
							  format);
		workCursor.insertBlock();
	}
}
