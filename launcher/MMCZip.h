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

#pragma once

#include <QString>
#include <QFileInfo>
#include <QFileInfoList>
#include <QDir>
#include <QSet>
#include <QDateTime>
#include "minecraft/mod/Mod.h"
#include <functional>
#include <optional>

struct archive;

namespace MMCZip
{
	using FilterFunction = std::function<bool(const QString&)>;

	/**
	 * Same question as FilterFunction - "leave this one out?" - asked
	 * about a file rather than about a path.
	 *
	 * The export side works from a list of QFileInfo it has already
	 * walked, and the models that decide what to exclude (see
	 * FileIgnoreProxy) need more than the name: whether an entry is a
	 * symlink, where it actually sits, how big it is. Reconstructing a
	 * QFileInfo from a relative path inside the filter would mean the
	 * caller stat()s every file twice.
	 */
	using FilterFileFunction = std::function<bool(const QFileInfo&)>;

	/**
	 * Merge two zip files, using a filter function.
	 * Reads entries from 'from' and writes them into the zip at 'intoPath'.
	 * 'contained' tracks already-added filenames to avoid duplicates.
	 */
	bool mergeZipFiles(const QString& intoPath, QFileInfo from,
					   QSet<QString>& contained,
					   const FilterFunction filter = nullptr);

	/**
	 * Take a source jar, add mods to it, resulting in target jar.
	 */
	bool createModdedJar(QString sourceJarPath, QString targetJarPath,
						 const QList<Mod>& mods);

	/**
	 * Find a single file in archive by file name (not path).
	 * \return the path prefix where the file is
	 */
	QString findFolderOfFileInZip(const QString& zipPath, const QString& what,
								  const QString& root = QString(""));

	/**
	 * Find multiple files of the same name in archive by file name.
	 * If a file is found in a path, no deeper paths are searched.
	 * \return true if anything was found
	 */
	bool findFilesInZip(const QString& zipPath, const QString& what,
						QStringList& result, const QString& root = QString());

	/**
	 * Reports how far through a long running archive operation we are.
	 * Called with (files done, files in total) — total is 0 when nothing
	 * matched the filter.
	 */
	using ProgressFunction = std::function<void(qint64, qint64)>;

	/**
	 * Compress a directory into a zip, using a filter function to exclude
	 * entries.
	 *
	 * \param progress Optional. When given, the directory is walked once
	 * up front to count the entries that will actually be written, so the
	 * callback can report a real percentage rather than a spinner. The
	 * callback runs on the calling thread, once before the first file and
	 * once per file written.
	 */
	bool compressDir(QString zipFile, QString dir,
					 FilterFunction excludeFilter,
					 ProgressFunction progress = nullptr);

	/**
	 * Walk `rootDir` and collect every file into `files`, skipping what
	 * `excludeFilter` rejects.
	 *
	 * The counterpart of compressDir() for callers that want to know what
	 * they are about to write before they start writing it - a progress
	 * bar with a real total, a manifest that has to list the same files
	 * the archive will carry, an export that leaves some of them out
	 * because they are being downloaded instead.
	 *
	 * \param subDir Recursion state. Pass a null QString at the top.
	 * \return false when a directory could not be read, which means the
	 * collected list is incomplete and must not be treated as the whole
	 * tree.
	 */
	bool collectFileListRecursively(const QString& rootDir,
									const QString& subDir,
									QFileInfoList* files,
									FilterFileFunction excludeFilter);

	/**
	 * A zip being written to, one entry at a time.
	 *
	 * compressDir() is the whole job in one call, which is all a backup
	 * needs. An export is not that: it writes a generated manifest
	 * beside files taken from disk, in an order the caller decides, and
	 * it has to be able to stop in the middle when the user says so.
	 * None of that fits behind a single call, so the archive is held
	 * open here instead.
	 *
	 * File contents are streamed in fixed-size blocks rather than read
	 * whole, because an instance's worth of files includes worlds and
	 * resource packs that are gigabytes on their own.
	 *
	 * Not copyable: two owners of one archive handle would close it
	 * twice.
	 */
	class ZipWriter
	{
	  public:
		explicit ZipWriter(QString path);
		~ZipWriter();

		ZipWriter(const ZipWriter&) = delete;
		ZipWriter& operator=(const ZipWriter&) = delete;

		bool open();

		/**
		 * Finish the archive. Safe to call twice; the destructor calls it
		 * for the paths that bail out early.
		 */
		bool close();

		/** Add a file from disk, stored in the archive as `entryName`. */
		bool addFile(const QString& sourcePath, const QString& entryName);

		/** Add `data` as `entryName`. For generated manifests. */
		bool addFile(const QString& entryName, const QByteArray& data);

		/** Human readable reason the last failed call failed. */
		QString errorString() const
		{
			return m_error;
		}

	  private:
		QString m_path;
		struct archive* m_archive = nullptr;
		QString m_error;
	};

	/**
	 * What a long-running extraction reports back, and how it is asked
	 * to stop.
	 *
	 * Every member is optional. `progress` is called once before the
	 * first entry and once per entry written; asking for it costs one
	 * extra pass over the archive's entry list up front, which is what
	 * makes a total - and therefore a percentage - possible at all.
	 *
	 * `isCancelled` is polled between entries. When it answers true the
	 * extraction stops, removes what it has already written and returns
	 * nullopt, exactly as it would for a damaged archive: the caller
	 * knows which of the two happened because it owns the flag.
	 */
	struct ExtractReporting {
		ProgressFunction progress = nullptr;
		std::function<bool()> isCancelled = nullptr;
		std::function<void(const QString&)> entryStarted = nullptr;
	};

	/**
	 * Extract a subdirectory from an archive.
	 */
	std::optional<QStringList> extractSubDir(const QString& zipPath,
												const QString& subdir,
												const QString& target);

	/**
	 * Extract a subdirectory from an archive, reporting progress and
	 * honouring cancellation. See ExtractReporting.
	 */
	std::optional<QStringList> extractSubDir(
		const QString& zipPath, const QString& subdir, const QString& target,
		const ExtractReporting& reporting);

	/**
	 * Extract a single file relative to the zip root.
	 *
	 * \param error If non-null, receives a human readable reason on failure.
	 * Worth passing: the underlying libarchive diagnostics ("Unsupported ZIP
	 * compression method (8: deflation)" and friends) are otherwise only
	 * visible in qWarning output, which turns a build/packaging mistake into
	 * an unexplained "extraction failed".
	 */
	bool extractRelFile(const QString& zipPath, const QString& file,
						const QString& target, QString* error = nullptr);

	/**
	 * Extract a whole archive.
	 *
	 * \param fileCompressed The name of the archive.
	 * \param dir The directory to extract to, the current directory if left
	 * empty.
	 * \return The list of the full paths of the files extracted, empty on
	 * failure.
	 */
	std::optional<QStringList> extractDir(QString fileCompressed,
											 QString dir);

	/**
	 * Extract a subdirectory from an archive.
	 *
	 * \param fileCompressed The name of the archive.
	 * \param subdir The directory within the archive to extract
	 * \param dir The directory to extract to, the current directory if left
	 * empty.
	 * \return The list of the full paths of the files extracted, empty on
	 * failure.
	 */
	std::optional<QStringList> extractDir(QString fileCompressed,
											 QString subdir, QString dir);

	/**
	 * Extract a single file from an archive into a directory.
	 *
	 * \param fileCompressed The name of the archive.
	 * \param file The file within the archive to extract
	 * \param dir The directory to extract to, the current directory if left
	 * empty.
	 * \return true for success or false for failure
	 */
	bool extractFile(QString fileCompressed, QString file, QString dir);

	/**
	 * Read a file's contents from inside a zip archive.
	 * \return the file data, or empty QByteArray on failure
	 */
	QByteArray readFileFromZip(const QString& zipPath,
							   const QString& entryName);

	/**
	 * Check if a given entry path exists in a zip archive.
	 */
	bool entryExists(const QString& zipPath, const QString& entryName);

	/**
	 * List all entry names in a zip archive.
	 */
	QStringList listEntries(const QString& zipPath);

	/**
	 * List entries under a specific directory in a zip archive.
	 * \param type QDir::Files, QDir::Dirs, or both
	 */
	QStringList listEntries(const QString& zipPath, const QString& dirPath,
							QDir::Filters type = QDir::Files | QDir::Dirs);

	/**
	 * Get the modification time of a specific entry in a zip archive.
	 */
	QDateTime getEntryModTime(const QString& zipPath, const QString& entryName);
} // namespace MMCZip
