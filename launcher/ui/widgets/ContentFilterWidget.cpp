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

#include "ContentFilterWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <algorithm>

#include "Application.h"
#include "meta/Index.h"
#include "meta/Version.h"
#include "meta/VersionList.h"
#include "tasks/Task.h"
#include "ui/widgets/CheckComboBox.h"

ContentFilterWidget::ContentFilterWidget(QWidget* parent) : QWidget(parent)
{
	auto* outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 6, 0);
	setLayout(outer);

	/* Scrolled, because the category list is as long as the provider
	 * cares to make it - CurseForge alone offers dozens for mods - and
	 * the panel shares its height with the results list. */
	auto* scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	outer->addWidget(scroll);

	auto* contents = new QWidget(scroll);
	m_contentLayout = new QVBoxLayout(contents);
	m_contentLayout->setContentsMargins(0, 0, 0, 0);
	contents->setLayout(m_contentLayout);
	scroll->setWidget(contents);
}

QCheckBox* ContentFilterWidget::addChannelBox(QGroupBox* group,
											  const QString& label,
											  const QString& channel)
{
	auto* box = new QCheckBox(label, group);
	box->setChecked(true);
	group->layout()->addWidget(box);
	m_channelBoxes.append({box, channel});
	connect(box, &QCheckBox::toggled, this, [this] {
		rebuildFilter();
		emit viewFilterChanged();
	});
	return box;
}

void ContentFilterWidget::buildVersionGroup(bool extended)
{
	auto* group = new QGroupBox(tr("Versions"), this);
	auto* groupLayout = new QVBoxLayout(group);
	group->setLayout(groupLayout);

	/* Above the box, as in the reference launcher - it decides what the
	 * box below has to offer, so reading it first makes sense. */
	m_showAllVersionsBox = new QCheckBox(tr("Show all versions"), group);
	m_showAllVersionsBox->setToolTip(
		tr("Also offer snapshots and other pre-release versions."));
	groupLayout->addWidget(m_showAllVersionsBox);
	connect(m_showAllVersionsBox, &QCheckBox::toggled, this,
			&ContentFilterWidget::applyMcVersions);

	if (extended) {
		m_mcVersionsBox = new CheckComboBox(group);
		m_mcVersionsBox->setDefaultText(tr("All Versions"));
		m_mcVersionsBox->setSeparator(QStringLiteral(", "));
		m_mcVersionsBox->setToolTip(
			tr("Tick every Minecraft version the search should cover. "
			   "With none ticked, every version is searched."));
		groupLayout->addWidget(m_mcVersionsBox);

		connect(m_mcVersionsBox, &CheckComboBox::checkedItemsChanged, this,
				[this] {
					const QStringList before = m_filter.mcVersions;
					rebuildFilter();
					/* Refilling the box during setup() reports a change
					 * too, and there is nothing to search yet then. */
					if (m_active && m_filter.mcVersions != before) {
						emit searchFilterChanged();
					}
				});
	} else {
		/* CurseForge's search takes a single game version, so a box that
		 * let several be ticked would quietly drop all but the first.
		 * The reference launcher draws the same line. */
		m_mcVersionBox = new QComboBox(group);
		m_mcVersionBox->setToolTip(
			tr("Which Minecraft version to search for. \"All Versions\" "
			   "is how a project that has not been updated yet is found."));
		groupLayout->addWidget(m_mcVersionBox);

		connect(m_mcVersionBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
			const QStringList before = m_filter.mcVersions;
			rebuildFilter();
			if (m_active && m_filter.mcVersions != before) {
				emit searchFilterChanged();
			}
		});
	}

	m_contentLayout->addWidget(group);

	loadMcVersions();
}

void ContentFilterWidget::loadMcVersions()
{
	/* The instance's own version is offered whatever happens, so a
	 * missing or slow list costs the user nothing they had before. */
	auto index = APPLICATION->metadataIndex();
	if (!index) {
		applyMcVersions();
		return;
	}

	m_mcVersionList = index->get(QStringLiteral("net.minecraft"));
	if (!m_mcVersionList) {
		applyMcVersions();
		return;
	}

	/* getLoadTask() also kicks the download off, and the entity holds
	 * the task, so there is nothing to keep alive here.
	 *
	 * Deliberately not the reference launcher's approach: it spins a
	 * nested event loop and waits up to four seconds for this list,
	 * freezing the window on a slow connection for a panel that is
	 * hidden until asked for. Filling the box when the answer arrives
	 * costs nothing and never blocks. */
	auto task = m_mcVersionList->getLoadTask();
	if (task) {
		connect(task.get(), &Task::succeeded, this,
				&ContentFilterWidget::applyMcVersions);
	}

	/* A list already on disk is usable right away; the request above
	 * only refreshes it. */
	applyMcVersions();
}

void ContentFilterWidget::applyMcVersions()
{
	QStringList versions;

	if (m_mcVersionList) {
		const bool showAll = m_showAllVersionsBox != nullptr &&
							 m_showAllVersionsBox->isChecked();

		auto entries = m_mcVersionList->versions();
		/* Newest first, by release date rather than by name: version
		 * strings do not sort usefully once snapshots are in the list. */
		std::sort(entries.begin(), entries.end(),
				  [](const Meta::VersionPtr& a, const Meta::VersionPtr& b) {
					  return a->rawTime() > b->rawTime();
				  });

		for (const auto& entry : entries) {
			if (!entry || entry->version().isEmpty()) {
				continue;
			}
			if (!showAll && entry->type() != QLatin1String("release")) {
				continue;
			}
			versions.append(entry->version());
		}
	}

	/* Offered even when the list has not arrived, and even when
	 * "releases only" would have filtered it out - a snapshot instance
	 * still has to be able to search for its own version. */
	if (!m_instanceMcVersion.isEmpty() &&
		!versions.contains(m_instanceMcVersion)) {
		versions.prepend(m_instanceMcVersion);
	}

	if (m_mcVersionsBox != nullptr) {
		const bool firstFill = m_mcVersionsBox->count() == 0;
		/* Ticks on versions that are still offered survive the refill. */
		m_mcVersionsBox->setItems(versions);
		if (firstFill && !m_instanceMcVersion.isEmpty()) {
			m_mcVersionsBox->setCheckedItems(
				ModPlatform::singleVersionList(m_instanceMcVersion));
		}
	} else if (m_mcVersionBox != nullptr) {
		const QString wanted = m_mcVersionBox->count() == 0
								   ? m_instanceMcVersion
								   : m_mcVersionBox->currentData().toString();

		/* Rebuilt in one go, so the intermediate states do not each
		 * count as the user changing the filter. */
		const bool blocked = m_mcVersionBox->blockSignals(true);
		m_mcVersionBox->clear();
		/* An empty value stands for "no version filter". */
		m_mcVersionBox->addItem(tr("All Versions"), QString());
		for (const QString& version : versions) {
			m_mcVersionBox->addItem(version, version);
		}
		const int row =
			wanted.isEmpty() ? 0 : m_mcVersionBox->findData(wanted);
		m_mcVersionBox->setCurrentIndex(row < 0 ? 0 : row);
		m_mcVersionBox->blockSignals(blocked);
	}

	const QStringList before = m_filter.mcVersions;
	rebuildFilter();
	/* Toggling "show all versions" usually only changes what is on
	 * offer, not what is picked; asking the provider again for that
	 * would be a round trip for nothing. */
	if (m_active && m_filter.mcVersions != before) {
		emit searchFilterChanged();
	}
}

void ContentFilterWidget::setup(ModPlatform::ContentType contentType,
								const QString& mcVersion,
								const QString& instanceLoader, bool extended)
{
	QVBoxLayout* layout = m_contentLayout;

	/* Group order and wording follow the reference launcher: Categories,
	 * Loaders, Versions, Environments, Hide installed, Open source,
	 * Release type. */

	/* Empty for now - the provider is asked for its list separately, and
	 * setCategories() fills this in when the answer arrives. */
	m_categoryGroup = new QGroupBox(tr("Categories"), this);
	m_categoryGroup->setLayout(new QVBoxLayout(m_categoryGroup));
	m_categoryGroup->hide();
	layout->addWidget(m_categoryGroup);

	/* Only mods are loader-specific; a resource pack works everywhere. */
	if (ModPlatform::contentTypeUsesLoader(contentType)) {
		auto* group = new QGroupBox(tr("Loaders"), this);
		auto* groupLayout = new QVBoxLayout(group);
		group->setLayout(groupLayout);

		const QList<QPair<QString, QString>> common = {
			{tr("NeoForge"), QStringLiteral("neoforge")},
			{tr("Forge"), QStringLiteral("forge")},
			{tr("Fabric"), QStringLiteral("fabric")},
			{tr("Quilt"), QStringLiteral("quilt")},
		};
		/* Behind "Show More", as in the reference launcher. CurseForge
		 * has no id for any of these, so ticking one narrows the search
		 * on Modrinth and is dropped on CurseForge. */
		const QList<QPair<QString, QString>> uncommon = {
			{tr("LiteLoader"), QStringLiteral("liteloader")},
			{tr("Babric"), QStringLiteral("babric")},
			{tr("BTA (Babric)"), QStringLiteral("bta-babric")},
			{tr("Legacy Fabric"), QStringLiteral("legacy-fabric")},
			{tr("Ornithe"), QStringLiteral("ornithe")},
			{tr("Rift"), QStringLiteral("rift")},
		};

		auto addLoaderBox = [this](QWidget* parent, QVBoxLayout* into,
								   const QPair<QString, QString>& loader,
								   const QString& instanceLoader) {
			auto* box = new QCheckBox(loader.first, parent);
			/* Start on what the instance actually runs; anything else
			 * would list mods that cannot be installed. */
			box->setChecked(loader.second == instanceLoader);
			into->addWidget(box);
			m_loaderBoxes.append({box, loader.second});

			connect(box, &QCheckBox::toggled, this, [this] {
				rebuildFilter();
				emit searchFilterChanged();
			});
		};

		for (const auto& loader : common) {
			addLoaderBox(group, groupLayout, loader, instanceLoader);
		}

		auto* extra = new QWidget(group);
		auto* extraLayout = new QVBoxLayout(extra);
		extraLayout->setContentsMargins(0, 0, 0, 0);
		extra->setLayout(extraLayout);
		for (const auto& loader : uncommon) {
			addLoaderBox(extra, extraLayout, loader, instanceLoader);
		}

		auto* showMore = new QPushButton(tr("Show More"), group);
		groupLayout->addWidget(showMore);
		groupLayout->addWidget(extra);

		/* Revealed once and for good, so the button goes away with it
		 * rather than turning into a toggle. */
		extra->setVisible(false);
		connect(showMore, &QPushButton::clicked, this, [extra, showMore] {
			extra->setVisible(true);
			showMore->setVisible(false);
		});

		layout->addWidget(group);
	}

	m_instanceMcVersion = mcVersion;
	buildVersionGroup(extended);

	/* Only Modrinth records which side a project runs on, so the group
	 * would be an inert control anywhere else. */
	if (extended && ModPlatform::contentTypeUsesLoader(contentType)) {
		auto* group = new QGroupBox(tr("Environments"), this);
		auto* groupLayout = new QVBoxLayout(group);
		group->setLayout(groupLayout);

		m_clientSideBox = new QCheckBox(tr("Client"), group);
		m_serverSideBox = new QCheckBox(tr("Server"), group);
		groupLayout->addWidget(m_clientSideBox);
		groupLayout->addWidget(m_serverSideBox);

		for (auto* box : {m_clientSideBox, m_serverSideBox}) {
			connect(box, &QCheckBox::toggled, this, [this] {
				rebuildFilter();
				emit searchFilterChanged();
			});
		}

		layout->addWidget(group);
	}

	m_hideInstalledBox = new QCheckBox(tr("Hide installed items"), this);
	m_hideInstalledBox->setToolTip(
		tr("Leave out results that are already in this instance."));
	connect(m_hideInstalledBox, &QCheckBox::toggled, this, [this] {
		rebuildFilter();
		emit viewFilterChanged();
	});
	layout->addWidget(m_hideInstalledBox);

	if (extended) {
		m_openSourceBox = new QCheckBox(tr("Open source only"), this);
		m_openSourceBox->setToolTip(
			tr("Only show projects published under a licence the provider "
			   "recognises as open source."));
		connect(m_openSourceBox, &QCheckBox::toggled, this, [this] {
			rebuildFilter();
			emit searchFilterChanged();
		});
		layout->addWidget(m_openSourceBox);
	}

	{
		auto* group = new QGroupBox(tr("Release type"), this);
		auto* groupLayout = new QVBoxLayout(group);
		group->setLayout(groupLayout);

		addChannelBox(group, tr("Release"), QStringLiteral("release"));
		addChannelBox(group, tr("Beta"), QStringLiteral("beta"));
		addChannelBox(group, tr("Alpha"), QStringLiteral("alpha"));
		/* The empty channel stands for a version whose type the provider
		 * did not state. CurseForge leaves it out often enough that
		 * unticking this hides a good part of its catalogue, which is
		 * exactly what the box is for. */
		addChannelBox(group, tr("Unknown"), QString());

		layout->addWidget(group);
	}

	layout->addStretch();

	m_active = true;
	rebuildFilter();
}

void ContentFilterWidget::setCategories(
	const QList<ModPlatform::Category>& categories)
{
	if (m_categoryGroup == nullptr || categories.isEmpty()) {
		return;
	}

	auto* groupLayout = qobject_cast<QVBoxLayout*>(m_categoryGroup->layout());
	if (groupLayout == nullptr) {
		return;
	}

	for (const auto& category : categories) {
		QString label = category.name;
		/* Providers hand these out slugged, and an ampersand would be
		 * eaten as a keyboard accelerator. */
		label.replace(QLatin1Char('-'), QLatin1Char(' '));
		label.replace(QLatin1String("&"), QLatin1String("&&"));

		auto* box = new QCheckBox(label, m_categoryGroup);
		QFont font = box->font();
		font.setCapitalization(QFont::Capitalize);
		box->setFont(font);
		groupLayout->addWidget(box);
		m_categoryBoxes.append({box, category.id});

		connect(box, &QCheckBox::toggled, this, [this] {
			rebuildFilter();
			emit searchFilterChanged();
		});
	}

	m_categoryGroup->show();
}

bool ContentFilterWidget::allowsVersionChannel(const QString& channel) const
{
	return !m_active || m_filter.versionChannels.contains(channel);
}

void ContentFilterWidget::rebuildFilter()
{
	m_filter.mcVersions.clear();
	if (m_mcVersionsBox != nullptr) {
		m_filter.mcVersions = m_mcVersionsBox->checkedItems();
	} else if (m_mcVersionBox != nullptr) {
		/* Empty data is the "All Versions" entry, which means no version
		 * filter at all. */
		const QString version = m_mcVersionBox->currentData().toString();
		m_filter.mcVersions = ModPlatform::singleVersionList(version);
	}

	m_filter.loaders.clear();
	for (const auto& entry : m_loaderBoxes) {
		if (entry.first->isChecked()) {
			m_filter.loaders.append(entry.second);
		}
	}

	const bool client =
		m_clientSideBox != nullptr && m_clientSideBox->isChecked();
	const bool server =
		m_serverSideBox != nullptr && m_serverSideBox->isChecked();
	if (client && server) {
		/* Both ticked is the strict reading - has to work on each side -
		 * rather than "either", which is what no filter already gives. */
		m_filter.side = ModPlatform::SideFilter::Universal;
	} else if (client) {
		m_filter.side = ModPlatform::SideFilter::Client;
	} else if (server) {
		m_filter.side = ModPlatform::SideFilter::Server;
	} else {
		m_filter.side = ModPlatform::SideFilter::Any;
	}

	m_filter.openSourceOnly =
		m_openSourceBox != nullptr && m_openSourceBox->isChecked();

	m_filter.categoryIds.clear();
	for (const auto& entry : m_categoryBoxes) {
		if (entry.first->isChecked()) {
			m_filter.categoryIds.append(entry.second);
		}
	}

	m_filter.versionChannels.clear();
	for (const auto& entry : m_channelBoxes) {
		if (entry.first->isChecked()) {
			m_filter.versionChannels.insert(entry.second);
		}
	}

	m_filter.hideInstalled =
		m_hideInstalledBox != nullptr && m_hideInstalledBox->isChecked();
}

ModPlatform::SearchFilters ContentFilterWidget::searchFilters() const
{
	ModPlatform::SearchFilters filters;
	/* An empty list means "any version", which is what the query
	 * expresses by leaving the version out. */
	filters.mcVersions = m_filter.mcVersions;
	filters.loaders = m_filter.loaders;
	filters.side = m_filter.side;
	filters.openSourceOnly = m_filter.openSourceOnly;
	filters.categoryIds = m_filter.categoryIds;
	return filters;
}
