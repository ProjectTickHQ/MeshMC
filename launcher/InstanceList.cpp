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

#include <QDir>
#include <QDirIterator>
#include <QSet>
#include <QFile>
#include <QFileInfo>
#include <QThread>
#include <QTextStream>
#include <QXmlStreamReader>
#include <QTimer>
#include <QDebug>
#include <QFileSystemWatcher>
#include <QUuid>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMimeData>
#include <algorithm>

#include "InstanceList.h"
#include "BaseInstance.h"
#include "InstanceTask.h"
#include "settings/INISettingsObject.h"
#include "minecraft/legacy/LegacyInstance.h"
#include "NullInstance.h"
#include "minecraft/MinecraftInstance.h"
#include "FileSystem.h"
#include "ExponentialSeries.h"
#include "WatchLock.h"

const static int GROUP_FILE_FORMAT_VERSION = 1;

InstanceList::InstanceList(SettingsObjectPtr settings,
						   const QStringList& instDirs, QObject* parent)
	: QAbstractListModel(parent), m_globalSettings(settings)
{
	resumeWatch();

	connect(this, &InstanceList::instancesChanged, this,
			&InstanceList::providerUpdated);

	m_watcher = new QFileSystemWatcher(this);
	connect(m_watcher, &QFileSystemWatcher::directoryChanged, this,
			&InstanceList::instanceDirContentsChanged);

	applyInstanceDirs(resolveInstanceDirs(instDirs));
}

QStringList InstanceList::resolveInstanceDirs(const QStringList& dirs) const
{
	QStringList resolved;
	for (const QString& dir : dirs) {
		if (dir.isEmpty()) {
			continue;
		}
		/* Create before canonicalising: canonicalPath() answers about the
		 * filesystem and returns nothing for a path that is not there, so
		 * the order here is load-bearing rather than stylistic. */
		if (!QDir::current().exists(dir) && !QDir::current().mkpath(dir)) {
			qWarning() << "Ignoring instance folder" << dir
					   << "- it does not exist and could not be created";
			continue;
		}
		const QString canonical = QDir(dir).canonicalPath();
		if (canonical.isEmpty()) {
			qWarning() << "Ignoring instance folder" << dir
					   << "- could not be resolved";
			continue;
		}
		/* Two settings pointing at one folder, or a path spelled two ways,
		 * would otherwise make every instance in it a duplicate of itself
		 * and get skipped by discovery. */
		if (resolved.contains(canonical)) {
			qDebug() << "Ignoring duplicate instance folder" << dir;
			continue;
		}
		resolved << canonical;
	}
	return resolved;
}

void InstanceList::applyInstanceDirs(const QStringList& resolved)
{
	for (const QString& dir : m_instDirs) {
		m_watcher->removePath(dir);
	}
	m_instDirs = resolved;
	for (const QString& dir : m_instDirs) {
		m_watcher->addPath(dir);
	}
	qDebug() << "Instance folders:" << m_instDirs;
}

QString InstanceList::rootDirOf(const InstanceId& id) const
{
	return m_instanceRootDirMap.value(id, primaryDir());
}

QStringList InstanceList::decodeInstanceDirList(const QVariant& value)
{
	/* A genuine QStringList is accepted as well as the encoded form.
	 *
	 * Nothing writes the setting that way, but a value set and read back
	 * within one session never passes through the INI file at all, and a
	 * config edited by hand can be either. Dropping the user's folders
	 * over that distinction would be a poor trade for strictness.
	 */
	/* userType() rather than Qt 6's metaType()/typeId(): it exists in both
	 * Qt 5 and Qt 6 (where it simply forwards to typeId()). */
	if (value.userType() == QMetaType::QStringList) {
		return value.toStringList();
	}

	const QString encoded = value.toString().trimmed();
	if (encoded.isEmpty()) {
		return {};
	}

	QJsonParseError error;
	const QJsonDocument doc = QJsonDocument::fromJson(encoded.toUtf8(), &error);
	if (error.error != QJsonParseError::NoError || !doc.isArray()) {
		qWarning() << "Could not read the additional instance folders setting"
				   << "-" << error.errorString()
				   << "- ignoring it rather than guessing";
		return {};
	}

	QStringList out;
	const QJsonArray array = doc.array();
	for (const QJsonValue& entry : array) {
		const QString dir = entry.toString();
		if (!dir.isEmpty()) {
			out << dir;
		}
	}
	return out;
}

QVariant InstanceList::encodeInstanceDirList(const QStringList& dirs)
{
	QJsonArray array;
	for (const QString& dir : dirs) {
		if (!dir.isEmpty()) {
			array.append(dir);
		}
	}
	return QString::fromUtf8(
		QJsonDocument(array).toJson(QJsonDocument::Compact));
}

QString InstanceList::rootForStaging(const QString& stagingPath) const
{
	if (stagingPath.isEmpty()) {
		return QString();
	}
	const QString absolute = QFileInfo(stagingPath).absoluteFilePath();
	for (const QString& dir : m_instDirs) {
		/* The separator is not decoration: without it "/data/instances"
		 * would claim a staging path under "/data/instances-backup". */
		if (absolute.startsWith(dir + QLatin1Char('/'))) {
			return dir;
		}
	}
	return QString();
}

InstanceList::~InstanceList() {}

Qt::DropActions InstanceList::supportedDragActions() const
{
	return Qt::MoveAction;
}

Qt::DropActions InstanceList::supportedDropActions() const
{
	return Qt::MoveAction;
}

bool InstanceList::canDropMimeData(const QMimeData* data, Qt::DropAction, int,
								   int, const QModelIndex&) const
{
	if (data && data->hasFormat("application/x-instanceid")) {
		return true;
	}
	return false;
}

bool InstanceList::dropMimeData(const QMimeData* data, Qt::DropAction, int, int,
								const QModelIndex&)
{
	if (data && data->hasFormat("application/x-instanceid")) {
		return true;
	}
	return false;
}

QStringList InstanceList::mimeTypes() const
{
	auto types = QAbstractListModel::mimeTypes();
	types.push_back("application/x-instanceid");
	return types;
}

QMimeData* InstanceList::mimeData(const QModelIndexList& indexes) const
{
	auto mimeData = QAbstractListModel::mimeData(indexes);
	if (indexes.size() == 1) {
		auto instanceId = data(indexes[0], InstanceIDRole).toString();
		mimeData->setData("application/x-instanceid", instanceId.toUtf8());
	}
	return mimeData;
}

int InstanceList::rowCount(const QModelIndex& parent) const
{
	Q_UNUSED(parent);
	return m_instances.count();
}

QModelIndex InstanceList::index(int row, int column,
								const QModelIndex& parent) const
{
	Q_UNUSED(parent);
	if (row < 0 || row >= m_instances.size())
		return QModelIndex();
	return createIndex(row, column, (void*)m_instances.at(row).get());
}

QVariant InstanceList::data(const QModelIndex& index, int role) const
{
	if (!index.isValid()) {
		return QVariant();
	}
	BaseInstance* pdata = static_cast<BaseInstance*>(index.internalPointer());
	switch (role) {
		case InstancePointerRole: {
			QVariant v = QVariant::fromValue((void*)pdata);
			return v;
		}
		case InstanceIDRole: {
			return pdata->id();
		}
		case Qt::EditRole:
		case Qt::DisplayRole: {
			return pdata->name();
		}
		case Qt::AccessibleTextRole: {
			return tr("%1 Instance").arg(pdata->name());
		}
		case Qt::ToolTipRole: {
			return pdata->instanceRoot();
		}
		case Qt::DecorationRole: {
			return pdata->iconKey();
		}
		// HACK: see InstanceView.h in gui!
		case GroupRole: {
			return getInstanceGroup(pdata->id());
		}
		default:
			break;
	}
	return QVariant();
}

bool InstanceList::setData(const QModelIndex& index, const QVariant& value,
						   int role)
{
	if (!index.isValid()) {
		return false;
	}
	if (role != Qt::EditRole) {
		return false;
	}
	BaseInstance* pdata = static_cast<BaseInstance*>(index.internalPointer());
	auto newName = value.toString();
	if (pdata->name() == newName) {
		return true;
	}
	pdata->setName(newName);
	return true;
}

Qt::ItemFlags InstanceList::flags(const QModelIndex& index) const
{
	Qt::ItemFlags f;
	if (index.isValid()) {
		f |= (Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable);
	}
	return f;
}

GroupId InstanceList::getInstanceGroup(const InstanceId& id) const
{
	auto inst = getInstanceById(id);
	if (!inst) {
		return GroupId();
	}
	auto iter = m_instanceGroupIndex.find(inst->id());
	if (iter != m_instanceGroupIndex.end()) {
		return *iter;
	}
	return GroupId();
}

void InstanceList::setInstanceGroup(const InstanceId& id, const GroupId& name)
{
	auto inst = getInstanceById(id);
	if (!inst) {
		qDebug() << "Attempt to set a null instance's group";
		return;
	}

	bool changed = false;
	auto iter = m_instanceGroupIndex.find(inst->id());
	if (iter != m_instanceGroupIndex.end()) {
		if (*iter != name) {
			*iter = name;
			changed = true;
		}
	} else {
		changed = true;
		m_instanceGroupIndex[id] = name;
	}

	if (changed) {
		m_groupNameCache.insert(name);
		auto idx = getInstIndex(inst.get());
		emit dataChanged(index(idx), index(idx), {GroupRole});
		saveGroupList();
	}
}

QStringList InstanceList::getGroups()
{
	return m_groupNameCache.values();
}

void InstanceList::deleteGroup(const QString& name)
{
	bool removed = false;
	qDebug() << "Delete group" << name;
	for (auto& instance : m_instances) {
		const auto& instID = instance->id();
		auto instGroupName = getInstanceGroup(instID);
		if (instGroupName == name) {
			m_instanceGroupIndex.remove(instID);
			qDebug() << "Remove" << instID << "from group" << name;
			removed = true;
			auto idx = getInstIndex(instance.get());
			if (idx > 0) {
				emit dataChanged(index(idx), index(idx), {GroupRole});
			}
		}
	}
	if (removed) {
		saveGroupList();
	}
}

bool InstanceList::isGroupCollapsed(const QString& group)
{
	return m_collapsedGroups.contains(group);
}

void InstanceList::deleteInstance(const InstanceId& id)
{
	auto inst = getInstanceById(id);
	if (!inst) {
		qDebug() << "Cannot delete instance" << id
				 << ". No such instance is present (deleted externally?).";
		return;
	}

	if (m_instanceGroupIndex.remove(id)) {
		saveGroupList();
	}

	/* Read before the folder goes: the list lives in the instance's own
	 * config file, and shortcuts() drops entries whose file has since
	 * been moved away, which is a check that needs the disk. */
	const QList<ShortcutData> shortcuts = inst->shortcuts();

	qDebug() << "Will delete instance" << id;
	if (!FS::deletePath(inst->instanceRoot())) {
		qWarning() << "Deletion of instance" << id
				   << "has not been completely successful ...";
		return;
	}

	qDebug() << "Instance" << id << "has been deleted by MeshMC.";

	/* The shortcuts are dead now whatever happens -- they point at a
	 * folder that is gone -- so a failure here is worth a line in the
	 * log and nothing more. */
	for (const ShortcutData& shortcut : shortcuts) {
		if (FS::deletePath(shortcut.filePath)) {
			qDebug() << "Deleted shortcut" << shortcut.name << "at"
					 << shortcut.filePath;
		} else {
			qWarning() << "Could not delete shortcut" << shortcut.name << "at"
					   << shortcut.filePath << "of deleted instance" << id;
		}
	}
}

bool InstanceList::trashInstance(const InstanceId& id)
{
	auto inst = getInstanceById(id);
	if (!inst) {
		qWarning() << "Cannot trash instance" << id
				   << ". No such instance is present (deleted externally?).";
		return false;
	}

	const QString root = inst->instanceRoot();
	const GroupId group = m_instanceGroupIndex.value(id);
	/* Read while the folder is still there: the list lives in the
	 * instance's own config file, and shortcuts() checks the disk. */
	const QList<ShortcutData> shortcuts = inst->shortcuts();

	qDebug() << "Will trash instance" << id;
	QString trashedRoot;
	if (!FS::trash(root, &trashedRoot)) {
		qWarning() << "Could not move instance" << id << "to the trash.";
		return false;
	}
	qDebug() << "Instance" << id << "is in the trash at" << trashedRoot;

	/* Only now: had the move failed, the instance would still be here
	 * and would have lost its group for nothing. */
	if (m_instanceGroupIndex.remove(id)) {
		saveGroupList();
	}

	TrashedInstance record;
	record.id = id;
	record.name = inst->name();
	record.path = root;
	record.trashPath = trashedRoot;
	record.group = group;

	for (const ShortcutData& shortcut : shortcuts) {
		QString trashedShortcut;
		if (FS::trash(shortcut.filePath, &trashedShortcut)) {
			record.shortcuts.append({shortcut, trashedShortcut});
			continue;
		}

		/* A shortcut whose instance is in the trash launches nothing, so
		 * one that refuses to be trashed is deleted instead. Losing the
		 * ability to undo that one beats leaving it on the desktop. */
		qWarning() << "Could not trash shortcut" << shortcut.name << "at"
				   << shortcut.filePath << "-- deleting it instead.";
		if (!FS::deletePath(shortcut.filePath)) {
			qWarning() << "... and could not delete it either.";
		}
	}

	m_trashHistory.append(record);
	return true;
}

bool InstanceList::trashedSomething() const
{
	return !m_trashHistory.isEmpty();
}

QString InstanceList::lastTrashedName() const
{
	if (m_trashHistory.isEmpty()) {
		return {};
	}
	const TrashedInstance& record = m_trashHistory.last();
	return record.name.isEmpty() ? record.id : record.name;
}

bool InstanceList::undoTrashInstance()
{
	if (m_trashHistory.isEmpty()) {
		/* Nothing to do is not a failed restore -- saying otherwise makes
		 * the caller apologise for something that did not happen. */
		qWarning() << "Nothing to restore from the trash.";
		return true;
	}

	const TrashedInstance record = m_trashHistory.takeLast();

	/* An instance's id is its folder name, so if something has appeared
	 * at that path in the meantime it now owns the name. Stepping aside
	 * keeps both instead of refusing to restore. */
	InstanceId id = record.id;
	QString path = record.path;
	while (QFileInfo::exists(path)) {
		id += QLatin1Char('1');
		path += QLatin1Char('1');
	}

	if (!QFile::rename(record.trashPath, path)) {
		qWarning() << "Could not move" << record.trashPath << "back to" << path;
		/* The folder is still in the trash, so put the record back: the
		 * obstacle may be temporary (a file open, the volume busy) and a
		 * second attempt is worth allowing. Only dropped once the move has
		 * actually happened. */
		m_trashHistory.append(record);
		return false;
	}
	qDebug() << "Restored instance" << id << "from" << record.trashPath;

	bool complete = true;
	for (const TrashedShortcut& item : record.shortcuts) {
		/* Not stepped aside the way the folder above is: a shortcut path
		 * ends in a suffix the shell reads (.lnk, .desktop, .app), and
		 * appending past it would produce something inert. */
		if (QFileInfo::exists(item.shortcut.filePath)) {
			qWarning() << "Not restoring shortcut" << item.shortcut.name
					   << "-- something else is at" << item.shortcut.filePath;
			complete = false;
			continue;
		}
		if (!QFile::rename(item.trashPath, item.shortcut.filePath)) {
			qWarning() << "Could not move shortcut" << item.shortcut.name
					   << "back to" << item.shortcut.filePath;
			complete = false;
			continue;
		}
		qDebug() << "Restored shortcut" << item.shortcut.name << "to"
				 << item.shortcut.filePath;
	}

	/* Set the group before the rescan, so the instance comes back in the
	 * group it left rather than at the top level. */
	if (!record.group.isEmpty()) {
		m_instanceGroupIndex[id] = record.group;
		m_groupNameCache.insert(record.group);
	}

	// Synchronous: this lands in providerUpdated() -> loadList().
	emit instancesChanged();

	/* Only worth writing once the rescan has put the id in instanceSet;
	 * saveGroupList() skips instances it does not know about. */
	if (!record.group.isEmpty()) {
		saveGroupList();
	}

	emit instanceSelectRequest(id);
	return complete;
}

static QMap<InstanceId, InstanceLocator>
getIdMapping(const QList<InstancePtr>& list)
{
	QMap<InstanceId, InstanceLocator> out;
	int i = 0;
	for (auto& item : list) {
		auto id = item->id();
		if (out.contains(id)) {
			qWarning() << "Duplicate ID" << id << "in instance list";
		}
		out[id] = std::make_pair(item, i);
		i++;
	}
	return out;
}

QList<InstanceId> InstanceList::discoverInstances()
{
	QList<InstanceId> out;
	m_instanceRootDirMap.clear();

	for (const QString& rootDir : m_instDirs) {
		qDebug() << "Discovering instances in" << rootDir;
		QDirIterator iter(rootDir,
						  QDir::Dirs | QDir::NoDot | QDir::NoDotDot |
							  QDir::Readable | QDir::Hidden,
						  QDirIterator::FollowSymlinks);
		while (iter.hasNext()) {
			QString subDir = iter.next();
			QFileInfo dirInfo(subDir);
			if (!QFileInfo(FS::PathCombine(subDir, "instance.cfg")).exists())
				continue;
			/* A symlink pointing into any configured root is the same
			 * instance reached a second way, not a second instance.
			 * Checking only the root being walked would let a link in one
			 * folder resurrect an instance from another as a phantom
			 * duplicate. */
			if (dirInfo.isSymLink()) {
				const QString targetCanonical =
					QFileInfo(dirInfo.symLinkTarget()).canonicalFilePath();
				const bool pointsIntoAnyRoot = std::any_of(
					m_instDirs.cbegin(), m_instDirs.cend(),
					[&targetCanonical](const QString& otherRoot) {
						return targetCanonical.startsWith(
							QFileInfo(otherRoot).canonicalFilePath());
					});
				if (pointsIntoAnyRoot) {
					qDebug() << "Ignoring symlink" << subDir
							 << "that leads into a configured instance folder";
					continue;
				}
			}
			auto id = dirInfo.fileName();
			/* Ids are directory names, so two roots can each hold one
			 * called "1.20.1". Only one can win, because everything else
			 * - the group index, shortcuts, play time - keys off the id
			 * alone. First root wins, which makes the outcome depend on
			 * the configured order rather than on directory iteration,
			 * and the loser is reported rather than silently missing. */
			if (m_instanceRootDirMap.contains(id)) {
				qWarning() << "Duplicate instance ID" << id << "found in"
						   << rootDir << "- already claimed by"
						   << m_instanceRootDirMap.value(id)
						   << ". Skipping the copy in" << rootDir;
				continue;
			}
			m_instanceRootDirMap[id] = rootDir;
			out.append(id);
			qDebug() << "Found instance ID" << id << "in" << rootDir;
		}
	}
	instanceSet = QSet<QString>(out.begin(), out.end());
	m_instancesProbed = true;
	return out;
}

InstanceList::InstListError InstanceList::loadList()
{
	auto existingIds = getIdMapping(m_instances);

	QList<InstancePtr> newList;

	for (auto& id : discoverInstances()) {
		if (existingIds.contains(id)) {
			auto instPair = existingIds[id];
			existingIds.remove(id);
			qDebug() << "Should keep and soft-reload" << id;
		} else {
			InstancePtr instPtr = loadInstance(id);
			if (instPtr) {
				newList.append(instPtr);
			}
		}
	}

	// TODO: looks like a general algorithm with a few specifics inserted. Do
	// something about it.
	if (!existingIds.isEmpty()) {
		// get the list of removed instances and sort it by their original
		// index, from last to first
		auto deadList = existingIds.values();
		auto orderSortPredicate = [](const InstanceLocator& a,
									 const InstanceLocator& b) -> bool {
			return a.second > b.second;
		};
		std::sort(deadList.begin(), deadList.end(), orderSortPredicate);
		// remove the contiguous ranges of rows
		int front_bookmark = -1;
		int back_bookmark = -1;
		int currentItem = -1;
		auto removeNow = [&]() {
			beginRemoveRows(QModelIndex(), front_bookmark, back_bookmark);
			m_instances.erase(m_instances.begin() + front_bookmark,
							  m_instances.begin() + back_bookmark + 1);
			endRemoveRows();
			front_bookmark = -1;
			back_bookmark = currentItem;
		};
		for (auto& removedItem : deadList) {
			auto instPtr = removedItem.first;
			instPtr->invalidate();
			currentItem = removedItem.second;
			if (back_bookmark == -1) {
				// no bookmark yet
				back_bookmark = currentItem;
			} else if (currentItem == front_bookmark - 1) {
				// part of contiguous sequence, continue
			} else {
				// seam between previous and current item
				removeNow();
			}
			front_bookmark = currentItem;
		}
		if (back_bookmark != -1) {
			removeNow();
		}
	}
	if (newList.size()) {
		add(newList);
	}
	m_dirty = false;
	updateTotalPlayTime();
	return NoError;
}

void InstanceList::updateTotalPlayTime()
{
	totalPlayTime = 0;
	for (auto const& itr : m_instances) {
		totalPlayTime += itr.get()->totalTimePlayed();
	}
}

void InstanceList::saveNow()
{
	for (auto& item : m_instances) {
		item->saveNow();
	}
}

void InstanceList::add(const QList<InstancePtr>& t)
{
	beginInsertRows(QModelIndex(), m_instances.count(),
					m_instances.count() + t.size() - 1);
	m_instances.append(t);
	for (auto& ptr : t) {
		connect(ptr.get(), &BaseInstance::propertiesChanged, this,
				&InstanceList::propertiesChanged);
	}
	endInsertRows();
}

void InstanceList::resumeWatch()
{
	if (m_watchLevel > 0) {
		qWarning() << "Bad suspend level resume in instance list";
		return;
	}
	m_watchLevel++;
	if (m_watchLevel > 0 && m_dirty) {
		loadList();
	}
}

void InstanceList::suspendWatch()
{
	m_watchLevel--;
}

void InstanceList::providerUpdated()
{
	m_dirty = true;
	if (m_watchLevel == 1) {
		loadList();
	}
}

InstancePtr InstanceList::getInstanceById(QString instId) const
{
	if (instId.isEmpty())
		return InstancePtr();
	for (auto& inst : m_instances) {
		if (inst->id() == instId) {
			return inst;
		}
	}
	return InstancePtr();
}

/* Whether two provider names mean the same catalogue.
 *
 * The launcher records CurseForge as "curseforge", but "flame" is the
 * name the same platform goes by in pack formats and in other launchers,
 * so an instance imported with that spelling has to still be recognised
 * as the same pack. */
static bool sameProvider(const QString& left, const QString& right)
{
	auto canonical = [](const QString& provider) {
		const QString lower = provider.toLower();
		if (lower == QLatin1String("flame")) {
			return QStringLiteral("curseforge");
		}
		return lower;
	};
	return canonical(left) == canonical(right);
}

InstancePtr InstanceList::getInstanceByManagedPack(const QString& provider,
												   const QString& packId) const
{
	/* Both halves are required. A pack id on its own is a number that
	 * means different things on each catalogue, and a provider on its own
	 * would match every pack the user ever installed from it. */
	if (provider.isEmpty() || packId.isEmpty()) {
		return InstancePtr();
	}

	for (auto& inst : m_instances) {
		if (inst->managedPackId() != packId) {
			continue;
		}
		if (sameProvider(inst->managedPackProvider(), provider)) {
			return inst;
		}
	}
	return InstancePtr();
}

QModelIndex InstanceList::getInstanceIndexById(const QString& id) const
{
	return index(getInstIndex(getInstanceById(id).get()));
}

int InstanceList::getInstIndex(BaseInstance* inst) const
{
	int count = m_instances.count();
	for (int i = 0; i < count; i++) {
		if (inst == m_instances[i].get()) {
			return i;
		}
	}
	return -1;
}

void InstanceList::propertiesChanged(BaseInstance* inst)
{
	int i = getInstIndex(inst);
	if (i != -1) {
		emit dataChanged(index(i), index(i));
		updateTotalPlayTime();
	}
}

InstancePtr InstanceList::loadInstance(const InstanceId& id)
{
	if (!m_groupsLoaded) {
		loadGroupList();
	}

	auto instanceRoot = FS::PathCombine(rootDirOf(id), id);
	auto instanceSettings = std::make_shared<INISettingsObject>(
		FS::PathCombine(instanceRoot, "instance.cfg"));
	InstancePtr inst;

	instanceSettings->registerSetting("InstanceType", "Legacy");

	QString inst_type = instanceSettings->get("InstanceType").toString();

	if (inst_type == "OneSix" || inst_type == "Nostalgia") {
		inst.reset(new MinecraftInstance(m_globalSettings, instanceSettings,
										 instanceRoot));
	} else if (inst_type == "Legacy") {
		inst.reset(new LegacyInstance(m_globalSettings, instanceSettings,
									  instanceRoot));
	} else {
		inst.reset(
			new NullInstance(m_globalSettings, instanceSettings, instanceRoot));
	}
	qDebug() << "Loaded instance " << inst->name() << " from "
			 << inst->instanceRoot();
	return inst;
}

void InstanceList::saveGroupList()
{
	qDebug() << "Will save group list now.";
	if (!m_instancesProbed) {
		qDebug() << "Group saving prevented because we don't know the full "
					"list of instances yet.";
		return;
	}
	/* The group file lives in the data folder, not inside an instance
	 * folder.
	 *
	 * Grouping is not per-folder data - an instance in a secondary folder
	 * can share a group with one from the primary, so a file per root
	 * could not express it. Keeping the single file in the primary folder
	 * instead, which is what this used to do, ties it to a folder the user
	 * is free to repoint, and repointing it then loses every group.
	 *
	 * No WatchLock any more: the path is no longer inside a watched
	 * instance folder, so writing it cannot trip the directory watcher
	 * into a rescan and there is nothing to suspend.
	 */
	QString groupFileName = QDir::current().filePath("instgroups.json");
	QMap<QString, QSet<QString>> reverseGroupMap;
	for (auto iter = m_instanceGroupIndex.begin();
		 iter != m_instanceGroupIndex.end(); iter++) {
		QString id = iter.key();
		QString group = iter.value();
		if (group.isEmpty())
			continue;
		if (!instanceSet.contains(id)) {
			qDebug() << "Skipping saving missing instance" << id
					 << "to groups list.";
			continue;
		}

		if (!reverseGroupMap.count(group)) {
			QSet<QString> set;
			set.insert(id);
			reverseGroupMap[group] = set;
		} else {
			QSet<QString>& set = reverseGroupMap[group];
			set.insert(id);
		}
	}
	QJsonObject toplevel;
	toplevel.insert("formatVersion", QJsonValue(QString("1")));
	QJsonObject groupsArr;
	for (auto iter = reverseGroupMap.begin(); iter != reverseGroupMap.end();
		 iter++) {
		auto list = iter.value();
		auto name = iter.key();
		QJsonObject groupObj;
		QJsonArray instanceArr;
		groupObj.insert("hidden", QJsonValue(m_collapsedGroups.contains(name)));
		for (auto item : list) {
			instanceArr.append(QJsonValue(item));
		}
		groupObj.insert("instances", instanceArr);
		groupsArr.insert(name, groupObj);
	}
	toplevel.insert("groups", groupsArr);
	QJsonDocument doc(toplevel);
	try {
		FS::write(groupFileName, doc.toJson());
		qDebug() << "Group list saved.";
	} catch (const FS::FileSystemException& e) {
		qCritical() << "Failed to write instance group file :" << e.cause();
	}
}

void InstanceList::loadGroupList()
{
	qDebug() << "Will load group list now.";

	QString groupFileName = QDir::current().filePath("instgroups.json");

	/* Start from nothing.
	 *
	 * This runs again whenever the folder settings change, and without
	 * clearing first, the indices would accumulate: groups for instances
	 * that no longer exist, and collapsed-state for groups that are gone.
	 * The file is the whole truth about grouping, so re-reading it should
	 * replace what we hold rather than add to it.
	 */
	m_instanceGroupIndex.clear();
	m_groupNameCache.clear();
	m_collapsedGroups.clear();

	bool migratingLegacyGroups = false;

	/* No file in the data folder? Look where it used to live.
	 *
	 * The group file was kept inside the primary instance folder until the
	 * launcher learned about more than one of them. Reading the old path
	 * once, and writing the result back to the new one at the end of this
	 * function, is what stops an upgrade from looking like every group the
	 * user ever made was deleted. The old file is left alone rather than
	 * removed: it costs nothing and it is the only copy if anything about
	 * this goes wrong.
	 */
	if (!QFileInfo::exists(groupFileName)) {
		QString legacyGroupFileName =
			FS::PathCombine(primaryDir(), "instgroups.json");
		if (!QFileInfo::exists(legacyGroupFileName)) {
			return;
		}
		qInfo() << "Migrating instance groups from legacy location"
				<< legacyGroupFileName;
		groupFileName = legacyGroupFileName;
		migratingLegacyGroups = true;
	}

	QByteArray jsonData;
	try {
		jsonData = FS::read(groupFileName);
	} catch (const FS::FileSystemException& e) {
		qCritical() << "Failed to read instance group file :" << e.cause();
		return;
	}

	QJsonParseError error;
	QJsonDocument jsonDoc = QJsonDocument::fromJson(jsonData, &error);

	// if the json was bad, fail
	if (error.error != QJsonParseError::NoError) {
		qCritical()
			<< QString("Failed to parse instance group file: %1 at offset %2")
				   .arg(error.errorString(), QString::number(error.offset))
				   .toUtf8();
		return;
	}

	// if the root of the json wasn't an object, fail
	if (!jsonDoc.isObject()) {
		qWarning() << "Invalid group file. Root entry should be an object.";
		return;
	}

	QJsonObject rootObj = jsonDoc.object();

	// Make sure the format version matches, otherwise fail.
	if (rootObj.value("formatVersion").toVariant().toInt() !=
		GROUP_FILE_FORMAT_VERSION)
		return;

	// Get the groups. if it's not an object, fail
	if (!rootObj.value("groups").isObject()) {
		qWarning() << "Invalid group list JSON: 'groups' should be an object.";
		return;
	}

	QSet<QString> groupSet;
	m_instanceGroupIndex.clear();

	// Iterate through all the groups.
	QJsonObject groupMapping = rootObj.value("groups").toObject();
	for (QJsonObject::iterator iter = groupMapping.begin();
		 iter != groupMapping.end(); iter++) {
		QString groupName = iter.key();

		// If not an object, complain and skip to the next one.
		if (!iter.value().isObject()) {
			qWarning()
				<< QString("Group '%1' in the group list should be an object.")
					   .arg(groupName)
					   .toUtf8();
			continue;
		}

		QJsonObject groupObj = iter.value().toObject();
		if (!groupObj.value("instances").isArray()) {
			qWarning() << QString("Group '%1' in the group list is invalid. It "
								  "should contain an array called 'instances'.")
							  .arg(groupName)
							  .toUtf8();
			continue;
		}

		// keep a list/set of groups for choosing
		groupSet.insert(groupName);

		auto hidden = groupObj.value("hidden").toBool(false);
		if (hidden) {
			m_collapsedGroups.insert(groupName);
		}

		// Iterate through the list of instances in the group.
		QJsonArray instancesArray = groupObj.value("instances").toArray();

		for (QJsonArray::iterator iter2 = instancesArray.begin();
			 iter2 != instancesArray.end(); iter2++) {
			m_instanceGroupIndex[(*iter2).toString()] = groupName;
		}
	}
	m_groupsLoaded = true;
	m_groupNameCache.unite(groupSet);
	qDebug() << "Group list loaded.";

	/* Having read the old file, write the new one.
	 *
	 * Done here rather than left to the next thing that happens to save,
	 * so that the migration completes on the launch that noticed it - if
	 * it waited for the user to rename a group, a crash in between would
	 * leave the groups only in a location nothing reads any more.
	 *
	 * m_groupsLoaded is already true, so this cannot recurse.
	 */
	if (migratingLegacyGroups) {
		saveGroupList();
	}
}

void InstanceList::instanceDirContentsChanged(const QString& path)
{
	Q_UNUSED(path);
	emit instancesChanged();
}

void InstanceList::on_InstFolderChanged(const Setting& setting, QVariant value)
{
	(void)setting;
	(void)value;
	/* Both folder settings are re-read rather than the changed one being
	 * applied on its own.
	 *
	 * This slot is connected to "InstanceDir" and to
	 * "AdditionalInstanceDirs", and the roots are an ordered set built
	 * from the pair - the primary decides where new instances and the
	 * group file go, so a change to either has to be resolved against the
	 * other. Taking the value argument alone cannot do that, and there is
	 * nothing to gain by trying: reading a setting is cheap next to the
	 * rescan this triggers.
	 */
	QStringList dirs;
	dirs << m_globalSettings->get("InstanceDir").toString();
	dirs << decodeInstanceDirList(
		m_globalSettings->get("AdditionalInstanceDirs"));

	const QStringList resolved = resolveInstanceDirs(dirs);
	if (resolved == m_instDirs) {
		return;
	}

	/* Flush the groups before the switch, not after.
	 *
	 * saveGroupList() writes to primaryDir(), so once the new list is in
	 * place it would write the current groups into the *new* primary
	 * folder - inventing a group file there and leaving the folder the
	 * user is moving away from with a stale one. Saving first puts them
	 * back where they came from.
	 */
	if (m_groupsLoaded) {
		saveGroupList();
	}

	applyInstanceDirs(resolved);
	m_groupsLoaded = false;
	emit instancesChanged();
}

void InstanceList::on_GroupStateChanged(const QString& group, bool collapsed)
{
	qDebug() << "Group" << group << (collapsed ? "collapsed" : "expanded");
	if (collapsed) {
		m_collapsedGroups.insert(group);
	} else {
		m_collapsedGroups.remove(group);
	}
	saveGroupList();
}

class InstanceStaging : public Task
{
	Q_OBJECT
	const unsigned minBackoff = 1;
	const unsigned maxBackoff = 16;

  public:
	InstanceStaging(InstanceList* parent, InstanceTask* child,
					const QString& stagingPath, const QString& groupName)
		: backoff(minBackoff, maxBackoff)
	{
		m_parent = parent;
		m_instanceTask = child;
		m_child.reset(child);
		connect(child, &Task::succeeded, this, &InstanceStaging::childSucceded);
		connect(child, &Task::failed, this, &InstanceStaging::childFailed);
		connect(child, &Task::status, this, &InstanceStaging::setStatus);
		connect(child, &Task::details, this, &InstanceStaging::setDetails);
		connect(child, &Task::progress, this, &InstanceStaging::setProgress);
		/* The dialog is watching us, not the task we wrap, so anything the
		 * task says about its abort button has to be passed through -
		 * including the point where it turns into a "Skip" button for the
		 * optional game-file download. Forwarded rather than recomputed:
		 * our own canAbort() already asks the child, so re-deriving it here
		 * would be two answers to one question. */
		connect(child, &Task::abortStatusChanged, this,
				&InstanceStaging::abortStatusChanged);
		connect(child, &Task::abortButtonTextChanged, this,
				&InstanceStaging::abortButtonTextChanged);
		// We are only a wrapper around the real work. Without this the step
		// list of whatever we are staging never reaches the dialog.
		propagateStepsFrom(child);
		m_groupName = groupName;
		m_stagingPath = stagingPath;
		m_backoffTimer.setSingleShot(true);
		connect(&m_backoffTimer, &QTimer::timeout, this,
				&InstanceStaging::childSucceded);
	}

	virtual ~InstanceStaging() {};

	// FIXME/TODO: add ability to abort during instance commit retries
	bool abort() override
	{
		if (m_child && m_child->canAbort()) {
			return m_child->abort();
		}
		return false;
	}
	bool canAbort() const override
	{
		if (m_child && m_child->canAbort()) {
			return true;
		}
		return false;
	}

  protected:
	virtual void executeTask() override
	{
		m_child->start();
	}
	QStringList warnings() const override
	{
		/* Both halves of the job: what the task ran into while building
		 * the instance, and what went wrong while putting it in place.
		 * The second only exists here, because committing happens after
		 * the task has already finished. */
		return m_child->warnings() + m_commitWarnings;
	}
	bool isMultiStep() const override
	{
		return m_child && m_child->isMultiStep();
	}
	TaskStepProgressList getStepProgress() const override
	{
		if (!m_child) {
			return {};
		}
		return m_child->getStepProgress();
	}

  private slots:
	void childSucceded()
	{
		unsigned sleepTime = backoff();

		/* Asked now rather than remembered from construction time.
		 *
		 * A task does not necessarily know at the moment it is wrapped
		 * whether it is replacing an instance or creating one - that can
		 * depend on what it finds once it starts looking - and the name
		 * it settles on can change while it runs, too. Reading both here
		 * means the staging step always acts on what the task actually
		 * decided, instead of on a snapshot taken before it ran. */
		const QString instanceName = m_instanceTask->name();
		const QString overrideInstanceId = m_instanceTask->overrideInstanceId();
		const QStringList filesToRemove =
			m_instanceTask->filesToRemoveAfterCommit();

		/* Cleared first: this slot runs again on every backoff retry,
		 * and a warning from an attempt that was then retried should not
		 * be reported twice - or at all, if the retry got further. */
		m_commitWarnings.clear();

		if (m_parent->commitStagedInstance(m_stagingPath, instanceName,
										   m_groupName, overrideInstanceId,
										   filesToRemove, &m_commitWarnings)) {
			emitSucceeded();
			return;
		}
		// we actually failed, retry?
		if (sleepTime == maxBackoff) {
			emitFailed(tr("Failed to commit instance, even after multiple "
						  "retries. It is being blocked by something."));
			return;
		}
		qDebug() << "Failed to commit instance" << instanceName
				 << "Initiating backoff:" << sleepTime;
		m_backoffTimer.start(sleepTime * 500);
	}
	void childFailed(const QString& reason)
	{
		m_parent->destroyStagingPath(m_stagingPath);
		emitFailed(reason);
	}

  private:
	/*
	 * WHY: the whole reason why this uses an exponential backoff retry scheme
	 * is antivirus on Windows. Basically, it starts messing things up while
	 * MeshMC is extracting/creating instances and causes that horrible failure
	 * that is NTFS to lock files in place because they are open.
	 */
	ExponentialSeries backoff;
	QString m_stagingPath;
	InstanceList* m_parent;
	unique_qobject_ptr<Task> m_child;
	QString m_groupName;
	/* Same object as m_child, kept at its real type so that the commit
	 * step can ask it the instance-level questions - its name, and
	 * whether it is replacing an existing instance - that Task does not
	 * answer. Ownership stays with m_child. */
	InstanceTask* m_instanceTask = nullptr;
	QTimer m_backoffTimer;
	/* Problems the commit itself ran into that are worth telling the
	 * user about without failing the whole thing. Merged into
	 * warnings(), which is where the caller looks after we succeed. */
	QStringList m_commitWarnings;
};

Task* InstanceList::wrapInstanceTask(InstanceTask* task)
{
	QString targetDir = task->targetDir();
	if (targetDir.isEmpty() && task->shouldOverride()) {
		/* An update is staged in the folder its instance already lives in,
		 * not in the primary one.
		 *
		 * Committing an override merges the staged tree over the existing
		 * instance, so staging elsewhere would make every pack update of
		 * an instance in a secondary folder a cross-filesystem copy of the
		 * whole pack. The caller is updating a specific instance and has
		 * no reason to know which disk it sits on, so this is worked out
		 * here rather than pushed onto them.
		 */
		targetDir = rootDirOf(task->overrideInstanceId());
	}
	auto stagingPath = getStagedInstancePath(targetDir);
	task->setStagingPath(stagingPath);
	task->setParentSettings(m_globalSettings);
	/* The group is settled before wrapping and does not change; the name
	 * and the override target are read at commit time instead, because
	 * the task may still decide them while it runs. */
	return new InstanceStaging(this, task, stagingPath, task->group());
}

QString InstanceList::getStagedInstancePath(const QString& targetDir)
{
	QString root = primaryDir();
	if (!targetDir.isEmpty()) {
		/* Refuse rather than fall back to the primary folder. A target
		 * that is not configured means either a caller bug or a folder the
		 * user removed from the settings while this task sat in the queue;
		 * quietly installing somewhere else is the one outcome nobody
		 * asked for, and it would be discovered much later. */
		if (!m_instDirs.contains(targetDir)) {
			qCritical() << "Requested instance folder" << targetDir
						<< "is not one of the configured folders";
			return QString();
		}
		if (!QDir(targetDir).exists()) {
			qCritical() << "Requested instance folder" << targetDir
						<< "is configured but not accessible on disk";
			return QString();
		}
		root = targetDir;
	}
	if (root.isEmpty()) {
		qCritical() << "No usable instance folder to stage into";
		return QString();
	}

	QString key = QUuid::createUuid().toString();
	QString relPath = FS::PathCombine("_MESHMC_TEMP/", key);
	QDir rootPath(root);
	auto path = FS::PathCombine(root, relPath);
	if (!rootPath.mkpath(relPath)) {
		return QString();
	}
	return path;
}

bool InstanceList::commitStagedInstance(const QString& path,
										const QString& instanceName,
										const QString& groupName,
										const QString& overrideInstanceId,
										const QStringList& filesToRemove,
										QStringList* removalWarnings)
{
	const bool isOverride = !overrideInstanceId.isEmpty();

	/* Which folder this instance is landing in.
	 *
	 * For an override it is wherever the instance already is - moving it
	 * between folders is not what updating a pack means. For a new
	 * instance it is the folder the staging directory is already inside,
	 * read back off the path rather than taken from the task a second
	 * time: committing is a rename, and a rename only stays a rename
	 * while both ends are on one filesystem. Deriving the destination
	 * from the source is what guarantees that.
	 */
	const QString root = isOverride ? rootDirOf(overrideInstanceId)
									: rootForStaging(path);
	if (root.isEmpty()) {
		qWarning() << "Cannot commit" << path
				   << "- no configured instance folder contains it";
		return false;
	}

	QDir dir;
	QString instID = isOverride ? overrideInstanceId
								: FS::DirNameFromString(instanceName, root);
	{
		WatchLock lock(m_watcher, root);
		QString destination = FS::PathCombine(root, instID);

		if (isOverride) {
			/* Merge over the instance that is already there, so that
			 * anything the pack does not ship - worlds, screenshots,
			 * the user's own config edits - is left where it is. */
			if (!FS::overrideFolder(destination, path)) {
				qWarning() << "Failed to merge" << path << "over"
						   << destination;
				return false;
			}

			/* No group or id bookkeeping to do: the instance already has
			 * both, and the whole point of overriding is that they do
			 * not change. Its name, however, lives inside the staged
			 * instance.cfg we just moved into place, so it has already
			 * been updated. */

			/* Re-read that file into the instance we still have in
			 * memory.
			 *
			 * loadList() deliberately keeps the existing object for an
			 * id it already knows, so nothing else is going to notice
			 * that instance.cfg changed under it. Two things then go
			 * wrong, and the second is the dangerous one:
			 *
			 *  - everything still shows the pre-update values, so the
			 *    pack page reports the version we just replaced;
			 *  - INISettingsObject rewrites the whole file on any set(),
			 *    so the first unrelated write - launching the instance
			 *    is enough, it touches lastLaunchTime - flushes the
			 *    stale in-memory copy back to disk and silently undoes
			 *    part of the update.
			 */
			if (auto existing = getInstanceById(instID)) {
				if (!existing->settings()->reload()) {
					qWarning() << "Could not re-read settings for" << instID
							   << "after update";
				}
			}

			/* Now that the new version is actually in place, remove what
			 * it no longer ships.
			 *
			 * After the merge, not before: a file the update makes
			 * obsolete is still a file the *current* version needs, so
			 * deleting it earlier would leave a failed or aborted update
			 * with an instance missing its mods. By this point the merge
			 * has succeeded and the old files are genuinely orphans.
			 *
			 * A failure to delete is reported but not fatal - the
			 * instance is already updated and consistent; a leftover jar
			 * is a problem to tell the user about, not a reason to
			 * declare the whole update failed and leave them with no
			 * idea what state they are in.
			 *
			 * Told to the user, though, and not only to the log: a mod
			 * the new version does not include is still going to be
			 * loaded by the game, so an update that could not remove one
			 * has not entirely happened. Only the user can free the file
			 * and only if they know about it. */
			for (const QString& path : filesToRemove) {
				if (!QFileInfo::exists(path)) {
					continue;
				}
				if (!FS::deletePath(path)) {
					qWarning() << "Could not remove obsolete file" << path;
					if (removalWarnings) {
						removalWarnings->append(
							tr("Could not remove a file the updated pack no "
							   "longer includes: %1")
								.arg(QDir::toNativeSeparators(path)));
					}
				} else {
					qDebug() << "Removed obsolete file" << path;
				}
			}
		} else {
			if (!dir.rename(path, destination)) {
				qWarning() << "Failed to move" << path << "to" << destination;
				return false;
			}
			m_instanceGroupIndex[instID] = groupName;
			m_groupNameCache.insert(groupName);
		}

		instanceSet.insert(instID);
		/* Record the folder now, rather than waiting for the next
		 * discovery pass. Everything between here and that pass - loading
		 * the instance so it can be selected, most immediately - asks
		 * rootDirOf() for its path, and without this it would be answered
		 * with the primary folder and look in the wrong place. */
		m_instanceRootDirMap[instID] = root;
		emit instancesChanged();
		emit instanceSelectRequest(instID);
	}
	saveGroupList();
	return true;
}

bool InstanceList::destroyStagingPath(const QString& keyPath)
{
	return FS::deletePath(keyPath);
}

int InstanceList::getTotalPlayTime()
{
	updateTotalPlayTime();
	return totalPlayTime;
}

#include "InstanceList.moc"
