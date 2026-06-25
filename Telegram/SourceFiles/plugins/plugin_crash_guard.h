#pragma once

// ═══════════════════════════════════════════════════════════════════════════
// plugin_crash_guard.h
//
// Installs a platform-level crash handler that detects whether a crash
// originates inside a loaded plugin shared library and, if so, writes a
// "last_fault_plugin" file before the process dies.  On the *next* launch
// PluginManager reads this file and enters safe mode automatically.
//
// Usage:
//   // In Application::create() or similar, after PluginManager::init():
//   Plugins::CrashGuard::install();
//
// ═══════════════════════════════════════════════════════════════════════════

#include <QtCore/QString>

namespace Plugins {

class CrashGuard {
public:
	// Install the signal/SEH handler.  Safe to call multiple times.
	static void install();

	// Return the id of the plugin that caused the last crash (read from
	// the sentinel file).  Empty if no crash was recorded.
	static QString lastFaultedPlugin();

	// Clear the sentinel file after it has been handled.
	static void clearLastFault();

	// Path of the sentinel file (inside the plugin dir).
	static QString sentinelPath();

private:
	CrashGuard() = delete;
};

} // namespace Plugins
