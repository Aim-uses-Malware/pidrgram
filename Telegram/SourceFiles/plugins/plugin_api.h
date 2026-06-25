#pragma once

// ═══════════════════════════════════════════════════════════════════════════
// plugin_api.h  —  Public API surface for pidrgram / materialgram plugins
//
// Include this header (and nothing else from the host) in your plugin.
// It is intentionally self-contained so that plugins can be compiled
// independently from the host.
// ═══════════════════════════════════════════════════════════════════════════

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <functional>

// ──────────────────────────────────────────────────────────────────────────
// Forward declarations (opaque to plugins – do NOT dereference directly)
// ──────────────────────────────────────────────────────────────────────────
namespace Main    { class Session; }
class HistoryItem;

namespace Plugins {

// ──────────────────────────────────────────────────────────────────────────
// PluginMeta  (same layout as in plugin_manager.h – keep in sync!)
// ──────────────────────────────────────────────────────────────────────────
struct PluginMeta {
	QString id;
	QString name;
	QString version;
	QString author;
	QString description;
	QString entryPoint;
	bool    enabled = true;
};

// ──────────────────────────────────────────────────────────────────────────
// IHostApi
//
// Injected into every plugin by the host via onLoad().
// Gives plugins a stable, versioned surface to call back into Telegram.
// Never store a raw Main::Session* – always go through this interface.
// ──────────────────────────────────────────────────────────────────────────
class IHostApi {
public:
	virtual ~IHostApi() = default;

	// ── API version ──────────────────────────────────────────────────────
	// Increment MAJOR on breaking changes, MINOR on additions.
	static constexpr int kApiMajor = 1;
	static constexpr int kApiMinor = 0;

	virtual int apiMajor() const = 0;
	virtual int apiMinor() const = 0;

	// ── Messaging ────────────────────────────────────────────────────────
	// Send a plain-text message to a peer identified by its numeric id.
	// peerId is the bare user/chat/channel id (no access_hash needed –
	// the host resolves it from the current session).
	virtual void sendMessage(qint64 peerId, const QString &text) = 0;

	// ── Notifications ────────────────────────────────────────────────────
	// Show a transient toast / system notification visible only to the
	// local user (never sent over the wire).
	virtual void showToast(const QString &text) = 0;

	// ── Settings storage ─────────────────────────────────────────────────
	// Isolated key-value store per plugin (keyed by plugin id internally).
	virtual void  setSetting(const QString &key, const QJsonValue &value) = 0;
	virtual QJsonValue getSetting(const QString &key,
	                              const QJsonValue &defaultValue = {}) const = 0;

	// ── UI callbacks ─────────────────────────────────────────────────────
	// Ask the host to open an arbitrary Telegram box.  The callable
	// receives a raw object_ptr<Ui::BoxContent> factory; the plugin is
	// responsible for providing it.  Provided as a std::function so
	// plugins don't need to link against tdesktop UI directly.
	using BoxFactory = std::function<void * /*object_ptr<BoxContent>*/ ()>;
	virtual void openBox(BoxFactory factory) = 0;

	// ── Session info ─────────────────────────────────────────────────────
	// Stable numeric ids for the logged-in user.
	virtual qint64 selfUserId()    const = 0;
	virtual QString selfUsername() const = 0;

	// ── Logging ──────────────────────────────────────────────────────────
	// Writes to the same log sink as the host (useful for debugging).
	virtual void log(const QString &message) = 0;
};

// ──────────────────────────────────────────────────────────────────────────
// PluginBase
//
// Every plugin must inherit from this class exactly once and export the
// two C symbols via PLUGIN_DECLARE (see below).
// ──────────────────────────────────────────────────────────────────────────
class PluginBase {
public:
	virtual ~PluginBase() = default;

	// ── Lifecycle ────────────────────────────────────────────────────────
	// Called once after the shared library is mapped and the session is
	// ready.  `api` remains valid until onUnload() returns.
	virtual void onLoad(IHostApi *api) = 0;

	// Called before dlclose().  Release all resources here.
	virtual void onUnload() = 0;

	// ── Hooks ────────────────────────────────────────────────────────────
	// Return false from onUpdate to drop the update (prevent host processing).
	// item in onNewMessage is always non-null.
	virtual bool onUpdate(const void *rawMTPUpdates) { return true; }
	virtual void onNewMessage(HistoryItem *item)      {}

	// ── Identity ─────────────────────────────────────────────────────────
	virtual const PluginMeta &meta() const = 0;
};

// ──────────────────────────────────────────────────────────────────────────
// PLUGIN_DECLARE
//
// Place this macro once in your plugin's .cpp file.
//
// Example:
//
//   PLUGIN_DECLARE(
//       MyPlugin,                          // C++ class name
//       "com.example.myplugin",            // unique reverse-DNS id
//       "My Plugin",                       // display name
//       "1.0.0",                           // semver
//       "Jane Doe <jane@example.com>",     // author
//       "Does something awesome"           // short description
//   )
// ──────────────────────────────────────────────────────────────────────────
#define PLUGIN_DECLARE(ClassName, Id, Name, Ver, Author, Desc)                \
static const ::Plugins::PluginMeta _sMeta_##ClassName {                       \
    Id, Name, Ver, Author, Desc, ""                                           \
};                                                                            \
const ::Plugins::PluginMeta &ClassName::meta() const {                        \
    return _sMeta_##ClassName;                                                \
}                                                                             \
extern "C" ::Plugins::PluginBase *pidrgram_plugin_create() {                  \
    return new ClassName();                                                   \
}                                                                             \
extern "C" void pidrgram_plugin_destroy(::Plugins::PluginBase *p) {           \
    delete p;                                                                 \
}

} // namespace Plugins
