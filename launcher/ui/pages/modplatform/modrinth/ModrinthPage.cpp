/* SPDX-FileCopyrightText: 2026 Project Tick
 * SPDX-FileContributor: Project Tick
 * SPDX-License-Identifier: Apache-2.0
 *
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

#include "ModrinthPage.h"
#include "ui_ModrinthPage.h"

#include <QKeyEvent>

#include "Application.h"
#include "Json.h"
#include "ui/dialogs/NewInstanceDialog.h"
#include "InstanceImportTask.h"
#include "ModrinthModel.h"
#include "modplatform/modrinth/ModrinthApi.h"

ModrinthPage::ModrinthPage(NewInstanceDialog* dialog, QWidget* parent)
	: QWidget(parent), ui(new Ui::ModrinthPage), dialog(dialog)
{
	ui->setupUi(this);
	connect(ui->searchButton, &QPushButton::clicked, this,
			&ModrinthPage::triggerSearch);
	ui->searchEdit->installEventFilter(this);
	listModel = new Modrinth::ListModel(this);
	ui->packView->setModel(listModel);

	ui->versionSelectionBox->view()->setVerticalScrollBarPolicy(
		Qt::ScrollBarAsNeeded);
	ui->versionSelectionBox->view()->parentWidget()->setMaximumHeight(300);

	/* Filled from the provider itself: the combo index is handed
	 * straight back to ModrinthApi, so the two must not be allowed to
	 * drift apart. */
	for (const auto& sorting : ModrinthApi::get().sortingMethods()) {
		ui->sortByBox->addItem(sorting.readableName);
	}

	connect(ui->sortByBox, QOverload<int>::of(qOverload<int>(&QComboBox::currentIndexChanged)),
			this, &ModrinthPage::triggerSearch);
	connect(ui->packView->selectionModel(),
			&QItemSelectionModel::currentChanged, this,
			&ModrinthPage::onSelectionChanged);
	connect(ui->versionSelectionBox, &QComboBox::currentTextChanged, this,
			&ModrinthPage::onVersionSelectionChanged);
}

ModrinthPage::~ModrinthPage()
{
	delete ui;
}

bool ModrinthPage::eventFilter(QObject* watched, QEvent* event)
{
	if (watched == ui->searchEdit && event->type() == QEvent::KeyPress) {
		QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
		if (keyEvent->key() == Qt::Key_Return) {
			triggerSearch();
			keyEvent->accept();
			return true;
		}
	}
	return QWidget::eventFilter(watched, event);
}

bool ModrinthPage::shouldDisplay() const
{
	return true;
}

void ModrinthPage::openedImpl()
{
	suggestCurrent();
	triggerSearch();
}

void ModrinthPage::triggerSearch()
{
	listModel->searchWithTerm(ui->searchEdit->text(),
							  ui->sortByBox->currentIndex());
}

void ModrinthPage::onSelectionChanged(QModelIndex first, QModelIndex second)
{
	ui->versionSelectionBox->clear();

	if (!first.isValid()) {
		if (isOpened) {
			dialog->setSuggestedPack();
		}
		return;
	}

	current =
		listModel->data(first, Qt::UserRole).value<Modrinth::IndexedPack>();
	QString text = "";
	QString name = current.name;

	if (current.slug.isEmpty()) {
		text = name;
	} else {
		text = "<a href=\"https://modrinth.com/modpack/" + current.slug +
			   "\">" + name + "</a>";
	}

	if (!current.author.isEmpty()) {
		text += "<br>" + tr(" by ") + current.author;
	}
	text += "<br><br>";

	ui->packDescription->setHtml(text + current.description);

	if (isOpened) {
		dialog->setSuggestedPack(current.name);
	}

	if (!current.versionsLoaded) {
		qDebug() << "Loading Modrinth modpack versions";
		NetJob* netJob =
			new NetJob(QString("Modrinth::PackVersions(%1)").arg(current.name),
					   APPLICATION->network());
		std::shared_ptr<QByteArray> versionResponse =
			std::make_shared<QByteArray>();
		QString projectId = current.projectId;
		/* A modpack ships its own loader, so accept any of them here
		 * rather than filtering to the instance's. */
		netJob->addNetAction(Net::Download::makeByteArray(
			ModrinthApi::projectVersionsUrlForLoaders(
				projectId, {QStringLiteral("forge"), QStringLiteral("fabric"),
							QStringLiteral("quilt"),
							QStringLiteral("neoforge")}),
			versionResponse.get()));

		QObject::connect(
			netJob, &NetJob::succeeded, this, [this, netJob, versionResponse] {
				netJob->deleteLater();
				QJsonParseError parse_error;
				QJsonDocument doc =
					QJsonDocument::fromJson(*versionResponse, &parse_error);
				if (parse_error.error != QJsonParseError::NoError) {
					qWarning()
						<< "Error while parsing JSON response from Modrinth at "
						<< parse_error.offset
						<< " reason: " << parse_error.errorString();
					qWarning() << *versionResponse;
					return;
				}
				QJsonArray arr = doc.array();
				try {
					Modrinth::loadIndexedPackVersions(current, arr);
				} catch (const JSONValidationError& e) {
					qDebug() << *versionResponse;
					qWarning()
						<< "Error while reading Modrinth modpack version: "
						<< e.cause();
				}

				for (auto version : current.versions) {
					QString label = version.versionNumber;
					if (!version.mcVersion.isEmpty()) {
						label += " [" + version.mcVersion + "]";
					}
					if (!version.loaders.isEmpty()) {
						label += " (" + version.loaders + ")";
					}
					ui->versionSelectionBox->addItem(
						label, QVariant(version.downloadUrl));
				}

				suggestCurrent();
			});
		QObject::connect(netJob, &NetJob::failed, this,
						 [netJob] { netJob->deleteLater(); });
		netJob->start();
	} else {
		for (auto version : current.versions) {
			QString label = version.versionNumber;
			if (!version.mcVersion.isEmpty()) {
				label += " [" + version.mcVersion + "]";
			}
			if (!version.loaders.isEmpty()) {
				label += " (" + version.loaders + ")";
			}
			ui->versionSelectionBox->addItem(label,
											 QVariant(version.downloadUrl));
		}

		suggestCurrent();
	}
}

void ModrinthPage::suggestCurrent()
{
	if (!isOpened) {
		return;
	}

	if (selectedVersion.isEmpty()) {
		dialog->setSuggestedPack();
		return;
	}

	/* Build the source hint up front. Everything we need to seed
	 * PackUpdater is already in `current` (the pack) and the
	 * version that matches the user's selection in the combo box.
	 * The combo stores the version's download URL as user data —
	 * we walk `current.versions` to find the matching IndexedVersion
	 * so we can pull its id + version_number too. */
	InstanceImportTask::PackSourceHint hint;
	hint.provider = QStringLiteral("modrinth");
	hint.packId = current.projectId;
	hint.packSlug = current.slug;
	hint.iconUrl = current.iconUrl;
	if (!current.slug.isEmpty()) {
		hint.sourceUrl =
			QStringLiteral("https://modrinth.com/modpack/%1").arg(current.slug);
	}
	for (const auto& v : current.versions) {
		if (v.downloadUrl == selectedVersion) {
			hint.versionId = v.id;
			hint.versionLabel = v.versionNumber;
			break;
		}
	}

	auto* task = new InstanceImportTask(selectedVersion);
	/* Chosen by the launcher from the catalogue the user is browsing, so
	 * the mod files come from Modrinth's own CDN. */
	task->setTrustedSource(true);
	/* Same as the CurseForge page: any question the task raises is a
	 * question about what the user is doing here. */
	task->setDialogParent(this);
	task->setPackSourceHint(hint);
	dialog->setSuggestedPack(current.name, task);
	QString editedLogoName;
	editedLogoName = "modrinth_" + current.slug;
	listModel->getLogo(
		current.slug, current.iconUrl, [this, editedLogoName](QString logo) {
			dialog->setSuggestedIconFromFile(logo, editedLogoName);
		});
}

void ModrinthPage::onVersionSelectionChanged(QString data)
{
	if (data.isNull() || data.isEmpty()) {
		selectedVersion = "";
		return;
	}
	selectedVersion = ui->versionSelectionBox->currentData().toString();
	suggestCurrent();
}
