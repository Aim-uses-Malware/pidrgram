#pragma once

#include "plugins/plugin_api.h"

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QList>
#include <QtCore/QHash>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonArray>

// Forward declarations
namespace Main    { class Session; }
namespace Window  { class SessionController; }
class MTPupdates;
class HistoryItem;

namespace Plugins {

// ─────────────────────────────────────────────────────────────────────────────
// PluginMeta (extended – stays binary-compatible with plugin_api.h version)
// ─────────────────────────────────────────────────────────────────────────────
struct PluginMetaFull : PluginMeta {
	// extra host-side fields not exposed to plugins
	QString manifestDir; // absolute path to the folder containing plugin.json

	static PluginMetaFull fromJson(const QJsonObject &obj, const QString &dir = {});
	QJsonObject toJson() const;
};

// ─────────────────────────────────────────────────────────────────────────────
// Runtime state
// ─────────────────────────────────────────────────────────────────────────────
enum class PluginState {
	Unloaded,
	Loaded,
	Error,
	DisabledBySafeMode,
};

struct PluginEntry {
	PluginMetaFull meta;
	PluginState    state    = PluginState::Unloaded;
	QString        errorMsg;
};

// ─────────────────────────────────────────────────────────────────────────────
// PluginManager
// ─────────────────────────────────────────────────────────────────────────────
class PluginManager : public QObject {
	Q_OBJECT
public:
	static PluginManager &instance();

	// Call once after the main session is available.
	void init(Main::Session *session);
	// Call on application exit.
	void shutdown();

	// Override default plugin directory (must be called before init()).
	void setPluginDir(const QString &path);
	QString pluginDir() const;

	// Discovery + lifecycle
	void discoverAll();
	bool loadPlugin(const QString &pluginId);
	bool unloadPlugin(const QString &pluginId);
	void enablePlugin(const QString &pluginId, bool enable);

	// Safe mode
	[[nodiscard]] bool isSafeMode() const;
	[[nodiscard]] QString safeModeVictim() const;
	void enterSafeMode(const QString &faultingPluginId, const QString &reason);
	void exitSafeMode();

	// Queries
	[[nodiscard]] const QList<PluginEntry> &entries() const;
	[[nodiscard]] const PluginEntry *entryById(const QString &id) const;

	// MTProto dispatch hooks (called from patched tdesktop internals)
	bool dispatchUpdate(const MTPUpdates &updates);
	void dispatchNewMessage(HistoryItem *item);

Q_SIGNALS:
	void pluginLoaded(const QString &id);
	void pluginUnloaded(const QString &id);
	void pluginError(const QString &id, const QString &error);
	void safeModeEntered(const QString &faultingId);
	void safeModeExited();
	void entriesChanged();

private:
	explicit PluginManager(QObject *parent = nullptr);
	~PluginManager() override;

	PluginEntry *mutableEntryById(const QString &id);
	void persistState();
	void loadState();
	QString statePath() const;

	QList<PluginEntry>  _entries;
	QString             _pluginDir;
	Main::Session      *_session  = nullptr;
	bool                _safeMode = false;
	QString             _safeModeVictim;
};

} // namespace Plugins
