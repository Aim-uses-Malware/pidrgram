#include "plugins/plugin_manager.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include <QtCore/QStandardPaths>

// tdesktop internals we need
#include "core/application.h"
#include "main/main_session.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "api/api_sending.h"
#include "ui/toast/toast.h"
#include "base/debug_log.h"

#if defined(Q_OS_WIN)
#  include <windows.h>
#  define DL_OPEN(p)   (void *)LoadLibraryW(reinterpret_cast<const wchar_t *>((p).utf16()))
#  define DL_SYM(h,s)  GetProcAddress((HMODULE)(h), (s))
#  define DL_CLOSE(h)  FreeLibrary((HMODULE)(h))
static QString dlErrorString() {
	return QString("LoadLibrary failed (error %1)").arg(GetLastError());
}
#else
#  include <dlfcn.h>
#  define DL_OPEN(p)   dlopen((p).toLocal8Bit().constData(), RTLD_LAZY | RTLD_LOCAL)
#  define DL_SYM(h,s)  dlsym((h), (s))
#  define DL_CLOSE(h)  dlclose(h)
static QString dlErrorString() {
	const char *err = dlerror();
	return err ? QString::fromUtf8(err) : QString("unknown dl error");
}
#endif

namespace Plugins {

// ─────────────────────────────────────────────────────────────────────────────
// HostApiImpl  –  concrete IHostApi injected into every plugin
// ─────────────────────────────────────────────────────────────────────────────
class HostApiImpl final : public IHostApi {
public:
	explicit HostApiImpl(const QString &pluginId, Main::Session *session)
	: _pluginId(pluginId), _session(session) {}

	int apiMajor() const override { return kApiMajor; }
	int apiMinor() const override { return kApiMinor; }

	// ── Messaging ────────────────────────────────────────────────────────
	void sendMessage(qint64 peerId, const QString &text) override {
		if (!_session || text.isEmpty()) return;
		// Resolve the peer and send via the high-level API.
		const auto peer = _session->data().peerLoaded(PeerId(peerId));
		if (!peer) {
			LOG(("[Plugin:%1] sendMessage: peer %2 not found in cache")
				.arg(_pluginId).arg(peerId));
			return;
		}
		auto message = Api::MessageToSend(
			Api::SendAction(_session->data().history(peer)));
		message.textWithTags = { text, {} };
		_session->api().sendMessage(std::move(message));
	}

	// ── Notifications ────────────────────────────────────────────────────
	void showToast(const QString &text) override {
		Ui::Toast::Show(text);
	}

	// ── Settings storage ─────────────────────────────────────────────────
	void setSetting(const QString &key, const QJsonValue &value) override {
		_settings[key] = value;
		flushSettings();
	}

	QJsonValue getSetting(const QString &key,
	                      const QJsonValue &defaultValue) const override {
		auto it = _settings.find(key);
		return (it != _settings.end()) ? it.value() : defaultValue;
	}

	// ── UI callbacks ─────────────────────────────────────────────────────
	void openBox(BoxFactory /*factory*/) override {
		// TODO: integrate with Window::SessionController when available.
		// For now plugins can't open boxes without a controller reference.
		LOG(("[Plugin:%1] openBox() called – not yet wired to a controller")
			.arg(_pluginId));
	}

	// ── Session info ─────────────────────────────────────────────────────
	qint64 selfUserId() const override {
		return _session ? _session->userId().bare : 0;
	}

	QString selfUsername() const override {
		if (!_session) return {};
		return _session->user()->username();
	}

	// ── Logging ──────────────────────────────────────────────────────────
	void log(const QString &message) override {
		LOG(("[Plugin:%1] %2").arg(_pluginId, message));
	}

	// ── Settings persistence ─────────────────────────────────────────────
	void loadSettings(const QString &path) {
		_settingsPath = path;
		QFile f(path);
		if (!f.open(QIODevice::ReadOnly)) return;
		const auto doc = QJsonDocument::fromJson(f.readAll());
		if (doc.isObject()) {
			for (auto it = doc.object().begin(); it != doc.object().end(); ++it) {
				_settings[it.key()] = it.value();
			}
		}
	}

private:
	void flushSettings() {
		if (_settingsPath.isEmpty()) return;
		QJsonObject obj;
		for (auto it = _settings.begin(); it != _settings.end(); ++it) {
			obj[it.key()] = it.value();
		}
		QFile f(_settingsPath);
		if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			f.write(QJsonDocument(obj).toJson());
		}
	}

	QString              _pluginId;
	Main::Session       *_session     = nullptr;
	QString              _settingsPath;
	QHash<QString, QJsonValue> _settings;
};

// ─────────────────────────────────────────────────────────────────────────────
// PluginMetaFull
// ─────────────────────────────────────────────────────────────────────────────
PluginMetaFull PluginMetaFull::fromJson(const QJsonObject &obj,
                                        const QString &dir) {
	PluginMetaFull m;
	m.id          = obj["id"].toString();
	m.name        = obj["name"].toString(m.id);
	m.version     = obj["version"].toString("0.0.0");
	m.author      = obj["author"].toString("Unknown");
	m.description = obj["description"].toString();
	m.entryPoint  = obj["entryPoint"].toString();
	m.enabled     = obj["enabled"].toBool(true);
	m.manifestDir = dir;
	return m;
}

QJsonObject PluginMetaFull::toJson() const {
	return QJsonObject{
		{ "id",          id          },
		{ "name",        name        },
		{ "version",     version     },
		{ "author",      author      },
		{ "description", description },
		{ "entryPoint",  entryPoint  },
		{ "enabled",     enabled     },
	};
}

// ─────────────────────────────────────────────────────────────────────────────
// Native plugin record (one per loaded plugin)
// ─────────────────────────────────────────────────────────────────────────────
struct NativePlugin {
	void            *dlHandle = nullptr;
	PluginBase      *instance = nullptr;
	HostApiImpl     *api      = nullptr; // owned
};

// ─────────────────────────────────────────────────────────────────────────────
// PluginManager singleton
// ─────────────────────────────────────────────────────────────────────────────
PluginManager &PluginManager::instance() {
	static PluginManager inst;
	return inst;
}

PluginManager::PluginManager(QObject *parent) : QObject(parent) {}

PluginManager::~PluginManager() {
	shutdown();
}

void PluginManager::init(Main::Session *session) {
	_session = session;

	if (_pluginDir.isEmpty()) {
		_pluginDir = QStandardPaths::writableLocation(
			QStandardPaths::AppDataLocation) + "/plugins";
	}
	QDir().mkpath(_pluginDir);

	loadState();
	discoverAll();
}

void PluginManager::shutdown() {
	// unload in reverse discovery order for cleaner teardown
	QStringList ids;
	for (const auto &e : _entries) ids.prepend(e.meta.id);
	for (const auto &id : ids) {
		const auto *e = entryById(id);
		if (e && e->state == PluginState::Loaded) unloadPlugin(id);
	}
	persistState();
}

void PluginManager::setPluginDir(const QString &path) {
	_pluginDir = path;
}

QString PluginManager::pluginDir() const {
	return _pluginDir;
}

// ─────────────────────────────────────────────────────────────────────────────
// Discovery
// ─────────────────────────────────────────────────────────────────────────────
void PluginManager::discoverAll() {
	QDir dir(_pluginDir);
	const auto subdirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);

	for (const auto &sub : subdirs) {
		const QString manifestPath = sub.absoluteFilePath() + "/plugin.json";
		if (!QFile::exists(manifestPath)) continue;

		QFile f(manifestPath);
		if (!f.open(QIODevice::ReadOnly)) continue;

		const auto doc = QJsonDocument::fromJson(f.readAll());
		if (doc.isNull() || !doc.isObject()) continue;

		PluginMetaFull meta = PluginMetaFull::fromJson(
			doc.object(), sub.absoluteFilePath());
		if (meta.id.isEmpty()) continue;

		auto *existing = mutableEntryById(meta.id);
		if (existing) {
			// Preserve persisted enabled flag; update everything else from disk.
			const bool wasEnabled = existing->meta.enabled;
			existing->meta = meta;
			existing->meta.enabled = wasEnabled;
		} else {
			// Check if we have a persisted entry for this id (from loadState)
			// that carries the enabled flag but no manifestDir yet.
			// mutableEntryById already found nothing, so just append.
			PluginEntry entry;
			entry.meta = meta;
			entry.state = PluginState::Unloaded;
			_entries.append(entry);
		}

		const auto *e = entryById(meta.id);
		if (e && e->meta.enabled && !_safeMode) {
			loadPlugin(meta.id);
		}
	}
	Q_EMIT entriesChanged();
}

// ─────────────────────────────────────────────────────────────────────────────
// Loaded plugins registry
// ─────────────────────────────────────────────────────────────────────────────
static QHash<QString, NativePlugin> s_native; // pluginId → NativePlugin

// ─────────────────────────────────────────────────────────────────────────────
// loadPlugin
// ─────────────────────────────────────────────────────────────────────────────
bool PluginManager::loadPlugin(const QString &pluginId) {
	auto *e = mutableEntryById(pluginId);
	if (!e) return false;
	if (e->state == PluginState::Loaded) return true;
	if (_safeMode) {
		e->state = PluginState::DisabledBySafeMode;
		return false;
	}

	const QString libPath = e->meta.manifestDir + "/" + e->meta.entryPoint;
	if (e->meta.entryPoint.isEmpty() || !QFile::exists(libPath)) {
		e->state    = PluginState::Error;
		e->errorMsg = QString("Entry point not found: %1").arg(libPath);
		Q_EMIT pluginError(pluginId, e->errorMsg);
		return false;
	}

	void *dlHandle = DL_OPEN(libPath);
	if (!dlHandle) {
		e->state    = PluginState::Error;
		e->errorMsg = dlErrorString();
		Q_EMIT pluginError(pluginId, e->errorMsg);
		return false;
	}

	using CreateFn  = PluginBase *(*)();
	using DestroyFn = void(*)(PluginBase *);
	auto *createFn = reinterpret_cast<CreateFn>(
		DL_SYM(dlHandle, "pidrgram_plugin_create"));
	if (!createFn) {
		DL_CLOSE(dlHandle);
		e->state    = PluginState::Error;
		e->errorMsg = "Symbol 'pidrgram_plugin_create' not found";
		Q_EMIT pluginError(pluginId, e->errorMsg);
		return false;
	}

	// ── Instantiate ───────────────────────────────────────────────────────
	PluginBase *inst = nullptr;
	try {
		inst = createFn();
	} catch (const std::exception &ex) {
		DL_CLOSE(dlHandle);
		e->state    = PluginState::Error;
		e->errorMsg = QString("Exception in create(): %1").arg(ex.what());
		enterSafeMode(pluginId, e->errorMsg);
		Q_EMIT pluginError(pluginId, e->errorMsg);
		return false;
	} catch (...) {
		DL_CLOSE(dlHandle);
		e->state    = PluginState::Error;
		e->errorMsg = "Unknown exception in create()";
		enterSafeMode(pluginId, e->errorMsg);
		Q_EMIT pluginError(pluginId, e->errorMsg);
		return false;
	}

	// ── Build HostApi ──────────────────────────────────────────────────────
	auto *api = new HostApiImpl(pluginId, _session);
	// Load per-plugin settings from <pluginDir>/<id>/settings.json
	api->loadSettings(e->meta.manifestDir + "/settings.json");

	// Check API compatibility
	if (inst->meta().version.isEmpty()) {
		LOG(("[PluginManager] Plugin '%1' has no version in meta()").arg(pluginId));
	}

	// ── onLoad ─────────────────────────────────────────────────────────────
	try {
		inst->onLoad(api);
	} catch (const std::exception &ex) {
		auto *destroyFn = reinterpret_cast<DestroyFn>(
			DL_SYM(dlHandle, "pidrgram_plugin_destroy"));
		if (destroyFn) destroyFn(inst);
		delete api;
		DL_CLOSE(dlHandle);
		e->state    = PluginState::Error;
		e->errorMsg = QString("Exception in onLoad(): %1").arg(ex.what());
		enterSafeMode(pluginId, e->errorMsg);
		Q_EMIT pluginError(pluginId, e->errorMsg);
		return false;
	} catch (...) {
		auto *destroyFn = reinterpret_cast<DestroyFn>(
			DL_SYM(dlHandle, "pidrgram_plugin_destroy"));
		if (destroyFn) destroyFn(inst);
		delete api;
		DL_CLOSE(dlHandle);
		e->state    = PluginState::Error;
		e->errorMsg = "Unknown exception in onLoad()";
		enterSafeMode(pluginId, e->errorMsg);
		Q_EMIT pluginError(pluginId, e->errorMsg);
		return false;
	}

	s_native[pluginId] = { dlHandle, inst, api };
	e->state    = PluginState::Loaded;
	e->errorMsg = {};
	Q_EMIT pluginLoaded(pluginId);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// unloadPlugin
// ─────────────────────────────────────────────────────────────────────────────
bool PluginManager::unloadPlugin(const QString &pluginId) {
	auto *e = mutableEntryById(pluginId);
	if (!e || e->state != PluginState::Loaded) return false;

	auto it = s_native.find(pluginId);
	if (it != s_native.end()) {
		try {
			it->instance->onUnload();
		} catch (...) {}

		using DestroyFn = void(*)(PluginBase *);
		auto *destroyFn = reinterpret_cast<DestroyFn>(
			DL_SYM(it->dlHandle, "pidrgram_plugin_destroy"));
		if (destroyFn) destroyFn(it->instance);

		delete it->api;
		DL_CLOSE(it->dlHandle);
		s_native.erase(it);
	}

	e->state = PluginState::Unloaded;
	Q_EMIT pluginUnloaded(pluginId);
	return true;
}

void PluginManager::enablePlugin(const QString &pluginId, bool enable) {
	auto *e = mutableEntryById(pluginId);
	if (!e) return;
	e->meta.enabled = enable;
	if (enable && e->state != PluginState::Loaded && !_safeMode) {
		loadPlugin(pluginId);
	} else if (!enable && e->state == PluginState::Loaded) {
		unloadPlugin(pluginId);
	}
	persistState();
	Q_EMIT entriesChanged();
}

// ─────────────────────────────────────────────────────────────────────────────
// Safe mode
// ─────────────────────────────────────────────────────────────────────────────
bool PluginManager::isSafeMode() const       { return _safeMode; }
QString PluginManager::safeModeVictim() const { return _safeModeVictim; }

void PluginManager::enterSafeMode(const QString &faultingPluginId,
                                   const QString &reason) {
	if (_safeMode) return;
	_safeMode       = true;
	_safeModeVictim = faultingPluginId;

	LOG(("[PluginManager] Entering safe mode – faulting plugin: %1 | reason: %2")
		.arg(faultingPluginId, reason));

	// Unload everything that's currently running
	// Collect ids first to avoid mutation while iterating
	QStringList toUnload;
	for (const auto &e : _entries) {
		if (e.state == PluginState::Loaded) toUnload << e.meta.id;
	}
	for (const auto &id : toUnload) {
		unloadPlugin(id);
		if (auto *e = mutableEntryById(id)) {
			e->state = PluginState::DisabledBySafeMode;
		}
	}

	// Persistently disable the faulting plugin
	if (auto *e = mutableEntryById(faultingPluginId)) {
		e->meta.enabled = false;
	}
	persistState();
	Q_EMIT safeModeEntered(faultingPluginId);
}

void PluginManager::exitSafeMode() {
	if (!_safeMode) return;
	_safeMode       = false;
	_safeModeVictim = {};

	for (auto &e : _entries) {
		if (e.state == PluginState::DisabledBySafeMode) {
			e.state = PluginState::Unloaded;
		}
	}
	for (auto &e : _entries) {
		if (e.meta.enabled) loadPlugin(e.meta.id);
	}
	persistState();
	Q_EMIT safeModeExited();
	Q_EMIT entriesChanged();
}

// ─────────────────────────────────────────────────────────────────────────────
// Dispatch hooks
// ─────────────────────────────────────────────────────────────────────────────
bool PluginManager::dispatchUpdate(const MTPUpdates &updates) {
	// Iterate over a snapshot of keys to avoid invalidation if enterSafeMode
	// removes entries during iteration.
	const QStringList ids = s_native.keys();
	for (const auto &id : ids) {
		auto it = s_native.find(id);
		if (it == s_native.end()) continue;
		try {
			if (!it->instance->onUpdate(&updates)) return false;
		} catch (const std::exception &ex) {
			enterSafeMode(id, QString("onUpdate() threw: %1").arg(ex.what()));
			return true; // don't propagate crash to caller
		} catch (...) {
			enterSafeMode(id, "onUpdate() threw unknown exception");
			return true;
		}
	}
	return true;
}

void PluginManager::dispatchNewMessage(HistoryItem *item) {
	if (!item) return;
	const QStringList ids = s_native.keys();
	for (const auto &id : ids) {
		auto it = s_native.find(id);
		if (it == s_native.end()) continue;
		try {
			it->instance->onNewMessage(item);
		} catch (const std::exception &ex) {
			enterSafeMode(id, QString("onNewMessage() threw: %1").arg(ex.what()));
			return;
		} catch (...) {
			enterSafeMode(id, "onNewMessage() threw unknown exception");
			return;
		}
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Persistence
// ─────────────────────────────────────────────────────────────────────────────
QString PluginManager::statePath() const {
	return _pluginDir + "/state.json";
}

void PluginManager::persistState() {
	QJsonArray arr;
	for (const auto &e : _entries) arr.append(e.meta.toJson());

	QJsonObject root;
	root["safeMode"]       = _safeMode;
	root["safeModeVictim"] = _safeModeVictim;
	root["plugins"]        = arr;

	QFile f(statePath());
	if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		f.write(QJsonDocument(root).toJson());
	}
}

void PluginManager::loadState() {
	QFile f(statePath());
	if (!f.open(QIODevice::ReadOnly)) return;

	const auto doc = QJsonDocument::fromJson(f.readAll());
	if (doc.isNull()) return;

	const auto root = doc.object();
	_safeMode       = root["safeMode"].toBool(false);
	_safeModeVictim = root["safeModeVictim"].toString();

	for (const auto &v : root["plugins"].toArray()) {
		PluginEntry e;
		e.meta  = PluginMetaFull::fromJson(v.toObject());
		e.state = PluginState::Unloaded;
		_entries.append(e);
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────
const QList<PluginEntry> &PluginManager::entries() const {
	return _entries;
}

const PluginEntry *PluginManager::entryById(const QString &id) const {
	for (const auto &e : _entries) {
		if (e.meta.id == id) return &e;
	}
	return nullptr;
}

PluginEntry *PluginManager::mutableEntryById(const QString &id) {
	for (auto &e : _entries) {
		if (e.meta.id == id) return &e;
	}
	return nullptr;
}

} // namespace Plugins
