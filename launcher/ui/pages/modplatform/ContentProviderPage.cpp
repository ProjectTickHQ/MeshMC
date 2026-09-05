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

#include "ContentProviderPage.h"
#include "ui_ContentProviderPage.h"

#include "QtCompat.h"

#include <QDebug>
#include <QDialogButtonBox>
#include <QKeyEvent>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollBar>
#include <QVBoxLayout>
#include <utility>

#include "DesktopServices.h"
#include "modplatform/ModDownloadTypes.h"
#include "ui/dialogs/DownloadContentDialog.h"
#include "ui/widgets/ContentFilterWidget.h"
#include "ui/widgets/ProgressWidget.h"
#include "ui/widgets/ProjectDescriptionPage.h"
#include "ui/widgets/ProjectItemDelegate.h"

ContentProviderPage::ContentProviderPage(DownloadContentDialog* dialog,
										 ContentProviderModel* model,
										 QIcon icon)
	: QWidget(dialog), m_ui(new Ui::ContentProviderPage), m_dialog(dialog),
	  m_model(model), m_icon(std::move(icon))
{
	m_ui->setupUi(this);

	/* The dialog builds the models but the page outlives nothing else,
	 * so it takes ownership from here on. */
	m_model->setParent(this);

	/* Filtering is offered for mods only, as in the reference launcher:
	 * elsewhere there is no loader to pick and no environment to narrow,
	 * and the button would open a panel with almost nothing in it. */
	m_ui->filterWidget->hide();
	if (ModPlatform::contentTypeSupportsFiltering(m_dialog->contentType())) {
		m_ui->filterWidget->setup(m_dialog->contentType(),
								  m_dialog->mcVersion(),
								  m_dialog->loaderType(),
								  m_model->supportsExtendedFilters());
		/* Hidden until asked for: most of the time the instance's own
		 * version and loader are what you want. */
		connect(m_ui->contentFilterButton, &QPushButton::clicked, this, [this] {
			m_ui->filterWidget->setVisible(m_ui->filterWidget->isHidden());
		});

		connect(m_ui->filterWidget, &ContentFilterWidget::searchFilterChanged,
				this, &ContentProviderPage::onSearchFilterChanged);
		connect(m_ui->filterWidget, &ContentFilterWidget::viewFilterChanged,
				this, &ContentProviderPage::onViewFilterChanged);

		/* The category list is a round trip of its own; the group stays
		 * hidden until it lands, so a slow or failed request costs
		 * nothing but a missing group. */
		connect(m_model, &ContentProviderModel::categoriesLoaded,
				m_ui->filterWidget, &ContentFilterWidget::setCategories);
		m_model->loadCategories();
	} else {
		m_ui->contentFilterButton->setVisible(false);
	}

	/* Search progress goes directly under the search box, and only
	 * shows up while a request is in flight. */
	m_searchProgress = new ProgressWidget(this);
	m_searchProgress->hideIfInactive(true);
	m_searchProgress->setFixedHeight(24);
	m_searchProgress->progressFormat("");
	m_searchProgress->hide();
	m_ui->verticalLayout->insertWidget(1, m_searchProgress);

	m_ui->packView->setModel(m_model);
	m_ui->packView->setSelectionMode(QAbstractItemView::SingleSelection);

	auto* delegate = new ProjectItemDelegate(this);
	m_ui->packView->setItemDelegate(delegate);
	/* Enter inside the list must toggle the row rather than travel on to
	 * the dialog's default button, and a middle click toggles too. */
	m_ui->packView->installEventFilter(this);
	m_ui->packView->viewport()->installEventFilter(this);

	m_ui->versionSelectionBox->view()->setVerticalScrollBarPolicy(
		Qt::ScrollBarAsNeeded);
	m_ui->versionSelectionBox->view()->setVerticalScrollMode(
		QAbstractItemView::ScrollPerPixel);

	/* The sort list comes from the provider: the two do not offer the
	 * same sorts, and the index is handed straight back to the API. */
	for (const auto& sorting : m_model->sortingMethods()) {
		m_ui->sortByBox->addItem(sorting.readableName);
	}

	m_searchTimer.setTimerType(Qt::CoarseTimer);
	m_searchTimer.setSingleShot(true);
	connect(&m_searchTimer, &QTimer::timeout, this,
			&ContentProviderPage::triggerSearch);

	connect(m_ui->searchEdit, &QLineEdit::textChanged, this,
			&ContentProviderPage::onSearchTextEdited);
	connect(m_ui->searchEdit, &QLineEdit::returnPressed, this,
			&ContentProviderPage::triggerSearch);
	connect(m_ui->sortByBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
			&ContentProviderPage::onSortChanged);
	connect(m_ui->versionSelectionBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
			&ContentProviderPage::onVersionChanged);
	connect(m_ui->contentSelectionButton, &QPushButton::clicked, this,
			&ContentProviderPage::onSelectionButtonClicked);
	connect(m_ui->packDescription, &QTextBrowser::anchorClicked, this,
			&ContentProviderPage::onAnchorClicked);

	connect(m_ui->packView->selectionModel(),
			&QItemSelectionModel::currentChanged, this,
			&ContentProviderPage::onCurrentChanged);
	connect(m_ui->packView, &QListView::doubleClicked, this,
			&ContentProviderPage::onToggleRequested);
	connect(delegate, &ProjectItemDelegate::checkboxClicked, this,
			&ContentProviderPage::onToggleRequested);

	connect(m_model, &ContentProviderModel::entryUpdated, this,
			&ContentProviderPage::onEntryUpdated);
	connect(m_model, &ContentProviderModel::searchStateChanged, this,
			&ContentProviderPage::onSearchStateChanged);
	connect(m_model, &QAbstractItemModel::modelReset, this,
			[this] { m_pendingToggles.clear(); });
}

ContentProviderPage::~ContentProviderPage()
{
	delete m_ui;
}

QString ContentProviderPage::id() const
{
	return m_model->platformId();
}

QString ContentProviderPage::displayName() const
{
	return m_model->platformDisplayName();
}

QIcon ContentProviderPage::icon() const
{
	return m_icon;
}

void ContentProviderPage::openedImpl()
{
	m_ui->searchEdit->setPlaceholderText(
		tr("Search for %1...").arg(m_dialog->contentsNoun()));
	updateSelectionButton();
	if (m_suppressInitialSearch) {
		/* One page's worth only: switching back here later is an
		 * ordinary visit and should search. */
		m_suppressInitialSearch = false;
	} else {
		triggerSearch();
	}
	m_ui->searchEdit->setFocus();
}

void ContentProviderPage::setSuppressInitialSearch(bool suppress)
{
	m_suppressInitialSearch = suppress;
}

QString ContentProviderPage::searchTerm() const
{
	return m_ui->searchEdit->text();
}

void ContentProviderPage::setSearchTerm(const QString& term)
{
	m_ui->searchEdit->setText(term);
}

void ContentProviderPage::queueChanged(const QSet<QString>& queuedNames)
{
	/* The tick itself is model state, so that the delegate can draw a
	 * row without asking anyone. */
	m_model->setSelectedNames(queuedNames);
	updateSelectionButton();
}

void ContentProviderPage::setInstalledIndex(
	std::shared_ptr<ModMetadataIndex> index)
{
	m_model->setInstalledIndex(std::move(index));
	updateSelectionButton();
}

bool ContentProviderPage::eventFilter(QObject* watched, QEvent* event)
{
	if (event->type() == QEvent::KeyPress) {
		auto* keyEvent = static_cast<QKeyEvent*>(event);
		if (watched == m_ui->packView &&
			(keyEvent->key() == Qt::Key_Return ||
			 keyEvent->key() == Qt::Key_Enter)) {
			/* Otherwise the key would travel on to the dialog's default
			 * button and close the whole thing. */
			onToggleRequested(m_ui->packView->currentIndex());
			keyEvent->accept();
			return true;
		}
	} else if (watched == m_ui->packView->viewport() &&
			   event->type() == QEvent::MouseButtonPress) {
		auto* mouseEvent = static_cast<QMouseEvent*>(event);
		if (mouseEvent->button() == Qt::MiddleButton) {
			onToggleRequested(
				m_ui->packView->indexAt(QtCompat::mousePosition(mouseEvent).toPoint()));
			return true;
		}
	}

	return QWidget::eventFilter(watched, event);
}

void ContentProviderPage::onSearchTextEdited()
{
	m_searchTimer.start(350);
}

void ContentProviderPage::onSortChanged(int index)
{
	Q_UNUSED(index)
	triggerSearch();
}

void ContentProviderPage::triggerSearch()
{
	m_searchTimer.stop();

	/* In version-change mode the rows are one looked-up project, and a
	 * search would throw it away. The search box is hidden there, but
	 * the page is also searched when it is opened - and it is opened
	 * again whenever its container brings it back to the front. */
	if (m_versionChangeMode) {
		return;
	}

	const QString text = m_ui->searchEdit->text().trimmed();

	/* A pasted project link is a request to open that project, not to
	 * search for the address. Only bother when it actually looks like
	 * one, so an ordinary term is never mangled. */
	if (text.contains(QLatin1String("://")) ||
		text.contains(QLatin1String(".com/"))) {
		if (m_dialog->openProjectLink(QUrl::fromUserInput(text))) {
			return;
		}
	}

	m_model->search(text, m_ui->sortByBox->currentIndex());

	/* search() ignores a repeat of the current query, in which case no
	 * state change is coming and the jump has to happen right here. */
	if (!m_model->isSearching()) {
		selectPendingSlug();
	}
}

void ContentProviderPage::openProjectSlug(const QString& slug)
{
	m_pendingSlug = slug;
	m_ui->searchEdit->setText(slug);
	triggerSearch();
}

void ContentProviderPage::selectPendingSlug()
{
	if (m_pendingSlug.isEmpty()) {
		return;
	}

	for (int row = 0; row < m_model->rowCount(QModelIndex()); ++row) {
		const auto* project = m_model->projectAt(row);
		if (project == nullptr ||
			project->slug.compare(m_pendingSlug, Qt::CaseInsensitive) != 0) {
			continue;
		}
		m_pendingSlug.clear();
		m_ui->packView->setCurrentIndex(m_model->index(row));
		m_ui->packView->scrollTo(m_model->index(row));
		return;
	}

	/* Not on this page of results. Give up rather than paging through
	 * the whole catalogue looking for it. */
	m_pendingSlug.clear();
}

void ContentProviderPage::openProject(const QString& projectId)
{
	if (projectId.isEmpty()) {
		return;
	}

	/* Everything that would let the user go somewhere else is taken
	 * away: the project is settled, only the version is in question. */
	m_versionChangeMode = true;
	m_ui->sortByBox->hide();
	m_ui->searchEdit->hide();
	m_ui->contentFilterButton->hide();
	m_ui->filterWidget->hide();
	m_ui->packView->hide();
	m_ui->contentSelectionButton->hide();

	/* Into the cell the selection button just vacated, so the version
	 * box keeps the row above it. */
	auto* buttonBox = new QDialogButtonBox(
		QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

	auto* okButton = buttonBox->button(QDialogButtonBox::Ok);
	okButton->setDefault(true);
	okButton->setAutoDefault(true);
	okButton->setText(tr("Reinstall"));
	okButton->setShortcut(tr("Ctrl+Return"));
	/* Nothing is selected yet - the version list has not even been
	 * asked for. The connection below opens this up. */
	okButton->setEnabled(false);

	auto* cancelButton = buttonBox->button(QDialogButtonBox::Cancel);
	cancelButton->setDefault(false);
	cancelButton->setAutoDefault(false);
	cancelButton->setText(tr("Cancel"));

	connect(okButton, &QPushButton::clicked, this, [this] {
		/* Same path as the ordinary selection button: the picked version
		 * goes into the dialog's queue, and the caller takes it from
		 * there through the usual resolve/review/download pipeline. */
		onSelectionButtonClicked();
		m_dialog->accept();
	});
	connect(cancelButton, &QPushButton::clicked, m_dialog, &QDialog::reject);

	m_ui->bottomLayout->addWidget(buttonBox, 1, 2);

	connect(m_ui->versionSelectionBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
			[this, okButton](int index) {
				/* The "No valid version found." placeholder carries -1,
				 * a real version its position in the list. */
				okButton->setEnabled(
					m_ui->versionSelectionBox->itemData(index).toInt() >= 0);
			});

	m_awaitingLookedUpProject = true;
	m_model->searchProject(projectId);

	/* An abort can complete synchronously, in which case no further
	 * state change is coming and the row has to be picked up here. */
	if (!m_model->isSearching()) {
		selectLookedUpProject();
	}
}

void ContentProviderPage::selectLookedUpProject()
{
	if (!m_awaitingLookedUpProject) {
		return;
	}
	m_awaitingLookedUpProject = false;

	if (m_model->rowCount(QModelIndex()) > 0) {
		/* Selecting the row is what fetches its version list and
		 * description, which is the whole point of this mode. */
		m_ui->packView->setCurrentIndex(m_model->index(0));
		return;
	}

	m_ui->packDescription->setText(tr("The resource was not found"));
}

void ContentProviderPage::onSearchStateChanged()
{
	/* watch() copes with a null task, and the widget hides itself when
	 * nothing is running. */
	m_searchProgress->watch(m_model->activeSearchJob());

	if (!m_model->isSearching()) {
		selectPendingSlug();
		selectLookedUpProject();
	}
}

void ContentProviderPage::onSearchFilterChanged()
{
	m_model->setSearchFilters(m_ui->filterWidget->searchFilters());
}

void ContentProviderPage::onViewFilterChanged()
{
	const auto& filter = m_ui->filterWidget->filter();
	m_model->setHideInstalled(filter.hideInstalled);

	/* The channel filter changes which versions the box may offer, so it
	 * has to be rebuilt even though the project did not change - and the
	 * key alone would not notice, since it only tracks the project and
	 * how many versions it has. */
	m_versionListKey.clear();
	updateVersionList();
	updateSelectionButton();
}

int ContentProviderPage::currentRow() const
{
	const QModelIndex current = m_ui->packView->currentIndex();
	return current.isValid() ? current.row() : -1;
}

const ModPlatform::IndexedProject* ContentProviderPage::currentProject() const
{
	const int row = currentRow();
	return row < 0 ? nullptr : m_model->projectAt(row);
}

void ContentProviderPage::onCurrentChanged(const QModelIndex& current,
										   const QModelIndex& previous)
{
	Q_UNUSED(previous)

	if (!current.isValid()) {
		m_ui->versionSelectionBox->clear();
		m_versionListKey.clear();
		updateDescription();
		updateSelectionButton();
		return;
	}

	/* Versions and the long description are fetched on demand; calling
	 * this repeatedly for the same row is free. */
	m_model->loadEntry(current.row());

	updateDescription();
	updateVersionList();
	updateSelectionButton();
}

void ContentProviderPage::onEntryUpdated(int row)
{
	if (row == currentRow()) {
		updateDescription();
		updateVersionList();
		updateSelectionButton();
	}

	/* A row ticked before its versions were known can be queued now. */
	if (m_pendingToggles.remove(row)) {
		onToggleRequested(m_model->index(row));
	}
}

void ContentProviderPage::updateDescription()
{
	const auto* project = currentProject();
	if (!project) {
		m_ui->packDescription->flush();
		m_ui->packDescription->clear();
		m_descriptionKey.clear();
		return;
	}

	/* Rebuilding the document scrolls it back to the top and restarts
	 * its image loads, so only do it when the text would differ. */
	const QString key =
		QString("%1|%2").arg(project->projectId,
							 project->bodyHtml.isEmpty() ? "0" : "1");
	if (key == m_descriptionKey) {
		return;
	}
	m_descriptionKey = key;
	m_ui->packDescription->flush();

	QString html;
	if (project->websiteUrl.isEmpty()) {
		html = project->name.toHtmlEscaped();
	} else {
		html = QString("<a href=\"%1\">%2</a>")
				   .arg(project->websiteUrl.toHtmlEscaped(),
						project->name.toHtmlEscaped());
	}

	if (!project->author.isEmpty()) {
		html += "<br>" + tr("by %1").arg(project->author.toHtmlEscaped());
	}

	/* No "already installed" note here: the reference launcher says it
	 * with the [installed] suffix on the list row and the matching tag
	 * in the version box, and repeating it in the description would be
	 * a third place to keep in step. */

	html += "<hr>";

	/* The long description arrives after the search result, so show the
	 * summary until it does rather than an empty pane. */
	if (project->bodyHtml.isEmpty()) {
		html += project->description.toHtmlEscaped();
	} else {
		html += project->bodyHtml;
	}

	m_ui->packDescription->setHtml(html);
	m_ui->packDescription->verticalScrollBar()->setValue(0);
}

bool ContentProviderPage::versionAllowed(
	const ModPlatform::ContentVersion& version) const
{
	/* A version whose channel the provider did not state matches the
	 * empty channel, which the panel offers as "Unknown". CurseForge
	 * leaves the release type out often enough that this needs to be a
	 * box of its own rather than an unconditional pass. */
	return m_ui->filterWidget->allowsVersionChannel(version.versionType);
}

int ContentProviderPage::firstAllowedVersionIndex(
	const ModPlatform::IndexedProject& project) const
{
	/* Versions arrive newest first, so the first match is the newest
	 * one the filter allows. */
	for (int i = 0; i < project.versions.size(); ++i) {
		if (versionAllowed(project.versions.at(i))) {
			return i;
		}
	}
	return -1;
}

void ContentProviderPage::updateVersionList()
{
	const auto* project = currentProject();

	/* Only rebuild when the contents would actually differ: the row is
	 * refreshed again when its description lands, and that must not
	 * silently reset the version the user picked. */
	const QString key =
		project == nullptr
			? QString()
			: QString("%1/%2").arg(project->projectId,
								   QString::number(project->versions.size()));
	if (key == m_versionListKey) {
		return;
	}
	m_versionListKey = key;

	/* Only clear() is silenced, and deliberately so: it reports index -1,
	 * whose itemData() is an empty QVariant that reads back as 0 - which
	 * looks like a perfectly good version to anything watching. The
	 * addItem() calls below are left to report normally, because the
	 * "Reinstall" button of the version-change flow learns from them that
	 * a real version is now selected. */
	m_ui->versionSelectionBox->blockSignals(true);
	m_ui->versionSelectionBox->clear();
	m_ui->versionSelectionBox->blockSignals(false);

	if (project) {
		for (int i = 0; i < project->versions.size(); ++i) {
			const auto& version = project->versions.at(i);
			if (!versionAllowed(version)) {
				continue;
			}
			QString text = version.name;
			if (!version.versionType.isEmpty()) {
				text += QString(" [%1]").arg(version.versionType);
			}
			if (!version.versionId.isEmpty() &&
				version.versionId ==
					m_dialog->installedVersionId(m_model->platformId(),
												 project->projectId)) {
				//: Marks the version of a mod that is already on disk
				text += tr(" [installed]");
			}
			m_ui->versionSelectionBox->addItem(text, i);
		}
	}

	if (m_ui->versionSelectionBox->count() == 0) {
		m_ui->versionSelectionBox->addItem(tr("No valid version found."), -1);
	}
}

int ContentProviderPage::selectedVersionIndex() const
{
	if (m_ui->versionSelectionBox->count() == 0) {
		return -1;
	}
	return m_ui->versionSelectionBox->currentData().toInt();
}

void ContentProviderPage::onVersionChanged(int index)
{
	Q_UNUSED(index)
	updateSelectionButton();
}

void ContentProviderPage::updateSelectionButton()
{
	auto* button = m_ui->contentSelectionButton;
	const QString noun = m_dialog->contentNoun();

	const auto* project = currentProject();
	if (!project) {
		button->setText(tr("Select %1 for download").arg(noun));
		button->setEnabled(false);
		return;
	}

	if (m_dialog->isNameQueued(project->name)) {
		button->setText(tr("Deselect %1 for download").arg(noun));
		button->setEnabled(true);
		return;
	}

	if (!project->versionsLoaded) {
		button->setText(tr("Loading versions..."));
		button->setEnabled(false);
		return;
	}

	if (project->versions.isEmpty() || selectedVersionIndex() < 0) {
		button->setText(tr("Cannot select invalid version :("));
		button->setEnabled(false);
		return;
	}

	button->setText(tr("Select %1 for download").arg(noun));
	button->setEnabled(true);
}

void ContentProviderPage::onSelectionButtonClicked()
{
	onToggleRequested(m_ui->packView->currentIndex());
}

void ContentProviderPage::onToggleRequested(const QModelIndex& index)
{
	if (!index.isValid()) {
		return;
	}

	const auto* project = m_model->projectAt(index.row());
	if (!project) {
		return;
	}

	// Ticking something already queued takes it back out of the queue.
	if (m_dialog->isNameQueued(project->name)) {
		m_dialog->unqueueContent(project->name);
		return;
	}

	/* An installed project is deliberately not refused: picking a
	 * different version of something already on disk is how it gets
	 * changed. The download plan works out whether that means replacing
	 * the file or doing nothing at all. */

	if (!project->versionsLoaded) {
		/* Nothing to queue yet. Remember the intent and let the reply
		 * finish the job - the row does not even have to be selected. */
		m_pendingToggles.insert(index.row());
		m_model->loadEntry(index.row());
		return;
	}

	if (project->versions.isEmpty()) {
		/* Nothing to offer. Saying so is better than a click that
		 * appears to do nothing at all. */
		auto* message = new QMessageBox(
			QMessageBox::Warning, tr("No versions available"),
			tr("No versions of '%1' are available for this instance.\n"
			   "The author may have blocked third-party launchers, or "
			   "there may be no build for this Minecraft version.")
				.arg(project->name),
			QMessageBox::Ok, this);
		message->setAttribute(Qt::WA_DeleteOnClose);
		message->open();
		return;
	}

	/* For the row the user is looking at, honour the version they
	 * picked; for any other row there is no picker, so take the newest. */
	int versionIndex = -1;
	if (index.row() == currentRow()) {
		const int picked = selectedVersionIndex();
		if (picked >= 0 && picked < project->versions.size()) {
			versionIndex = picked;
		}
	}
	if (versionIndex < 0) {
		/* Either another row, or the box is showing the "no valid
		 * version" placeholder. Take the newest version the release
		 * channel filter still allows. */
		versionIndex = firstAllowedVersionIndex(*project);
	}

	if (versionIndex < 0) {
		/* Every version this project has is on a channel the filter is
		 * hiding, so there is nothing to queue. Saying so beats a click
		 * that appears to do nothing. */
		auto* message = new QMessageBox(
			QMessageBox::Warning, tr("No versions available"),
			tr("Every version of '%1' is on a release channel that the "
			   "filter is currently hiding.")
				.arg(project->name),
			QMessageBox::Ok, this);
		message->setAttribute(Qt::WA_DeleteOnClose);
		message->open();
		return;
	}

	queueProject(index.row(), versionIndex);
}

void ContentProviderPage::queueProject(int row, int versionIndex)
{
	const auto* project = m_model->projectAt(row);
	if (!project || versionIndex < 0 ||
		versionIndex >= project->versions.size()) {
		return;
	}

	const auto& version = project->versions.at(versionIndex);

	ModPlatform::SelectedMod selected;
	selected.name = project->name;
	selected.projectId = project->projectId;
	selected.slug = project->slug;
	selected.versionId = version.versionId;
	selected.fileName = version.fileName;
	selected.downloadUrl = version.downloadUrl;
	selected.sha1 = version.sha1;
	selected.fileSize = version.fileSize;
	selected.platform = m_model->platformId();
	selected.mcVersion = m_dialog->mcVersion();
	selected.loaders = m_dialog->loaderType();
	/* Carried through for the review dialog, which says what kind of
	 * build each file is before anything is downloaded. */
	selected.versionType = version.versionType;
	selected.browserDownloadOnly = version.browserDownloadOnly;

	m_dialog->queueContent(selected);
}

void ContentProviderPage::onAnchorClicked(const QUrl& url)
{
	/* The browser is deliberately not allowed to follow links itself
	 * (openLinks is off in the .ui), so that a link to another project
	 * can be opened here instead of in a web browser.
	 *
	 * Not in version-change mode though: this window is about one
	 * project's versions, and swapping the project out from under it
	 * would leave the "Reinstall" button pointing at something else. */
	if (!m_versionChangeMode && m_dialog->openProjectLink(url)) {
		return;
	}

	if (url.scheme() != "http" && url.scheme() != "https") {
		qWarning() << "Refusing to open link with scheme" << url.scheme();
		return;
	}
	DesktopServices::openUrl(url);
}
