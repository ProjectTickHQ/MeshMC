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

#include "ManagedPackPage.h"
#include "ui_ManagedPackPage.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QTextBrowser>
#include <QUrlQuery>
#include <memory>

#include "Application.h"
#include "HoeDown.h"
#include "InstanceImportTask.h"
#include "InstanceList.h"
#include "InstanceTask.h"
#include "MMCStrings.h"
#include "QObjectPtr.h"
#include "modplatform/flame/FlameApi.h"
#include "modplatform/modrinth/ModrinthApi.h"
#include "net/Download.h"
#include "ui/InstanceWindow.h"
#include "ui/dialogs/CustomMessageBox.h"
#include "ui/dialogs/ProgressDialog.h"

/* Insert a line break between a list and a picture that immediately
 * follows it.
 *
 * QTextBrowser lays out an <img> that directly follows </ul> flush
 * against the last bullet, which in a changelog full of "fixed X / see
 * screenshot" pairs makes the picture look like part of the list item.
 * There is no stylesheet fix for it, so the break goes into the markup.
 *
 * Kept local rather than added to Strings:: because it only makes sense
 * for provider-authored changelog HTML, and Strings::htmlListPatch is
 * already applied to every project description in the launcher - giving
 * it a second, unrelated job would change all of those too. */
static QString breakBeforeImagesAfterLists(QString html)
{
	static const QRegularExpression listEnd(QStringLiteral("<\\s*/\\s*ul\\s*>"));

	int searchFrom = html.indexOf(listEnd);
	while (searchFrom != -1) {
		/* Step past the closing ">" of the </ul> we just found. */
		const int afterTag = html.indexOf(QLatin1Char('>'), searchFrom) + 1;
		if (afterTag <= 0) {
			break;
		}

		const int imageStart = html.indexOf(QStringLiteral("<img "), afterTag);
		if (imageStart == -1) {
			/* No picture anywhere after this list; nothing left to fix. */
			break;
		}

		/* Only when the two are genuinely adjacent - anything else in
		 * between already separates them. */
		if (html.mid(afterTag, imageStart - afterTag).trimmed().isEmpty()) {
			html.insert(afterTag, QStringLiteral("<br>"));
		}

		searchFrom = html.indexOf(listEnd, afterTag);
	}
	return html;
}

ManagedPackPage::Provider
ManagedPackPage::providerFromString(const QString& provider)
{
	const QString normalised = provider.trimmed().toLower();
	if (normalised == QLatin1String("modrinth")) {
		return Provider::Modrinth;
	}
	/* "flame" is what the upstream launchers call CurseForge. MeshMC
	 * writes "curseforge", but an instance.cfg that came from elsewhere
	 * should not lose its provider over vocabulary. */
	if (normalised == QLatin1String("curseforge") ||
		normalised == QLatin1String("flame")) {
		return Provider::CurseForge;
	}
	return Provider::Unknown;
}

bool ManagedPackPage::isSupported(const BaseInstance* instance)
{
	if (instance == nullptr || !instance->isManagedPack()) {
		return false;
	}

	const Provider provider =
		providerFromString(instance->managedPackProvider());
	if (provider == Provider::Unknown) {
		return false;
	}

	if (provider == Provider::CurseForge &&
		APPLICATION->settings()->get("CurseForgeAPIKey").toString().isEmpty()) {
		/* Every request this page makes to CurseForge needs that key,
		 * and a build without one cannot list a single version. Showing
		 * the tab anyway would offer a feature that can only fail, so it
		 * is better not to offer it: the instance is still a CurseForge
		 * pack, we just have no way to talk to the catalogue about it. */
		return false;
	}

	return true;
}

ManagedPackPage::ManagedPackPage(BaseInstance* instance,
								 InstanceWindow* instanceWindow,
								 QWidget* parent)
	: QWidget(parent), ui(new Ui::ManagedPackPage), m_instance(instance),
	  m_instanceWindow(instanceWindow)
{
	Q_ASSERT(instance);

	ui->setupUi(this);

	m_provider = providerFromString(m_instance->managedPackProvider());

	/* A pack can have hundreds of versions. Without these the popup
	 * grows past the screen and scrolls a whole item at a time. */
	ui->versionsComboBox->view()->setVerticalScrollBarPolicy(
		Qt::ScrollBarAsNeeded);
	ui->versionsComboBox->view()->setVerticalScrollMode(
		QAbstractItemView::ScrollPerPixel);

	/* Only offered once something has actually failed. */
	ui->reloadButton->setVisible(false);

	connect(ui->reloadButton, &QPushButton::clicked, this,
			&ManagedPackPage::reload);
	connect(ui->versionsComboBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
			&ManagedPackPage::suggestVersion);
	connect(ui->updateButton, &QPushButton::clicked, this,
			&ManagedPackPage::update);
	connect(ui->updateFromFileButton, &QPushButton::clicked, this,
			&ManagedPackPage::updateFromFile);

	/* The changelog browser has openLinks off so that clicks come here
	 * instead of QTextBrowser trying to navigate the pane itself. */
	connect(ui->changelogTextBrowser, &QTextBrowser::anchorClicked, this,
			[](const QUrl& url) {
				if (!url.scheme().isEmpty()) {
					QDesktopServices::openUrl(url);
					return;
				}

				/* A schemeless link is CurseForge's outbound redirect,
				 * which in changelog HTML arrives as the relative
				 * "linkout?remoteUrl=<percent-encoded>". The real
				 * destination is in the query. */
				const QString target =
					QUrlQuery(url.query())
						.queryItemValue(QStringLiteral("remoteUrl"),
										QUrl::FullyDecoded);
				const QUrl decoded(QUrl::fromPercentEncoding(target.toUtf8()));

				/* Only http(s). A changelog is remote, untrusted text,
				 * and handing an arbitrary scheme to the desktop is how
				 * a "file:" or worse ends up being opened. */
				if (decoded.isValid() && (decoded.scheme() == "http" ||
										  decoded.scheme() == "https")) {
					QDesktopServices::openUrl(decoded);
				}
			});

	connect(ui->urlLine, &QLineEdit::textChanged, this,
			[this](const QString& text) {
				m_instance->setManagedPackUpdateUrl(text);
			});
}

ManagedPackPage::~ManagedPackPage()
{
	/* Replies must not land on a half-destroyed page. */
	if (m_versionsJob) {
		m_versionsJob->abort();
	}
	if (m_changelogJob) {
		m_changelogJob->abort();
	}
	delete ui;
}

QString ManagedPackPage::displayName() const
{
	switch (m_provider) {
		case Provider::Modrinth:
			return QStringLiteral("Modrinth");
		case Provider::CurseForge:
			return QStringLiteral("CurseForge");
		case Provider::Unknown:
			break;
	}
	return QString();
}

QIcon ManagedPackPage::icon() const
{
	/* The instance-icon set ships these under the names the launcher
	 * already uses for pack provenance elsewhere; "flame" is the
	 * historical file name for the CurseForge mark. */
	switch (m_provider) {
		case Provider::Modrinth:
			return QIcon::fromTheme(QStringLiteral("modrinth"));
		case Provider::CurseForge:
			return QIcon::fromTheme(QStringLiteral("flame"));
		case Provider::Unknown:
			break;
	}
	return QIcon();
}

QString ManagedPackPage::helpPage() const
{
	/* Per provider, because the two have genuinely different things to
	 * explain - CurseForge's blocked downloads and third-party
	 * distribution opt-out have no Modrinth equivalent. */
	switch (m_provider) {
		case Provider::Modrinth:
			return QStringLiteral("Modrinth-Managed-Pack");
		case Provider::CurseForge:
			return QStringLiteral("CurseForge-Managed-Pack");
		case Provider::Unknown:
			break;
	}
	return QStringLiteral("Instance-Modpack");
}

bool ManagedPackPage::shouldDisplay() const
{
	return isSupported(m_instance);
}

QString ManagedPackPage::packUrl() const
{
	const QString packId = m_instance->managedPackId();

	switch (m_provider) {
		case Provider::Modrinth:
			/* Modrinth accepts either the slug or the id on this path.
			 * The slug is preferred because it is what the user would
			 * see in a browser, but it is not always recorded. */
			{
				const QString slug = m_instance->managedPackSlug();
				return ModrinthApi::get()
					.projectPageUrl(slug.isEmpty() ? packId : slug)
					.toString();
			}
		case Provider::CurseForge:
			/* CurseForge has no addressable page keyed by project id
			 * alone; the API's own project route redirects to the real
			 * one, which is the best we can do without an extra
			 * request just to render a label. */
			return FlameApi::get().projectPageUrl(packId).toString();
		case Provider::Unknown:
			break;
	}

	/* Nothing better than whatever the importer recorded. */
	return m_instance->managedPackSourceUrl();
}

void ManagedPackPage::openedImpl()
{
	if (!m_instance->hasManagedPackId()) {
		showLocalPackMode();
		return;
	}

	/* The URL field belongs to the no-pack-id mode only. */
	ui->urlLine->hide();

	showPackInformation();
	fetchVersions();
}

void ManagedPackPage::showPackInformation()
{
	ui->packName->setText(m_instance->managedPackName());
	ui->packVersion->setText(m_instance->managedPackVersionName());
	ui->packOrigin->setText(
		tr("Website: <a href=%1>%2</a>    |    Pack ID: %3    |    Version "
		   "ID: %4")
			.arg(packUrl(), displayName(), m_instance->managedPackId(),
				 m_instance->managedPackVersionId()));
}

void ManagedPackPage::showLocalPackMode()
{
	/* Nothing to say about a catalogue entry we do not have. */
	ui->packVersion->hide();
	ui->packVersionLabel->hide();
	ui->packOrigin->hide();
	ui->packOriginLabel->hide();
	ui->versionsComboBox->hide();

	ui->updateToVersionLabel->setText(tr("URL:"));
	ui->updateButton->setText(tr("Update Pack"));
	ui->updateButton->setDisabled(false);
	ui->urlLine->setText(m_instance->managedPackUpdateUrl());

	ui->packName->setText(m_instance->name());
	ui->changelogTextBrowser->setText(
		tr("This is a local modpack.\n"
		   "This can be updated either using a file in %1 format or an URL.\n"
		   "Do not use a different format than the one mentioned as it may "
		   "break the instance.\n"
		   "Make sure you also trust the URL.\n")
			.arg(displayName()));
}

void ManagedPackPage::reload()
{
	ui->reloadButton->setVisible(false);

	/* Drop what we think we know, then go through the same path as
	 * opening the tab, so there is only one way the page gets built. */
	m_loaded = false;
	m_versions.clear();

	ui->updateButton->setText(tr("Fetching versions..."));
	ui->updateButton->setDisabled(true);

	openedImpl();
}

void ManagedPackPage::fetchVersions()
{
	if (m_loaded) {
		/* Reopening the tab should not re-hit the API. */
		return;
	}
	if (!m_instance->hasManagedPackId()) {
		return;
	}

	if (m_versionsJob && m_versionsJob->isRunning()) {
		m_versionsJob->abort();
	}

	const QString packId = m_instance->managedPackId();

	QUrl url;
	switch (m_provider) {
		case Provider::Modrinth: {
			/* No version or loader filter: the page lists everything the
			 * pack has, and a modpack's own loader is not something the
			 * user is filtering by here. */
			ModPlatform::VersionQuery query;
			query.projectId = packId;
			url = ModrinthApi::get().projectVersionsUrl(query);
			break;
		}
		case Provider::CurseForge: {
			/* Goes through projectVersionsUrl rather than
			 * allProjectFilesUrl because only the former asks for the
			 * whole list. CurseForge pages this endpoint at fifty files
			 * by default, and for a long-lived modpack fifty files is an
			 * arbitrary window onto its history - quite possibly one
			 * that excludes both the newest version and the installed
			 * one, which leaves the page unable to offer any change at
			 * all. */
			ModPlatform::VersionQuery query;
			query.projectId = packId;
			url = FlameApi::get().projectVersionsUrl(query);
			break;
		}
		case Provider::Unknown:
			setFailState();
			return;
	}

	/* Held by both handlers: exactly one of them runs, and if the job
	 * dies without either firing the buffer goes with it. */
	auto response = std::make_shared<QByteArray>();
	const quint64 generation = ++m_generation;

	m_versionsJob.reset(new NetJob(
		QString("ManagedPack::Versions(%1)").arg(packId),
		APPLICATION->network()));
	m_versionsJob->addNetAction(
		Net::Download::makeByteArray(url, response.get()));

	connect(m_versionsJob.get(), &NetJob::succeeded, this,
			[this, response, generation] {
				applyVersions(generation, *response);
			});
	/* An abort arrives here too: Task::emitAborted() reports itself as a
	 * failure with the reason "Aborted.", so there is no separate signal
	 * to hook. Either way the page has no versions and must offer a
	 * retry. */
	connect(m_versionsJob.get(), &NetJob::failed, this,
			[this, response, generation](QString) {
				if (generation != m_generation) {
					return;
				}
				setFailState();
			});

	ui->changelogTextBrowser->setText(tr("Fetching changelogs..."));

	m_versionsJob->start();
}

void ManagedPackPage::applyVersions(quint64 generation,
									const QByteArray& bytes)
{
	if (generation != m_generation) {
		/* A newer request has already been issued. */
		return;
	}

	bool parsed = false;
	switch (m_provider) {
		case Provider::Modrinth:
			m_versions = ManagedPack::parseModrinthVersions(bytes, &parsed);
			break;
		case Provider::CurseForge:
			m_versions = ManagedPack::parseCurseForgeFiles(bytes, &parsed);
			break;
		case Provider::Unknown:
			break;
	}

	/* Either way there is nothing to offer an update to, and either way
	 * a retry is the only useful action - a pack that reports no
	 * versions is far more often a transient API answer than a pack
	 * genuinely without releases - so both end in the same state. */
	if (!parsed || m_versions.isEmpty()) {
		setFailState();
		return;
	}

	if (m_provider == Provider::CurseForge) {
		/* CurseForge returns a null downloadUrl for any file whose
		 * project has third-party distribution switched off, which for
		 * modpacks is the common case rather than the exception. Taking
		 * that at face value would leave the button reading "Cannot
		 * update!" for most CurseForge packs - which is what it did.
		 *
		 * The site's own download route serves those files, and it is
		 * already what this launcher falls back to when *installing* such
		 * a pack (see FlamePage::suggestCurrent). Changing versions has
		 * to agree with installing, so the same fallback applies here. */
		const QString packId = m_instance->managedPackId();
		for (ManagedPack::Version& version : m_versions) {
			if (version.downloadUrl.isEmpty()) {
				version.downloadUrl =
					FlameApi::browserDownloadUrl(packId, version.versionId);
			}
		}
	}

	/* Signals off while filling: currentIndexChanged would otherwise
	 * fire against a half-populated list. */
	ui->versionsComboBox->blockSignals(true);
	ui->versionsComboBox->clear();

	const QString installedId = m_instance->managedPackVersionId();
	const QString installedName = m_instance->managedPackVersionName();

	for (const ManagedPack::Version& version : m_versions) {
		QString label = version.label();

		/* Mark what is on disk. Match on id first - that is the
		 * authoritative key - and fall back to the human version,
		 * because some older imports recorded only that, and because
		 * Modrinth's pack index has been known to carry a version id
		 * that disagrees with the catalogue while the version string
		 * still matches. */
		const bool isInstalled =
			(!installedId.isEmpty() && version.versionId == installedId) ||
			(installedId.isEmpty() && !installedName.isEmpty() &&
			 (version.versionNumber == installedName ||
			  version.displayName == installedName));

		if (isInstalled) {
			label = tr("%1 (Current)").arg(label);
		}

		ui->versionsComboBox->addItem(label, version.versionId);
	}

	ui->versionsComboBox->blockSignals(false);

	m_loaded = true;

	/* Selection is on entry 0 - the newest version - and nothing has
	 * told the rest of the page about it yet. */
	suggestVersion();
}

const ManagedPack::Version* ManagedPackPage::selectedVersion() const
{
	const int index = ui->versionsComboBox->currentIndex();
	if (index < 0 || index >= m_versions.size()) {
		return nullptr;
	}
	return &m_versions.at(index);
}

void ManagedPackPage::suggestVersion()
{
	const int index = ui->versionsComboBox->currentIndex();
	const ManagedPack::Version* version = selectedVersion();
	if (version == nullptr) {
		setFailState();
		return;
	}

	if (version->changelogLoaded) {
		renderChangelog(version->changelog);
	} else {
		/* CurseForge: one request per file, issued only for the version
		 * the user is actually looking at. */
		ui->changelogTextBrowser->setText(tr("Fetching changelogs..."));
		fetchChangelogFor(index);
	}

	if (version->isInstallable()) {
		ui->updateButton->setText(tr("Update Pack"));
		ui->updateButton->setDisabled(false);
	} else {
		/* CurseForge withholds download URLs for packs whose author
		 * opted out of third-party distribution. The version is real
		 * and worth listing, but we cannot fetch it. */
		ui->updateButton->setText(tr("Cannot update!"));
		ui->updateButton->setDisabled(true);
	}
}

void ManagedPackPage::fetchChangelogFor(int versionIndex)
{
	if (versionIndex < 0 || versionIndex >= m_versions.size()) {
		return;
	}
	if (m_provider != Provider::CurseForge) {
		/* Only CurseForge needs a second request; anything else has
		 * already been told everything it is going to be told. */
		return;
	}

	if (m_changelogJob && m_changelogJob->isRunning()) {
		/* The user moved on before the previous changelog arrived. */
		m_changelogJob->abort();
	}

	const QString packId = m_instance->managedPackId();
	const QString fileId = m_versions.at(versionIndex).versionId;
	const quint64 generation = m_generation;

	auto response = std::make_shared<QByteArray>();

	m_changelogJob.reset(new NetJob(
		QString("ManagedPack::Changelog(%1/%2)").arg(packId, fileId),
		APPLICATION->network()));
	m_changelogJob->addNetAction(Net::Download::makeByteArray(
		FlameApi::fileChangelogUrl(packId, fileId), response.get()));

	connect(m_changelogJob.get(), &NetJob::succeeded, this,
			[this, response, generation, versionIndex] {
				if (generation != m_generation ||
					versionIndex >= m_versions.size()) {
					return;
				}

				/* Cache it, so flicking back and forth between two
				 * versions does not re-request either of them. */
				m_versions[versionIndex].changelog =
					ManagedPack::parseCurseForgeChangelog(*response);
				m_versions[versionIndex].changelogLoaded = true;

				/* Only paint if this is still what is selected. */
				if (ui->versionsComboBox->currentIndex() == versionIndex) {
					renderChangelog(m_versions.at(versionIndex).changelog);
				}
			});

	/* A missing changelog is not worth failing the page over - the
	 * version list is fine and the update button still works, so say so
	 * in the pane and leave everything else alone. Not marked loaded, so
	 * revisiting the version tries again.
	 *
	 * Aborts land here as well; Task reports them as a failure. That is
	 * what we want, because the only thing that aborts this job is the
	 * user selecting a different version, and the guard below means the
	 * message is then dropped anyway. */
	connect(m_changelogJob.get(), &NetJob::failed, this,
			[this, generation, versionIndex](QString) {
				if (generation != m_generation) {
					return;
				}
				if (ui->versionsComboBox->currentIndex() == versionIndex) {
					ui->changelogTextBrowser->setText(tr(
						"Failed to request changelog data for this modpack."));
				}
			});

	m_changelogJob->start();
}

void ManagedPackPage::renderChangelog(const QString& changelog)
{
	if (changelog.isEmpty()) {
		/* Let the browser's own placeholder text show through rather
		 * than inventing a second way of saying the same thing. */
		ui->changelogTextBrowser->clear();
		return;
	}

	/* Modrinth sends markdown, CurseForge sends HTML. cmark leaves
	 * existing HTML alone, so one pass handles both without having to
	 * guess which we were given. */
	HoeDown renderer;
	QString html = renderer.process(changelog.toUtf8());

	html = Strings::htmlListPatch(html);
	html = breakBeforeImagesAfterLists(html);

	/* Images in a changelog are remote; the browser fetches them
	 * through the shared cache and re-lays out when they land. */
	ui->changelogTextBrowser->flush();
	ui->changelogTextBrowser->setHtml(html);
}

void ManagedPackPage::setFailState()
{
	qDebug() << "ManagedPackPage: entering fail state for"
			 << m_instance->id();

	/* Signals off: clearing and refilling would fire
	 * currentIndexChanged into suggestVersion(), which would find no
	 * version and re-enter this function. */
	ui->versionsComboBox->blockSignals(true);
	ui->versionsComboBox->clear();
	ui->versionsComboBox->addItem(
		tr("Failed to search for available versions."), QString());
	ui->versionsComboBox->blockSignals(false);

	ui->changelogTextBrowser->setText(
		tr("Failed to request changelog data for this modpack."));

	ui->updateButton->setText(tr("Cannot update!"));
	ui->updateButton->setDisabled(true);

	ui->reloadButton->setVisible(true);
}

void ManagedPackPage::update()
{
	/* No catalogue entry: the only thing we can act on is whatever the
	 * user typed. Deliberately checked before the version list, so that
	 * an instance in local mode never falls through to it. */
	if (!m_instance->hasManagedPackId()) {
		const QString typed = m_instance->managedPackUpdateUrl();
		if (typed.isEmpty()) {
			CustomMessageBox::selectable(
				this, tr("No URL"),
				tr("Enter the URL of the modpack to update from, or use "
				   "\"Update From File\" instead."),
				QMessageBox::Warning)
				->show();
			return;
		}

		const QUrl url(typed);
		if (!url.isValid() ||
			(url.scheme() != "http" && url.scheme() != "https")) {
			CustomMessageBox::selectable(
				this, tr("Invalid URL"),
				tr("\"%1\" is not a valid http or https URL.").arg(typed),
				QMessageBox::Warning)
				->show();
			return;
		}

		/* An address the user typed: the archive behind it is free to
		 * point its mod downloads anywhere, so the importer must list
		 * what it wants to install before fetching it. */
		updatePack(url, false);
		return;
	}

	const ManagedPack::Version* version = selectedVersion();
	if (version == nullptr) {
		setFailState();
		return;
	}
	if (!version->isInstallable()) {
		/* The button is disabled in this case, so this is only
		 * reachable by keyboard activation racing a selection change. */
		return;
	}

	/* Copied before the task runs: updatePack() re-enters the event loop
	 * through a modal dialog, and m_versions can be replaced underneath
	 * us by a reload in that time. */
	const QUrl downloadUrl(version->downloadUrl);
	const QString versionId = version->versionId;
	const QString versionName = version->versionNumber.isEmpty()
									? version->displayName
									: version->versionNumber;

	/* Straight out of the catalogue's own version list. */
	updatePack(downloadUrl, true, versionId, versionName);
}

void ManagedPackPage::updateFromFile()
{
	QString filter;
	switch (m_provider) {
		case Provider::Modrinth:
			filter = tr("Modrinth pack") + QStringLiteral(" (*.mrpack *.zip)");
			break;
		case Provider::CurseForge:
			filter = tr("CurseForge pack") + QStringLiteral(" (*.zip)");
			break;
		case Provider::Unknown:
			filter = tr("Modpack") + QStringLiteral(" (*.zip *.mrpack)");
			break;
	}

	const QUrl chosen = QFileDialog::getOpenFileUrl(
		this, tr("Choose update file"), QUrl::fromLocalFile(QDir::homePath()),
		filter);
	if (chosen.isEmpty()) {
		return;
	}

	/* No version id or name: an archive on disk says nothing reliable
	 * about which catalogue version it is, and recording a guess would
	 * make the page lie about what is installed. */
	updatePack(chosen, false);
}

void ManagedPackPage::updatePack(const QUrl& url, bool trusted,
								 const QString& versionId,
								 const QString& versionName)
{
	/* Not while the game is running.
	 *
	 * An update replaces the pack's files underneath a live process:
	 * mods are deleted while the JVM holds them open, which on Windows
	 * fails outright and elsewhere leaves the running game executing
	 * code that is no longer on disk. Either way what comes back is not
	 * what the update meant to produce.
	 *
	 * Checked here because this is the one point every route to an
	 * update passes through. */
	if (m_instance->isRunning()) {
		CustomMessageBox::selectable(
			this, tr("Instance is running"),
			tr("This instance is currently running. Close the game before "
			   "updating the modpack - an update replaces the files the "
			   "game has open."),
			QMessageBox::Warning)
			->show();
		return;
	}

	auto* task = new InstanceImportTask(url);

	/* Replace this instance rather than create a second one. */
	InstanceImportTask::UpdateTarget target;
	target.instanceId = m_instance->id();
	target.versionId = versionId;
	target.versionLabel = versionName;
	task->setUpdateTarget(target);
	task->setDialogParent(this);
	task->setTrustedSource(trusted);

	/* Carry the pack's identity across explicitly.
	 *
	 * The update stages a brand new instance.cfg, so anything not
	 * written into it is gone once it replaces the live one. What the
	 * importer can recover from the archive is only the pack's *name* -
	 * ids and slugs are not in either manifest format - so without this
	 * the instance would come back from a successful update no longer
	 * recognised as a managed pack, and this page would vanish with it. */
	InstanceImportTask::PackSourceHint hint;
	hint.provider = m_instance->managedPackProvider();
	hint.packId = m_instance->managedPackId();
	hint.packSlug = m_instance->managedPackSlug();
	hint.packName = m_instance->managedPackName();
	hint.sourceUrl = m_instance->managedPackSourceUrl();
	hint.versionId = versionId;
	hint.versionLabel = versionName;
	task->setPackSourceHint(hint);

	const QString oldVersionName = m_instance->managedPackVersionName();

	/* Keep the instance where and how the user left it. */
	task->setGroup(
		APPLICATION->instances()->getInstanceGroup(m_instance->id()));
	task->setIcon(m_instance->iconKey());

	/* If the user never renamed the instance away from the pack's own
	 * naming, carry the version forward in the name too. The replace is
	 * a no-op when the old version string does not appear, which is
	 * what keeps a hand-renamed instance from being renamed back. */
	QString newName = m_instance->name();
	if (!versionName.isEmpty() && !oldVersionName.isEmpty()) {
		newName.replace(oldVersionName, versionName);
		if (newName != m_instance->name()) {
			/* Asked rather than done. The name is the user's, even when
			 * it started out as the pack's: they may have kept the old
			 * version in it on purpose, and an update is not the moment
			 * to decide that for them. Only reachable when the old
			 * version string actually appears in the name, so this is
			 * never a question about a name we were not going to touch
			 * anyway. */
			auto* box = CustomMessageBox::selectable(
				this, tr("Change instance name"),
				tr("The instance's name includes the version being "
				   "replaced. Would you like to update it?\n\n"
				   "Old name: %1\n"
				   "New name: %2")
					.arg(m_instance->name(), newName),
				QMessageBox::Question, QMessageBox::Yes | QMessageBox::No,
				QMessageBox::Yes);
			if (box->exec() != QMessageBox::Yes) {
				newName = m_instance->name();
			}
		}
	}
	task->setName(newName);

	const bool succeeded = runUpdateTask(task);
	onUpdateFinished(succeeded,
					 versionName.isEmpty() ? oldVersionName : versionName);
}

bool ManagedPackPage::runUpdateTask(InstanceTask* task)
{
	Q_ASSERT(task);

	/* wrapInstanceTask takes ownership of `task` and gives back the
	 * staging wrapper that actually commits it. */
	unique_qobject_ptr<Task> wrapped(
		APPLICATION->instances()->wrapInstanceTask(task));

	/* Raw pointer for the lambdas: they only run while `wrapped` is
	 * alive on this stack frame, and capturing the owning pointer by
	 * reference would tie their validity to a local going out of
	 * scope. */
	Task* wrappedTask = wrapped.get();

	connect(wrappedTask, &Task::failed, this, [this](const QString& reason) {
		CustomMessageBox::selectable(this, tr("Error"), reason,
									 QMessageBox::Critical)
			->show();
	});
	connect(wrappedTask, &Task::succeeded, this, [this, wrappedTask] {
		const QStringList warnings = wrappedTask->warnings();
		if (!warnings.isEmpty()) {
			CustomMessageBox::selectable(this, tr("Warnings"),
										 warnings.join(QLatin1Char('\n')),
										 QMessageBox::Warning)
				->show();
		}
	});

	ProgressDialog dialog(this);
	dialog.setSkipButton(true, tr("Abort"));
	dialog.execWithTask(wrappedTask);

	return wrappedTask->wasSuccessful();
}

void ManagedPackPage::onUpdateFinished(bool succeeded,
									   const QString& versionName)
{
	/* Parented to nothing on purpose: a successful update closes the
	 * instance window this page lives in, and a message box parented to
	 * a widget being torn down goes with it unseen.
	 */
	if (!succeeded) {
		CustomMessageBox::selectable(
			nullptr, tr("Update Failed"),
			tr("The instance failed to update to pack version %1. Please "
			   "check launcher logs for more information.")
				.arg(versionName),
			QMessageBox::Critical)
			->show();
		return;
	}

	if (m_instanceWindow != nullptr) {
		/* Everything this window is showing has just been replaced on
		 * disk. */
		m_instanceWindow->close();
	} else {
		/* Opened from the settings dialog, so there is no window to
		 * close and this page is about to be looked at again. Rebuild it
		 * from the instance, which the commit step has just re-read from
		 * disk - otherwise it would go on reporting the version we
		 * replaced, with "(Current)" pointing at the wrong entry.
		 *
		 * The version list itself is dropped rather than reused: the
		 * pack may have been updated by someone else in the meantime,
		 * and this is the one moment we know the page is wrong. */
		m_loaded = false;
		m_versions.clear();
		ui->reloadButton->setVisible(false);
		ui->updateButton->setText(tr("Fetching versions..."));
		ui->updateButton->setDisabled(true);
		openedImpl();
	}

	CustomMessageBox::selectable(
		nullptr, tr("Update Successful"),
		tr("The instance updated to pack version %1 successfully.")
			.arg(versionName),
		QMessageBox::Information)
		->show();
}
