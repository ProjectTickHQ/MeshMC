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

#include "Sys.h"

#include <QStringList>
#include <QMap>
#include <QSettings>
#include <QFile>
#include <QProcess>
#include <QDebug>
#include <QDir>
#include <QRegularExpression>
#include <QString>

#include <functional>
#include <fstream>
#include <limits>

#ifdef Q_OS_WINDOWS
#include <QOperatingSystemVersion>
#include <windows.h>
#elif defined(Q_OS_UNIX)
#include <sys/utsname.h>
#endif

Sys::KernelInfo Sys::getKernelInfo()
{
	Sys::KernelInfo out;
#ifdef Q_OS_UNIX
	struct utsname buf;
	uname(&buf);
    out.kernelName = buf.sysname;
	QString release = out.kernelVersion = buf.release;
	out.kernelMajor = 0;
	out.kernelMinor = 0;
	out.kernelPatch = 0;
#ifdef Q_OS_MACOS
	out.kernelType = KernelType::Darwin;
	out.isCursed = false;
#elif defined(Q_OS_LINUX)
	out.kernelType = KernelType::Linux;
	out.isCursed = release.contains("WSL", Qt::CaseInsensitive) ||
				   release.contains("Microsoft", Qt::CaseInsensitive);
#endif
	auto sections = release.split('-');
	if (sections.size() >= 1) {
		auto versionParts = sections[0].split('.');
		if (versionParts.size() >= 3) {
			out.kernelMajor = versionParts[0].toInt();
			out.kernelMinor = versionParts[1].toInt();
			out.kernelPatch = versionParts[2].toInt();
		} else {
			qWarning() << "Not enough version numbers in " << sections[0]
					   << " found " << versionParts.size();
		}
	} else {
		qWarning() << "Not enough '-' sections in " << release << " found "
				   << sections.size();
	}
	return out;
#elif defined(Q_OS_WINDOWS)
	out.kernelType = KernelType::Windows;
	out.kernelName = "Windows";
	const auto osVersion = QOperatingSystemVersion::current();
	out.kernelMajor = osVersion.majorVersion();
	out.kernelMinor = osVersion.minorVersion();
	out.kernelPatch = osVersion.microVersion();
	out.kernelVersion = QString("%1.%2.%3")
							.arg(out.kernelMajor)
							.arg(out.kernelMinor)
							.arg(out.kernelPatch);
	return out;
#endif
}

#ifdef Q_OS_MACOS
#include <sys/sysctl.h>
#endif

uint64_t Sys::getSystemRam()
{
#ifdef Q_OS_WINDOWS
	MEMORYSTATUSEX status;
	status.dwLength = sizeof(status);
	GlobalMemoryStatusEx(&status);
	// bytes
	return (uint64_t)status.ullTotalPhys;
#elif defined(Q_OS_UNIX)
	std::string token;
#ifdef Q_OS_LINUX
	std::ifstream file("/proc/meminfo");
	while (file >> token) {
		if (token == "MemTotal:") {
			uint64_t mem;
			if (file >> mem) {
				return mem * 1024ull;
			} else {
				return 0;
			}
		}
		// ignore rest of the line
		file.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
	}
#elif defined(Q_OS_FREEBSD)
	char buff[512];
	FILE* fp = popen("sysctl hw.physmem", "r");
	if (fp != NULL) {
		while (fgets(buff, 512, fp) != NULL) {
			std::string str(buff);
			uint64_t mem = std::stoull(str.substr(12, std::string::npos));
			return mem * 1024ull;
		}
	}
#elif defined(Q_OS_MACOS)
	uint64_t memsize;
	size_t memsizesize = sizeof(memsize);
	if (!sysctlbyname("hw.memsize", &memsize, &memsizesize, NULL, 0)) {
		return memsize;
	} else {
		return 0;
	}
#endif
#endif
	return 0; // nothing found
}

bool Sys::isCPU64bit()
{
#ifdef Q_OS_MACOS
	// not even going to pretend I'm going to support anything else
	return true;
#elif defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
	return isSystem64bit();
#elif defined(Q_OS_WINDOWS)
	SYSTEM_INFO info;
	ZeroMemory(&info, sizeof(SYSTEM_INFO));
	GetNativeSystemInfo(&info);
	auto arch = info.wProcessorArchitecture;
	return arch == PROCESSOR_ARCHITECTURE_AMD64 ||
		   arch == PROCESSOR_ARCHITECTURE_IA64;
#endif
}

bool Sys::isSystem64bit()
{
#ifdef Q_OS_MACOS
	// yep. maybe when we have 128bit CPUs on consumer devices.
	return true;
#elif defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
	// kernel build arch on linux
	return QSysInfo::currentCpuArchitecture() == "x86_64";
#elif defined(_WIN64)
	return true;
#elif defined(_WIN32)
	BOOL f64 = false;
	return IsWow64Process(GetCurrentProcess(), &f64) && f64;
#else
	// it's some other kind of system...
	return false;
#endif
}

// The code from this point onwards is licensed under the
// MIT Expat license. See the COPYING.md file, # lionshed section.

Sys::DistributionInfo Sys::read_os_release()
{
	Sys::DistributionInfo out;
	QStringList files = {"/etc/os-release", "/usr/lib/os-release"};
	QString name;
	QString version;
	for (auto& file : files) {
		if (!QFile::exists(file)) {
			continue;
		}
		QSettings settings(file, QSettings::IniFormat);
		if (settings.contains("ID")) {
			name = settings.value("ID").toString().toLower();
		} else if (settings.contains("NAME")) {
			name = settings.value("NAME").toString().toLower();
		} else {
			continue;
		}

		if (settings.contains("VERSION_ID")) {
			version = settings.value("VERSION_ID").toString().toLower();
		} else if (settings.contains("VERSION")) {
			version = settings.value("VERSION").toString().toLower();
		}
		break;
	}
	if (name.isEmpty()) {
		return out;
	}
	out.distributionName = name;
	out.distributionVersion = version;
	return out;
}

bool Sys::main_lsb_info(Sys::LsbInfo& out)
{
	int status = 0;
	QProcess lsbProcess;
	lsbProcess.start("lsb_release -a");
	lsbProcess.waitForFinished();
	status = lsbProcess.exitStatus();
	QString output = lsbProcess.readAllStandardOutput();
	qDebug() << output;
	lsbProcess.close();
	if (status == 0) {
		auto lines = output.split('\n');
		for (auto line : lines) {
			int index = line.indexOf(':');
			auto key = line.left(index).trimmed();
			auto value = line.mid(index + 1).toLower().trimmed();
			if (key == "Distributor ID")
				out.distributor = value;
			else if (key == "Release")
				out.version = value;
			else if (key == "Description")
				out.description = value;
			else if (key == "Codename")
				out.codename = value;
		}
		return !out.distributor.isEmpty();
	}
	return false;
}

bool Sys::fallback_lsb_info(Sys::LsbInfo& out)
{
	// running lsb_release failed, try to read the file instead
	// /etc/lsb-release format, if the file even exists, is non-standard.
	// Only the `lsb_release` command is specified by LSB. Nonetheless, some
	// distributions install an /etc/lsb-release as part of the base
	// distribution, but `lsb_release` remains optional.
	QString file = "/etc/lsb-release";
	if (QFile::exists(file)) {
		QSettings settings(file, QSettings::IniFormat);
		if (settings.contains("DISTRIB_ID")) {
			out.distributor = settings.value("DISTRIB_ID").toString().toLower();
		}
		if (settings.contains("DISTRIB_RELEASE")) {
			out.version =
				settings.value("DISTRIB_RELEASE").toString().toLower();
		}
		return !out.distributor.isEmpty();
	}
	return false;
}

void Sys::lsb_postprocess(Sys::LsbInfo& lsb, Sys::DistributionInfo& out)
{
	QString dist = lsb.distributor;
	QString vers = lsb.version;
	if (dist.startsWith("redhatenterprise")) {
		dist = "rhel";
	} else if (dist == "archlinux") {
		dist = "arch";
	} else if (dist.startsWith("suse")) {
		if (lsb.description.startsWith("opensuse")) {
			dist = "opensuse";
		} else if (lsb.description.startsWith("suse linux enterprise")) {
			dist = "sles";
		}
	} else if (dist == "debian" && vers == "testing") {
		vers = lsb.codename;
	} else {
		// ubuntu, debian, gentoo, scientific, slackware, ... ?
		auto parts = dist.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
		if (parts.size()) {
			dist = parts[0];
		}
	}
	if (!dist.isEmpty()) {
		out.distributionName = dist;
		out.distributionVersion = vers;
	}
}

Sys::DistributionInfo Sys::read_lsb_release()
{
	LsbInfo lsb;
	if (!main_lsb_info(lsb)) {
		if (!fallback_lsb_info(lsb)) {
			return Sys::DistributionInfo();
		}
	}
	Sys::DistributionInfo out;
	lsb_postprocess(lsb, out);
	return out;
}

QString Sys::_extract_distribution(const QString& x)
{
	QString release = x.toLower();
	if (release.startsWith("red hat enterprise")) {
		return "rhel";
	}
	if (release.startsWith("suse linux enterprise")) {
		return "sles";
	}
	QStringList list =
		release.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
	if (list.size()) {
		return list[0];
	}
	return QString();
}

QString Sys::_extract_version(const QString& x)
{
	QRegularExpression versionish_string("\\d+(?:\\.\\d+)*$");
	QStringList list = x.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
	for (int i = list.size() - 1; i >= 0; --i) {
		QString chunk = list[i];
		if (versionish_string.match(chunk).hasMatch()) {
			return chunk;
		}
	}
	return QString();
}

Sys::DistributionInfo Sys::read_legacy_release()
{
	struct checkEntry {
		QString file;
		std::function<QString(const QString&)> extract_distro;
		std::function<QString(const QString&)> extract_version;
	};
	QList<checkEntry> checks = {
		{"/etc/arch-release", [](const QString&) { return "arch"; },
		 [](const QString&) { return "rolling"; }},
		{"/etc/slackware-version", &Sys::_extract_distribution,
		 &Sys::_extract_version},
		{QString(), &Sys::_extract_distribution, &Sys::_extract_version},
		{"/etc/debian_version", [](const QString&) { return "debian"; },
		 [](const QString& x) { return x; }},
	};
	for (auto& check : checks) {
		QStringList files;
		if (check.file.isNull()) {
			QDir etcDir("/etc");
			etcDir.setNameFilters({"*-release"});
			etcDir.setFilter(QDir::Files | QDir::NoDot | QDir::NoDotDot |
							 QDir::Readable | QDir::Hidden);
			files = etcDir.entryList();
		} else {
			files.append(check.file);
		}
		for (auto file : files) {
			QFile relfile(file);
			if (!relfile.open(QIODevice::ReadOnly | QIODevice::Text))
				continue;
			QString contents = QString::fromUtf8(relfile.readLine()).trimmed();
			QString dist = check.extract_distro(contents);
			QString vers = check.extract_version(contents);
			if (!dist.isEmpty()) {
				Sys::DistributionInfo out;
				out.distributionName = dist;
				out.distributionVersion = vers;
				return out;
			}
		}
	}
	return Sys::DistributionInfo();
}
