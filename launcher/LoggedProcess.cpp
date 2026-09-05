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

#include "LoggedProcess.h"
#include "MessageLevel.h"
#include <QDebug>

#ifdef Q_OS_UNIX
#include <signal.h>
#include <unistd.h>
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#endif

LoggedProcess::LoggedProcess(QObject* parent) : QProcess(parent)
{
	// QProcess has a strange interface... let's map a lot of those into a few.
	connect(this, &QProcess::readyReadStandardOutput, this,
			&LoggedProcess::on_stdOut);
	connect(this, &QProcess::readyReadStandardError, this,
			&LoggedProcess::on_stdErr);
	connect(this, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &LoggedProcess::on_exit);
	connect(this, &QProcess::errorOccurred, this, &LoggedProcess::on_error);
	connect(this, &QProcess::stateChanged, this,
			&LoggedProcess::on_stateChange);

#if defined(Q_OS_UNIX) && QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	// Create a new process group so we can kill the entire tree.
	// On Qt 5 this is done by overriding setupChildProcess() instead --
	// see below.
	setChildProcessModifier([]() { setsid(); });
#endif
}

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0) && defined(Q_OS_UNIX)
void LoggedProcess::setupChildProcess()
{
	// Runs in the forked child before exec(), same as the Qt 6 modifier
	// above. setsid() is async-signal-safe, which is the constraint here.
	setsid();
}
#endif

LoggedProcess::~LoggedProcess()
{
	if (m_is_detachable) {
		setProcessState(QProcess::NotRunning);
	}
#ifdef Q_OS_WIN
	// Closing the job handle does not kill anything, because the job was
	// created without JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE. That is deliberate:
	// see assignToJobObject().
	if (m_job) {
		CloseHandle(static_cast<HANDLE>(m_job));
		m_job = nullptr;
	}
#endif
}

QStringList reprocess(const QByteArray& data, QString& leftover)
{
	QString str = leftover + QString::fromLocal8Bit(data);

	str.remove('\r');
	QStringList lines = str.split("\n");
	leftover = lines.takeLast();
	return lines;
}

void LoggedProcess::on_stdErr()
{
	auto lines = reprocess(readAllStandardError(), m_err_leftover);
	emit log(lines, MessageLevel::StdErr);
}

void LoggedProcess::on_stdOut()
{
	auto lines = reprocess(readAllStandardOutput(), m_out_leftover);
	emit log(lines, MessageLevel::StdOut);
}

void LoggedProcess::on_exit(int exit_code, QProcess::ExitStatus status)
{
	// save the exit code
	m_exit_code = exit_code;

	// Flush console window
	if (!m_err_leftover.isEmpty()) {
		emit log({m_err_leftover}, MessageLevel::StdErr);
		m_err_leftover.clear();
	}
	if (!m_out_leftover.isEmpty()) {
		emit log({m_err_leftover}, MessageLevel::StdOut);
		m_out_leftover.clear();
	}

	// based on state, send signals
	if (!m_is_aborting) {
		if (status == QProcess::NormalExit) {
			//: Message displayed on instance exit
			emit log({tr("Process exited with code %1.").arg(exit_code)},
					 MessageLevel::MeshMC);
			changeState(LoggedProcess::Finished);
		} else {
			//: Message displayed on instance crashed
			if (exit_code == -1)
				emit log({tr("Process crashed.")}, MessageLevel::MeshMC);
			else
				emit log(
					{tr("Process crashed with exitcode %1.").arg(exit_code)},
					MessageLevel::MeshMC);
			changeState(LoggedProcess::Crashed);
		}
	} else {
		//: Message displayed after the instance exits due to kill request
		emit log({tr("Process was killed by user.")}, MessageLevel::Error);
		changeState(LoggedProcess::Aborted);
	}
}

void LoggedProcess::on_error(QProcess::ProcessError error)
{
	switch (error) {
		case QProcess::FailedToStart: {
			emit log({tr("The process failed to start.")}, MessageLevel::Fatal);
			changeState(LoggedProcess::FailedToStart);
			break;
		}
		// we'll just ignore those... never needed them
		case QProcess::Crashed:
		case QProcess::ReadError:
		case QProcess::Timedout:
		case QProcess::UnknownError:
		case QProcess::WriteError:
			break;
	}
}

void LoggedProcess::kill()
{
	m_is_aborting = true;
#ifdef Q_OS_UNIX
	// Kill the entire process group to ensure all child processes
	// (e.g. Java launched through a wrapper) are terminated
	auto pid = processId();
	if (pid > 0) {
		::kill(-pid, SIGKILL);
	}
#elif defined(Q_OS_WIN)
	// Terminating the job takes down every process in it, so a wrapper
	// command and the java process behind it both die. QProcess::kill() is
	// only the fallback: it reaches the direct child and nothing else.
	if (m_job) {
		if (TerminateJobObject(static_cast<HANDLE>(m_job), 1)) {
			return;
		}
		qWarning() << "TerminateJobObject failed with error"
				   << GetLastError()
				   << "- killing only the direct child instead";
	}
	QProcess::kill();
#else
	QProcess::kill();
#endif
}

#ifdef Q_OS_WIN
void LoggedProcess::assignToJobObject()
{
	if (m_job) {
		return;
	}
	auto pid = processId();
	if (pid <= 0) {
		// There is no pid before the process is actually running, which is
		// why this is called from the Running state change and not earlier.
		return;
	}

	HANDLE job = CreateJobObjectW(nullptr, nullptr);
	if (!job) {
		qWarning() << "Could not create a job object for process" << pid
				   << "- error" << GetLastError()
				   << "; killing it will not take down its children";
		return;
	}

	// NOTE: the job deliberately has no limits set on it. In particular it
	// must NOT get JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: setDetachable(true)
	// means the game is allowed to outlive the launcher, and that flag would
	// shoot it the moment the handle is closed. A limitless job is still
	// enough for TerminateJobObject().

	// Reopening the child by pid is safe here: QProcess keeps a handle to it,
	// and Windows does not recycle a pid while a handle to the process is
	// open.
	HANDLE process = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE,
								 static_cast<DWORD>(pid));
	if (!process) {
		qWarning() << "Could not open process" << pid << "- error"
				   << GetLastError();
		CloseHandle(job);
		return;
	}
	if (!AssignProcessToJobObject(job, process)) {
		qWarning() << "Could not assign process" << pid
				   << "to a job object - error" << GetLastError();
		CloseHandle(process);
		CloseHandle(job);
		return;
	}
	CloseHandle(process);
	m_job = job;

	// Known, unavoidable race: a grandchild spawned between CreateProcess and
	// the assignment above is not in the job. Closing that hole would need
	// the child to start suspended, and QProcess does not hand out the
	// PROCESS_INFORMATION required to resume its main thread.
}
#endif

int LoggedProcess::exitCode() const
{
	return m_exit_code;
}

void LoggedProcess::changeState(LoggedProcess::State state)
{
	if (state == m_state)
		return;
	m_state = state;
	emit stateChanged(m_state);
}

LoggedProcess::State LoggedProcess::state() const
{
	return m_state;
}

void LoggedProcess::on_stateChange(QProcess::ProcessState state)
{
	switch (state) {
		case QProcess::NotRunning:
			break; // let's not - there are too many that handle this already.
		case QProcess::Starting: {
			if (m_state != LoggedProcess::NotRunning) {
				qWarning() << "Wrong state change for process from state"
						   << m_state << "to" << (int)LoggedProcess::Starting;
			}
			changeState(LoggedProcess::Starting);
			return;
		}
		case QProcess::Running: {
			if (m_state != LoggedProcess::Starting) {
				qWarning() << "Wrong state change for process from state"
						   << m_state << "to" << (int)LoggedProcess::Running;
			}
#ifdef Q_OS_WIN
			// Before changeState(), so that anything reacting to Running -
			// including an immediate kill() - already sees the job.
			assignToJobObject();
#endif
			changeState(LoggedProcess::Running);
			return;
		}
	}
}

qint64 LoggedProcess::processId() const
{
	return QProcess::processId();
}

void LoggedProcess::setDetachable(bool detachable)
{
	m_is_detachable = detachable;
}
