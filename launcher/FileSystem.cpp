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

#include "FileSystem.h"

#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QFileInfo>
#include <QDebug>
#include <QUrl>
#include <QStandardPaths>
#include <QTextStream>

#if defined Q_OS_WIN32
#include <windows.h>
#include <string>
#include <sys/utime.h>
#include <winnls.h>
#include <shobjidl.h>
#include <objbase.h>
#include <objidl.h>
#include <shlguid.h>
#include <shlobj.h>
#include <versionhelpers.h>
#else
#include <utime.h>
#endif

#include "DesktopServices.h"

namespace FS
{

	void ensureExists(const QDir& dir)
	{
		if (!QDir().mkpath(dir.absolutePath())) {
			throw FileSystemException("Unable to create folder " +
									  dir.dirName() + " (" +
									  dir.absolutePath() + ")");
		}
	}

	void write(const QString& filename, const QByteArray& data)
	{
		ensureExists(QFileInfo(filename).dir());
		QSaveFile file(filename);
		if (!file.open(QSaveFile::WriteOnly)) {
			throw FileSystemException("Couldn't open " + filename +
									  " for writing: " + file.errorString());
		}
		if (data.size() != file.write(data)) {
			throw FileSystemException("Error writing data to " + filename +
									  ": " + file.errorString());
		}
		if (!file.commit()) {
			throw FileSystemException("Error while committing data to " +
									  filename + ": " + file.errorString());
		}
	}

	QByteArray read(const QString& filename)
	{
		QFile file(filename);
		if (!file.open(QFile::ReadOnly)) {
			throw FileSystemException("Unable to open " + filename +
									  " for reading: " + file.errorString());
		}
		const qint64 size = file.size();
		QByteArray data(int(size), 0);
		const qint64 ret = file.read(data.data(), size);
		if (ret == -1 || ret != size) {
			throw FileSystemException("Error reading data from " + filename +
									  ": " + file.errorString());
		}
		return data;
	}

	bool updateTimestamp(const QString& filename)
	{
#ifdef Q_OS_WIN32
		std::wstring filename_utf_16 = filename.toStdWString();
		return (_wutime64(filename_utf_16.c_str(), nullptr) == 0);
#else
		QByteArray filenameBA = QFile::encodeName(filename);
		return (utime(filenameBA.data(), nullptr) == 0);
#endif
	}

	bool ensureFilePathExists(QString filenamepath)
	{
		QFileInfo a(filenamepath);
		QDir dir;
		QString ensuredPath = a.path();
		bool success = dir.mkpath(ensuredPath);
		return success;
	}

	bool ensureFolderPathExists(QString foldernamepath)
	{
		QFileInfo a(foldernamepath);
		QDir dir;
		QString ensuredPath = a.filePath();
		bool success = dir.mkpath(ensuredPath);
		return success;
	}

	bool copy::operator()(const QString& offset)
	{
// NOTE always deep copy on windows. the alternatives are too messy.
#if defined Q_OS_WIN32
		m_followSymlinks = true;
#endif

		auto src = PathCombine(m_src.absolutePath(), offset);
		auto dst = PathCombine(m_dst.absolutePath(), offset);

		QFileInfo currentSrc(src);
		if (!currentSrc.exists())
			return false;

		if (!m_followSymlinks && currentSrc.isSymLink()) {
			qDebug() << "creating symlink" << src << " - " << dst;
			if (!ensureFilePathExists(dst)) {
				qWarning() << "Cannot create path!";
				return false;
			}
			return QFile::link(currentSrc.symLinkTarget(), dst);
		} else if (currentSrc.isFile()) {
			qDebug() << "copying file" << src << " - " << dst;
			if (!ensureFilePathExists(dst)) {
				qWarning() << "Cannot create path!";
				return false;
			}
			return QFile::copy(src, dst);
		} else if (currentSrc.isDir()) {
			qDebug() << "recursing" << offset;
			if (!ensureFolderPathExists(dst)) {
				qWarning() << "Cannot create path!";
				return false;
			}
			QDir currentDir(src);
			for (auto& f : currentDir.entryList(QDir::Files | QDir::Dirs |
												QDir::NoDotAndDotDot |
												QDir::Hidden | QDir::System)) {
				auto inner_offset = PathCombine(offset, f);
				// ignore and skip stuff that matches the blacklist.
				if (m_blacklist && m_blacklist->matches(inner_offset)) {
					continue;
				}
				if (!operator()(inner_offset)) {
					qWarning() << "Failed to copy" << inner_offset;
					return false;
				}
			}
		} else {
			qCritical() << "Copy ERROR: Unknown filesystem object:" << src;
			return false;
		}
		return true;
	}

	bool canTrash()
	{
		/* Qt writes into the sandbox's own trash instead of asking the
		 * host through the portal, so from the user's side the folder
		 * would simply vanish with no way back. */
		if (DesktopServices::isFlatpak()) {
			return false;
		}

#if defined(Q_OS_WIN)
		/* A Server install has no recycle bin unless one was set up, and
		 * the shell answers a recycle request on such a volume by
		 * deleting outright -- the one thing a trash operation is
		 * supposed to rule out. */
		if (IsWindowsServer()) {
			return false;
		}
#endif
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    return QFile::supportsMoveToTrash();
#else
    return true;
#endif
	}

	bool trash(const QString& path, QString* pathInTrash)
	{
		if (!canTrash()) {
			return false;
		}
		return QFile::moveToTrash(path, pathInTrash);
	}

	bool overrideFolder(const QString& destination, const QString& source)
	{
		if (!ensureFolderPathExists(destination)) {
			qWarning() << "Cannot create override destination" << destination;
			return false;
		}

		const QDir sourceDir(source);
		if (!sourceDir.exists()) {
			qWarning() << "Override source" << source << "does not exist";
			return false;
		}

		const auto entries = sourceDir.entryInfoList(
			QDir::NoDotAndDotDot | QDir::System | QDir::Hidden |
				QDir::AllDirs | QDir::Files,
			QDir::DirsFirst);

		for (const QFileInfo& entry : entries) {
			const QString target =
				PathCombine(destination, entry.fileName());

			if (entry.isDir() && !entry.isSymLink()) {
				/* Recurse rather than move the directory wholesale: the
				 * destination almost certainly has a directory of the
				 * same name (every pack ships "mods", every instance
				 * already has one) and moving onto an existing
				 * directory fails. Merging is the entire point. */
				if (!overrideFolder(target, entry.absoluteFilePath())) {
					return false;
				}
				continue;
			}

			/* QFile::rename refuses to clobber, so anything already
			 * there has to go first. This is the case that makes an
			 * update an update: the pack's own files are replaced. */
			if (QFileInfo::exists(target) && !deletePath(target)) {
				qWarning() << "Cannot replace" << target;
				return false;
			}

			if (!QFile::rename(entry.absoluteFilePath(), target)) {
				/* Across filesystems rename fails and there is nothing
				 * to be gained by pretending otherwise -- the staging
				 * directory lives under the instance root precisely so
				 * that this stays a rename. */
				qWarning() << "Cannot move" << entry.absoluteFilePath()
						   << "to" << target;
				return false;
			}
		}

		/* Whatever is left is empty directories we have just drained. */
		if (!deletePath(source)) {
			/* Not fatal: the merge itself succeeded and the instance is
			 * consistent. A leftover staging directory is litter, not
			 * corruption, so say so and carry on. */
			qWarning() << "Merged" << source << "but could not remove it";
		}

		return true;
	}

	bool deletePath(QString path)
	{
		bool OK = true;
		QFileInfo finfo(path);
		if (finfo.isFile()) {
			return QFile::remove(path);
		}

		QDir dir(path);

		if (!dir.exists()) {
			return OK;
		}
		auto allEntries =
			dir.entryInfoList(QDir::NoDotAndDotDot | QDir::System |
								  QDir::Hidden | QDir::AllDirs | QDir::Files,
							  QDir::DirsFirst);

		for (auto& info : allEntries) {
#if defined Q_OS_WIN32
			QString nativePath =
				QDir::toNativeSeparators(info.absoluteFilePath());
			auto wString = nativePath.toStdWString();
			DWORD dwAttrs = GetFileAttributesW(wString.c_str());
			// Windows: check for junctions, reparse points and other nasty
			// things of that sort
			if (dwAttrs & FILE_ATTRIBUTE_REPARSE_POINT) {
				if (info.isFile()) {
					OK &= QFile::remove(info.absoluteFilePath());
				} else if (info.isDir()) {
					OK &= dir.rmdir(info.absoluteFilePath());
				}
			}
#else
			// We do not trust Qt with reparse points, but do trust it with unix
			// symlinks.
			if (info.isSymLink()) {
				OK &= QFile::remove(info.absoluteFilePath());
			}
#endif
			else if (info.isDir()) {
				OK &= deletePath(info.absoluteFilePath());
			} else if (info.isFile()) {
				OK &= QFile::remove(info.absoluteFilePath());
			} else {
				OK = false;
				qCritical() << "Delete ERROR: Unknown filesystem object:"
							<< info.absoluteFilePath();
			}
		}
		OK &= dir.rmdir(dir.absolutePath());
		return OK;
	}

	QString PathCombine(const QString& path1, const QString& path2)
	{
		if (!path1.size())
			return path2;
		if (!path2.size())
			return path1;
		return QDir::cleanPath(path1 + QDir::separator() + path2);
	}

	QString PathCombine(const QString& path1, const QString& path2,
						const QString& path3)
	{
		return PathCombine(PathCombine(path1, path2), path3);
	}

	QString PathCombine(const QString& path1, const QString& path2,
						const QString& path3, const QString& path4)
	{
		return PathCombine(PathCombine(path1, path2, path3), path4);
	}

	QString AbsolutePath(QString path)
	{
		return QFileInfo(path).absolutePath();
	}

	QString ResolveExecutable(QString path)
	{
		if (path.isEmpty()) {
			return QString();
		}
		if (!path.contains('/')) {
			path = QStandardPaths::findExecutable(path);
		}
		QFileInfo pathInfo(path);
		if (!pathInfo.exists() || !pathInfo.isExecutable()) {
			return QString();
		}
		return pathInfo.absoluteFilePath();
	}

	/**
	 * Normalize path
	 *
	 * Any paths inside the current folder will be normalized to relative paths
	 * (to current) Other paths will be made absolute
	 */
	QString NormalizePath(QString path)
	{
		QDir a = QDir::currentPath();
		QString currentAbsolute = a.absolutePath();

		QDir b(path);
		QString newAbsolute = b.absolutePath();

		if (newAbsolute.startsWith(currentAbsolute)) {
			return a.relativeFilePath(newAbsolute);
		} else {
			return newAbsolute;
		}
	}

	QString badFilenameChars = "\"\\/?<>:;*|!+\r\n";

	QString RemoveInvalidFilenameChars(QString string, QChar replaceWith)
	{
		for (int i = 0; i < string.length(); i++) {
			if (badFilenameChars.contains(string[i])) {
				string[i] = replaceWith;
			}
		}
		return string;
	}

	QString DirNameFromString(QString string, QString inDir)
	{
		int num = 0;
		QString baseName = RemoveInvalidFilenameChars(string, '-');
		QString dirName;
		do {
			if (num == 0) {
				dirName = baseName;
			} else {
				dirName = baseName + QString::number(num);
				;
			}

			// If it's over 9000
			if (num > 9000)
				return "";
			num++;
		} while (QFileInfo(PathCombine(inDir, dirName)).exists());
		return dirName;
	}

	// Does the folder path contain any '!'? If yes, return true, otherwise
	// false. (This is a problem for Java)
	bool checkProblemticPathJava(QDir folder)
	{
		QString pathfoldername = folder.absolutePath();
		return pathfoldername.contains("!", Qt::CaseInsensitive);
	}

// Win32 crap
#if defined Q_OS_WIN

	bool called_coinit = false;

	HRESULT CreateLink(LPCSTR linkPath, LPCSTR targetPath, LPCSTR args)
	{
		HRESULT hres;

		if (!called_coinit) {
			hres = CoInitialize(NULL);
			called_coinit = true;

			if (!SUCCEEDED(hres)) {
				qWarning("Failed to initialize COM. Error 0x%08lX", hres);
				return hres;
			}
		}

		IShellLinkA* link;
		hres = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
								IID_IShellLinkA, (LPVOID*)&link);

		if (SUCCEEDED(hres)) {
			IPersistFile* persistFile;

			link->SetPath(targetPath);
			link->SetArguments(args);

			hres =
				link->QueryInterface(IID_IPersistFile, (LPVOID*)&persistFile);
			if (SUCCEEDED(hres)) {
				WCHAR wstr[MAX_PATH];

				MultiByteToWideChar(CP_ACP, 0, linkPath, -1, wstr, MAX_PATH);

				hres = persistFile->Save(wstr, TRUE);
				persistFile->Release();
			}
			link->Release();
		}
		return hres;
	}

#endif

	QString getDesktopDir()
	{
		return QStandardPaths::writableLocation(
			QStandardPaths::DesktopLocation);
	}

	QString getApplicationsDir()
	{
		return QStandardPaths::writableLocation(
			QStandardPaths::ApplicationsLocation);
	}

	namespace
	{
		/* Joins arguments into one command line for a file that a shell or
		 * the shell API will read back, wrapping each argument in @p wrap and
		 * rewriting any wrapper character inside an argument as @p escaped.
		 * With @p onlyIfNeeded an argument that cannot be misread is left
		 * bare, which keeps a Windows link's argument string readable in its
		 * properties dialog. */
		QString quoteArgs(const QStringList& args, const QString& wrap,
						  const QString& escaped, bool onlyIfNeeded = false)
		{
			QStringList quoted;
			quoted.reserve(args.size());
			for (QString arg : args) {
				arg.replace(wrap, escaped);
				const bool needsWrap = !onlyIfNeeded || arg.isEmpty() ||
									   arg.contains(QLatin1Char(' ')) ||
									   arg.contains(QLatin1Char('\t')) ||
									   arg.contains(wrap);
				quoted << (needsWrap ? wrap + arg + wrap : arg);
			}
			return quoted.join(QLatin1Char(' '));
		}
	} // namespace

	QString createShortcut(QString destination, QString target,
						   QStringList args, QString name, QString iconPath)
	{
		if (destination.isEmpty()) {
			destination =
				PathCombine(getDesktopDir(), RemoveInvalidFilenameChars(name));
		}
		if (!ensureFilePathExists(destination)) {
			qWarning() << "Cannot create the folder the shortcut goes in:"
					   << destination;
			return QString();
		}

#if defined(Q_OS_MACOS)
		/* Finder only honours a custom icon and fixed arguments for a real
		 * bundle, so the shortcut is a minimal .app around a shell script. */
		QDir bundle(destination + QLatin1String(".app"));
		if (bundle.exists()) {
			qWarning() << "Refusing to overwrite the bundle already at"
					   << bundle.path();
			return QString();
		}

		QDir contents(bundle.filePath(QStringLiteral("Contents")));
		QDir resources(contents.filePath(QStringLiteral("Resources")));
		QDir macos(contents.filePath(QStringLiteral("MacOS")));
		if (!(bundle.mkpath(".") && contents.mkpath(".") &&
			  resources.mkpath(".") && macos.mkpath("."))) {
			qWarning() << "Could not lay out the bundle at" << bundle.path();
			return QString();
		}

		if (!iconPath.isEmpty()) {
			QFile::copy(iconPath,
						resources.filePath(QStringLiteral("Icon.icns")));
		}

		QFile runner(macos.filePath(QStringLiteral("Run.command")));
		if (!runner.open(QIODevice::WriteOnly | QIODevice::Text)) {
			qWarning() << "Cannot write" << runner.fileName() << ":"
					   << runner.errorString();
			return QString();
		}
		{
			QTextStream stream(&runner);
			stream << "#!/bin/bash\n";
			/* One list, one call: quoting the target and the arguments
			 * separately and gluing them with a space left a trailing
			 * space behind whenever there were no arguments. Harmless --
			 * every parser drops it -- but it is written into a file a
			 * user can open. */
			QStringList command{target};
			command += args;
			stream << quoteArgs(command, QStringLiteral("\""),
								QStringLiteral("\\\""))
				   << "\n";
		}
		runner.close();
		runner.setPermissions(runner.permissions() | QFileDevice::ExeOwner |
							  QFileDevice::ExeGroup | QFileDevice::ExeOther);

		QFile info(contents.filePath(QStringLiteral("Info.plist")));
		if (!info.open(QIODevice::WriteOnly | QIODevice::Text)) {
			qWarning() << "Cannot write" << info.fileName() << ":"
					   << info.errorString();
			return QString();
		}
		{
			QTextStream stream(&info);
			stream << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
				   << "<!DOCTYPE plist PUBLIC \"-//Apple Computer//DTD PLIST "
					  "1.0//EN\" "
					  "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
				   << "<plist version=\"1.0\">\n<dict>\n"
				   << "\t<key>CFBundleExecutable</key>\n"
				   << "\t<string>Run.command</string>\n"
				   << "\t<key>CFBundleIconFile</key>\n"
				   << "\t<string>Icon.icns</string>\n"
				   << "\t<key>CFBundleName</key>\n\t<string>" << name
				   << "</string>\n"
				   << "\t<key>CFBundlePackageType</key>\n"
				   << "\t<string>APPL</string>\n"
				   << "\t<key>CFBundleShortVersionString</key>\n"
				   << "\t<string>1.0</string>\n"
				   << "\t<key>CFBundleVersion</key>\n\t<string>1.0</string>\n"
				   << "</dict>\n</plist>\n";
		}
		info.close();
		return bundle.path();

#elif defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD) || defined(Q_OS_OPENBSD)
		if (!destination.endsWith(QLatin1String(".desktop"))) {
			destination += QLatin1String(".desktop");
		}

		QFile entry(destination);
		if (!entry.open(QIODevice::WriteOnly | QIODevice::Text)) {
			qWarning() << "Cannot write" << destination << ":"
					   << entry.errorString();
			return QString();
		}
		{
			QTextStream stream(&entry);
			stream << "[Desktop Entry]\n";
			stream << "Type=Application\n";
			stream << "Categories=Game;ActionGame;AdventureGame;Simulation\n";
			/* One list, one call, so that an argument-less shortcut does
			 * not get a trailing space after Exec=. With arguments the
			 * line is byte-for-byte what it was: quoteArgs() joins with
			 * the same single space. */
			QStringList command{target};
			command += args;
			stream << "Exec="
				   << quoteArgs(command, QStringLiteral("'"),
								QStringLiteral("'\\''"))
				   << "\n";
			stream << "Name=" << name << "\n";
			if (!iconPath.isEmpty()) {
				stream << "Icon=" << iconPath << "\n";
			}
		}
		entry.close();
		entry.setPermissions(entry.permissions() | QFileDevice::ExeOwner |
							 QFileDevice::ExeGroup | QFileDevice::ExeOther);
		return destination;

#elif defined(Q_OS_WIN)
		QFileInfo targetInfo(target);
		if (!targetInfo.exists()) {
			qWarning() << "Shortcut target does not exist:" << target;
			return QString();
		}
		target = targetInfo.absoluteFilePath();
		if (target.length() >= MAX_PATH || iconPath.length() >= MAX_PATH) {
			qWarning() << "Shortcut target or icon path is too long for the "
						  "shell to accept.";
			return QString();
		}
		if (!destination.endsWith(QLatin1String(".lnk"), Qt::CaseInsensitive)) {
			destination += QLatin1String(".lnk");
		}

		/* Only balance CoUninitialize() against an initialise that actually
		 * happened here: the GUI thread is usually already in an apartment,
		 * and tearing that down underneath it would be rude. */
		const HRESULT init = CoInitialize(nullptr);
		const bool weInitialised = SUCCEEDED(init);

		IShellLinkW* link = nullptr;
		HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr,
									  CLSCTX_INPROC_SERVER, IID_IShellLinkW,
									  reinterpret_cast<void**>(&link));
		if (FAILED(hr)) {
			if (weInitialised) {
				CoUninitialize();
			}
			qWarning() << "Could not reach the shell to build a link.";
			return QString();
		}

		const QString nativeTarget = QDir::toNativeSeparators(target);
		const QString nativeDir =
			QDir::toNativeSeparators(targetInfo.absolutePath());
		const QString argLine = quoteArgs(args, QStringLiteral("\""),
										  QStringLiteral("\\\""), true);

		link->SetPath(reinterpret_cast<LPCWSTR>(nativeTarget.utf16()));
		link->SetWorkingDirectory(reinterpret_cast<LPCWSTR>(nativeDir.utf16()));
		link->SetDescription(reinterpret_cast<LPCWSTR>(name.utf16()));
		link->SetArguments(reinterpret_cast<LPCWSTR>(argLine.utf16()));
		if (!iconPath.isEmpty()) {
			// The shell wants a native path; Qt hands out '/' separators.
			const QString nativeIcon = QDir::toNativeSeparators(iconPath);
			link->SetIconLocation(reinterpret_cast<LPCWSTR>(nativeIcon.utf16()),
								  0);
		}

		IPersistFile* file = nullptr;
		hr = link->QueryInterface(IID_IPersistFile,
								  reinterpret_cast<void**>(&file));
		if (SUCCEEDED(hr)) {
			const QString nativeDest = QDir::toNativeSeparators(destination);
			hr = file->Save(reinterpret_cast<LPCOLESTR>(nativeDest.utf16()),
							TRUE);
			file->Release();
		}
		link->Release();
		if (weInitialised) {
			CoUninitialize();
		}

		if (FAILED(hr)) {
			qWarning() << "The shell refused to save a link at" << destination;
			return QString();
		}
		return destination;

#else
		(void)target;
		(void)args;
		(void)name;
		(void)iconPath;
		qWarning() << "Shortcuts are not supported on this platform.";
		return QString();
#endif
	}
} // namespace FS
