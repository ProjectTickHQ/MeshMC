/* SPDX-FileCopyrightText: 2026 Project Tick
 * SPDX-License-Identifier: Apache-2.0
 *
 * OfflineWikiPlugin — MMCO entry point. Clones the official MeshMC wiki
 * into <plugin_data>/ in the background and exposes it as a read-only
 * global settings page. The wiki is the plugin's only content source:
 * there is no bundle management and no ZIM support.
 */

#include "plugin/sdk/mmco_cxx_sdk.hpp"
#include "WikiRepoBundle.h"
#include "WikiPage.h"

#include <QPointer>
#include <QProcess>
#include <QStandardPaths>

MMCO_DEFINE_MODULE("OfflineWiki", "1.0.0", "Project Tick",
				   "Offline, read-only viewer for the MeshMC wiki",
				   "Apache-2.0");

namespace
{
	MMCOContext* g_ctx = nullptr;

	/* The single wiki bundle. Null until the clone has produced a usable
	 * checkout; WikiPage holds &g_wiki so it sees updates live. */
	WikiRepoBundle* g_wiki = nullptr;

	/* Open WikiPages register here so a background clone/pull can refresh
	 * them when it finishes. QPointers null out when a page is destroyed. */
	QList<QPointer<WikiPage>> g_openPages;

	/* Guards against launching a second background sync while one is
	 * already running (e.g. the settings dialog reopened mid-clone). */
	bool g_wikiSyncRunning = false;

	/* The official MeshMC wiki, served offline. Cloned on first run and
	 * fast-forwarded on every later start so the local copy tracks
	 * upstream. */
	constexpr const char kMeshmcWikiUrl[] =
		"https://github.com/Project-Tick/MeshMC.wiki.git";
	constexpr const char kMeshmcWikiDirName[] = "meshmc-wiki";

	QString pluginDataDir()
	{
		if (!g_ctx)
			return {};
		return QString::fromUtf8(
			g_ctx->fs_plugin_data_dir(g_ctx->module_handle));
	}

	QString wikiPath()
	{
		return QDir(pluginDataDir()).filePath(kMeshmcWikiDirName);
	}

	QString gitExecutable()
	{
		return QStandardPaths::findExecutable(QStringLiteral("git"));
	}

	void refreshOpenPages()
	{
		for (auto& p : g_openPages)
			if (p)
				p->refreshBundle();
	}

	/* Try to (re)open the on-disk wiki checkout into g_wiki. Returns true
	 * when a usable wiki is open afterwards. */
	bool openWiki()
	{
		const QString path = wikiPath();
		if (!QFileInfo::exists(QDir(path).filePath(QStringLiteral(".git"))))
			return g_wiki != nullptr;

		auto* w = new WikiRepoBundle();
		if (!w->open(path)) {
			delete w;
			return g_wiki != nullptr;
		}
		delete g_wiki;
		g_wiki = w;
		return true;
	}

	/* Kick off the MeshMC wiki clone/update *in the background* so boot
	 * is never blocked on git or the network. The launcher event loop is
	 * already running when mmco_init is called, so the asynchronous
	 * QProcess completes on the GUI thread without any blocking wait.
	 *
	 * Offline behaviour:
	 *   • A previously cloned copy is opened synchronously at init, so the
	 *     wiki is fully usable offline. The background pull is best-effort
	 *     and silently leaves the cached copy in place when it fails.
	 *   • With no cached copy and no network, the clone simply fails; the
	 *     plugin stays healthy and retries on the next launch / when the
	 *     settings page is reopened. */
	void startMeshmcWikiSync()
	{
		if (g_wikiSyncRunning)
			return;

		const QString exe = gitExecutable();
		if (exe.isEmpty()) {
			MMCO_WARN(g_ctx, "OfflineWiki: git not found — the wiki cannot be "
							 "downloaded (install git and restart).");
			return;
		}

		const QString dir = pluginDataDir();
		QDir().mkpath(dir);
		const QString path = wikiPath();
		const bool haveCheckout =
			QFileInfo::exists(QDir(path).filePath(QStringLiteral(".git")));

		QStringList args;
		QString workingDir;
		if (haveCheckout) {
			workingDir = path;
			args << QStringLiteral("pull") << QStringLiteral("--ff-only")
				 << QStringLiteral("--quiet");
		} else {
			workingDir = dir;
			args << QStringLiteral("clone") << QStringLiteral("--depth")
				 << QStringLiteral("1") << QStringLiteral("--quiet")
				 << QString::fromUtf8(kMeshmcWikiUrl) << path;
		}

		auto* proc = new QProcess();
		proc->setWorkingDirectory(workingDir);
		auto env = QProcessEnvironment::systemEnvironment();
		env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
		env.insert(QStringLiteral("GIT_CONFIG_NOSYSTEM"), QStringLiteral("1"));
		proc->setProcessEnvironment(env);
		proc->setProgram(exe);
		proc->setArguments(args);

		g_wikiSyncRunning = true;
		MMCO_LOG(g_ctx, haveCheckout
							? "OfflineWiki: updating MeshMC wiki in background…"
							: "OfflineWiki: cloning MeshMC wiki in background…");

		QObject::connect(
			proc,
			QOverload<int, QProcess::ExitStatus>::of(qOverload<int, QProcess::ExitStatus>(&QProcess::finished)),
			proc, [proc, haveCheckout](int code, QProcess::ExitStatus) {
				g_wikiSyncRunning = false;
				if (!g_ctx) { // plugin unloaded mid-flight
					proc->deleteLater();
					return;
				}
				if (code != 0) {
					QByteArray err = proc->readAllStandardError();
					QByteArray msg =
						QByteArray(haveCheckout
									   ? "OfflineWiki: wiki update failed "
										 "(using cached copy if present): "
									   : "OfflineWiki: wiki clone failed (will "
										 "retry next start): ") +
						err;
					MMCO_WARN(g_ctx, msg.constData());
				} else {
					MMCO_LOG(g_ctx, haveCheckout
										? "OfflineWiki: MeshMC wiki updated."
										: "OfflineWiki: MeshMC wiki cloned.");
					// Open / re-open the checkout and refresh any open page.
					openWiki();
					refreshOpenPages();
				}
				proc->deleteLater();
			});

		proc->start();
	}
} // namespace

static int on_global_settings_pages(void*, uint32_t, void* payload, void*)
{
	auto* evt = static_cast<MMCOGlobalSettingsPagesEvent*>(payload);
	if (!evt || !evt->page_list_handle)
		return 0;
	auto* pages = static_cast<QList<BasePage*>*>(evt->page_list_handle);

	auto* page = new WikiPage(&g_wiki, !gitExecutable().isEmpty());
	pages->append(page);

	// Track the page so a background clone/pull can refresh its nav.
	// Drop dead entries opportunistically and forget this one on destroy.
	g_openPages.removeAll(QPointer<WikiPage>(nullptr));
	g_openPages.append(QPointer<WikiPage>(page));
	QObject::connect(page, &QObject::destroyed, page, [page]() {
		g_openPages.removeAll(QPointer<WikiPage>(page));
	});

	// If we still have no wiki (offline first run, or an earlier failed
	// clone), try again now that the user is looking at the page. Cheap
	// no-op when a sync is already running.
	if (!g_wiki)
		startMeshmcWikiSync();
	return 0;
}

extern "C" {

MMCO_EXPORT int mmco_init(MMCOContext* ctx)
{
	g_ctx = ctx;
	MMCO_LOG(ctx, "OfflineWiki initialising…");

	// Open a previously cloned wiki synchronously (local I/O) so it is
	// usable offline immediately. The network clone/update runs in the
	// background so boot is never blocked on git/network.
	openWiki();
	MMCO_LOG(ctx, g_wiki ? "OfflineWiki: MeshMC wiki ready (cached copy)."
						 : "OfflineWiki: no cached wiki yet.");

	startMeshmcWikiSync();

	ctx->hook_register(ctx->module_handle, MMCO_HOOK_UI_GLOBAL_SETTINGS_PAGES,
					   on_global_settings_pages, nullptr);

	MMCO_LOG(ctx, "OfflineWiki ready.");
	return 0;
}

MMCO_EXPORT void mmco_unload()
{
	if (g_ctx)
		MMCO_LOG(g_ctx, "OfflineWiki unloading.");
	// Clear g_ctx first so any in-flight background git callback becomes a
	// no-op (it checks g_ctx and only self-deletes its QProcess).
	g_ctx = nullptr;
	g_openPages.clear();
	delete g_wiki;
	g_wiki = nullptr;
}

} /* extern "C" */
