#include "plugins/plugin_crash_guard.h"
#include "plugins/plugin_manager.h"

#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QStandardPaths>

#if defined(Q_OS_WIN)
#  include <windows.h>
#  include <DbgHelp.h>
#  pragma comment(lib, "DbgHelp.lib")
#else
#  include <signal.h>
#  include <execinfo.h>
#  include <unistd.h>
#  include <dlfcn.h>
#endif

namespace Plugins {

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────
static QString sentinelDir() {
	return QStandardPaths::writableLocation(
		QStandardPaths::AppDataLocation) + "/plugins";
}

QString CrashGuard::sentinelPath() {
	return sentinelDir() + "/last_fault_plugin.txt";
}

static void writeSentinel(const QString &pluginId) {
	QFile f(CrashGuard::sentinelPath());
	if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		f.write(pluginId.toUtf8());
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Try to identify which loaded plugin's address range contains `addr`.
// Returns empty string when no match.
// ─────────────────────────────────────────────────────────────────────────────
static QString pluginIdForAddress(void *addr) {
#if defined(Q_OS_WIN)
	wchar_t modPath[MAX_PATH] = {};
	HMODULE hMod = nullptr;
	if (GetModuleHandleExW(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(addr),
		&hMod))
	{
		GetModuleFileNameW(hMod, modPath, MAX_PATH);
	}
	const QString path = QString::fromWCharArray(modPath);
#else
	Dl_info info;
	if (dladdr(addr, &info) == 0 || !info.dli_fname) return {};
	const QString path = QString::fromUtf8(info.dli_fname);
#endif

	if (path.isEmpty()) return {};

	// Walk the loaded plugins and match by entry point filename.
	const auto &entries = PluginManager::instance().entries();
	for (const auto &e : entries) {
		if (e.state == PluginState::Loaded &&
		    path.contains(e.meta.entryPoint))
		{
			return e.meta.id;
		}
	}
	return {};
}

// ─────────────────────────────────────────────────────────────────────────────
// Platform crash handlers
// ─────────────────────────────────────────────────────────────────────────────

#if defined(Q_OS_WIN)
static LONG WINAPI sehHandler(EXCEPTION_POINTERS *ep) {
	if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;

	void *faultAddr = ep->ExceptionRecord->ExceptionAddress;
	const QString id = pluginIdForAddress(faultAddr);
	if (!id.isEmpty()) {
		writeSentinel(id);
		// Don't call PluginManager here – the heap may be corrupt.
		// Safe mode will be activated on next launch via lastFaultedPlugin().
	}
	return EXCEPTION_CONTINUE_SEARCH; // let the default handler deal with it
}
#else
static struct sigaction s_prevSigSegv;
static struct sigaction s_prevSigAbrt;
static struct sigaction s_prevSigBus;
static struct sigaction s_prevSigFpe;

static void posixSignalHandler(int sig, siginfo_t *info, void *ctx) {
	if (info) {
		void *faultAddr = (sig == SIGSEGV || sig == SIGBUS)
			? info->si_addr
			: nullptr;
		if (faultAddr) {
			const QString id = pluginIdForAddress(faultAddr);
			if (!id.isEmpty()) writeSentinel(id);
		}
	}
	// Re-raise to the previous handler (tdesktop crash reporter, etc.)
	struct sigaction *prev = nullptr;
	if (sig == SIGSEGV) prev = &s_prevSigSegv;
	else if (sig == SIGABRT) prev = &s_prevSigAbrt;
	else if (sig == SIGBUS)  prev = &s_prevSigBus;
	else if (sig == SIGFPE)  prev = &s_prevSigFpe;
	if (prev && prev->sa_sigaction) {
		prev->sa_sigaction(sig, info, ctx);
	} else {
		signal(sig, SIG_DFL);
		raise(sig);
	}
}
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────
static bool s_installed = false;

void CrashGuard::install() {
	if (s_installed) return;
	s_installed = true;

	QDir().mkpath(sentinelDir());

#if defined(Q_OS_WIN)
	AddVectoredExceptionHandler(0 /*last*/, sehHandler);
#else
	struct sigaction sa {};
	sa.sa_sigaction = posixSignalHandler;
	sa.sa_flags     = SA_SIGINFO | SA_RESTART;
	sigemptyset(&sa.sa_mask);

	sigaction(SIGSEGV, &sa, &s_prevSigSegv);
	sigaction(SIGABRT, &sa, &s_prevSigAbrt);
	sigaction(SIGBUS,  &sa, &s_prevSigBus);
	sigaction(SIGFPE,  &sa, &s_prevSigFpe);
#endif
}

QString CrashGuard::lastFaultedPlugin() {
	QFile f(sentinelPath());
	if (!f.open(QIODevice::ReadOnly)) return {};
	return QString::fromUtf8(f.readAll()).trimmed();
}

void CrashGuard::clearLastFault() {
	QFile::remove(sentinelPath());
}

} // namespace Plugins
