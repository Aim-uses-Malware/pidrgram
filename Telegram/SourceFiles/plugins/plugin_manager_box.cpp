#include "plugins/plugin_manager_box.h"
#include "plugins/plugin_manager.h"

#include "ui/widgets/labels.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/scroll_area.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/painter.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"
#include "lang/lang_keys.h"

#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QLabel>

namespace Plugins {
namespace {

// ─────────────────────────────────────────────────────────────────────────────
// PluginCard  –  one card per PluginEntry
// ─────────────────────────────────────────────────────────────────────────────
class PluginCard final : public Ui::RpWidget {
public:
	PluginCard(
		QWidget *parent,
		const PluginEntry &entry,
		Fn<void(QString, bool)> onToggle)
	: Ui::RpWidget(parent)
	, _entry(entry)
	, _onToggle(std::move(onToggle))
	{
		setupUi();
	}

	void updateEntry(const PluginEntry &e) {
		_entry = e;
		_toggle->setChecked(_entry.meta.enabled, anim::type::instant);
		refreshState();
		update();
	}

private:
	// ─── layout constants ────────────────────────────────────────────────
	static constexpr int kPad      = 12;
	static constexpr int kToggleW  = 50;
	static constexpr int kToggleH  = 28;
	static constexpr int kBaseH    = 74;
	static constexpr int kErrorH   = 20;

	void setupUi() {
		// Name + version
		_nameLabel = Ui::CreateChild<Ui::FlatLabel>(
			this,
			QString("%1  %2").arg(_entry.meta.name, _entry.meta.version),
			st::settingsSubsectionTitle);

		// Author — Description (truncated)
		const QString sub = QString("%1  ·  %2")
			.arg(_entry.meta.author)
			.arg(_entry.meta.description.left(100));
		_subLabel = Ui::CreateChild<Ui::FlatLabel>(
			this, sub, st::boxTextSmall);

		// Toggle
		_toggle = Ui::CreateChild<Ui::ToggleButton>(
			this,
			rpl::single(_entry.meta.enabled),
			st::settingsToggle);
		_toggle->setClickedCallback([this] {
			_onToggle(_entry.meta.id, !_entry.meta.enabled);
		});

		// Error / safe mode label (hidden by default)
		_errorLabel = Ui::CreateChild<Ui::FlatLabel>(
			this,
			QString(),
			st::attentionBoxLabel);
		_errorLabel->hide();

		refreshState();
	}

	void refreshState() {
		switch (_entry.state) {
		case PluginState::Loaded:
			_errorLabel->hide();
			_toggle->setDisabled(false);
			break;
		case PluginState::Error:
			_errorLabel->setText(
				QString("⚠  %1").arg(_entry.errorMsg.left(140)));
			_errorLabel->show();
			_toggle->setDisabled(false);
			break;
		case PluginState::DisabledBySafeMode:
			_errorLabel->setText("Отключён из-за безопасного режима");
			_errorLabel->show();
			_toggle->setDisabled(true);
			break;
		case PluginState::Unloaded:
		default:
			_errorLabel->hide();
			_toggle->setDisabled(false);
			break;
		}
		updateGeometry();
	}

	QSize sizeHint() const override {
		const int h = kBaseH + (_errorLabel->isVisible() ? kErrorH + 4 : 0);
		return { width(), h };
	}

	void resizeEvent(QResizeEvent *e) override {
		const int w   = width();
		const int tw  = kToggleW;
		const int tx  = w - tw - kPad;
		const int cy  = (kBaseH - kToggleH) / 2;
		const int lw  = tx - kPad - 8;

		_nameLabel->setGeometry(kPad, 10, lw, 20);
		_subLabel ->setGeometry(kPad, 34, lw, 32);
		_toggle   ->setGeometry(tx,   cy, tw, kToggleH);

		if (_errorLabel->isVisible()) {
			_errorLabel->setGeometry(kPad, kBaseH, w - 2 * kPad, kErrorH);
		}
		RpWidget::resizeEvent(e);
	}

	void paintEvent(QPaintEvent *) override {
		Painter p(this);
		const auto r = rect().adjusted(4, 2, -4, -2);
		p.setBrush(st::boxBg);
		p.setPen(Qt::NoPen);
		p.drawRoundedRect(r, st::roundRadiusSmall, st::roundRadiusSmall);
	}

	PluginEntry            _entry;
	Fn<void(QString,bool)> _onToggle;
	Ui::FlatLabel         *_nameLabel  = nullptr;
	Ui::FlatLabel         *_subLabel   = nullptr;
	Ui::ToggleButton      *_toggle     = nullptr;
	Ui::FlatLabel         *_errorLabel = nullptr;
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// PluginManagerBox::Inner
// ─────────────────────────────────────────────────────────────────────────────
class PluginManagerBox::Inner final : public Ui::RpWidget {
public:
	explicit Inner(QWidget *parent) : Ui::RpWidget(parent) {
		rebuild();

		// React to any change in plugin states (load/unload/error/safe mode)
		PluginManager::instance().entriesChanged(
		) | rpl::start_with_next([this] { rebuild(); }, lifetime());

		PluginManager::instance().pluginLoaded(
		) | rpl::start_with_next([this](const QString &) { rebuild(); },
		                          lifetime());

		PluginManager::instance().pluginError(
		) | rpl::start_with_next([this](const QString &, const QString &) {
			rebuild();
		}, lifetime());

		PluginManager::instance().safeModeEntered(
		) | rpl::start_with_next([this](const QString &) { rebuild(); },
		                          lifetime());

		PluginManager::instance().safeModeExited(
		) | rpl::start_with_next([this] { rebuild(); }, lifetime());
	}

	void rebuild() {
		// Tear down old layout cleanly
		if (auto *l = layout()) {
			while (auto *item = l->takeAt(0)) {
				if (auto *w = item->widget()) w->deleteLater();
				delete item;
			}
			delete l;
		}

		auto *vbox = new QVBoxLayout(this);
		vbox->setContentsMargins(8, 8, 8, 8);
		vbox->setSpacing(6);

		// ── Safe mode notice ─────────────────────────────────────────────
		if (PluginManager::instance().isSafeMode()) {
			const QString victim = PluginManager::instance().safeModeVictim();
			auto *notice = new QLabel(
				QString(
					"⚠  Безопасный режим активен.\n"
					"Плагин «%1» вызвал ошибку и был отключён.\n"
					"Остальные плагины временно приостановлены."
				).arg(victim.isEmpty() ? "неизвестен" : victim),
				this);
			notice->setWordWrap(true);
			notice->setStyleSheet(
				"background:#FFA000;color:#000;border-radius:6px;padding:8px;");
			vbox->addWidget(notice);
		}

		const auto &entries = PluginManager::instance().entries();
		if (entries.isEmpty()) {
			auto *empty = new QLabel(
				"Плагины не найдены.\n\n"
				"Положите папку с plugin.json в:\n" +
				PluginManager::instance().pluginDir(),
				this);
			empty->setWordWrap(true);
			empty->setAlignment(Qt::AlignCenter);
			empty->setStyleSheet("color:gray;padding:24px;");
			vbox->addWidget(empty);
			vbox->addStretch();
			setMinimumHeight(180);
			return;
		}

		for (const auto &e : entries) {
			auto *card = new PluginCard(
				this,
				e,
				[](const QString &id, bool en) {
					PluginManager::instance().enablePlugin(id, en);
				});
			vbox->addWidget(card);
		}
		vbox->addStretch();

		const int minH = 160 + static_cast<int>(entries.size()) * 82;
		setMinimumHeight(minH);
		adjustSize();
	}
};

// ─────────────────────────────────────────────────────────────────────────────
// PluginManagerBox
// ─────────────────────────────────────────────────────────────────────────────
PluginManagerBox::PluginManagerBox(QWidget *parent)
: Ui::BoxContent() {}

void PluginManagerBox::prepare() {
	setTitle(rpl::single(QString("Менеджер плагинов")));

	const int boxW = st::boxWideWidth;

	auto scroll = object_ptr<Ui::ScrollArea>(this);
	auto *inner = scroll->setOwnedWidget(
		object_ptr<Inner>(scroll.data()));
	_inner = inner;

	inner->resizeToWidth(boxW);
	scroll->setGeometry(0, 0, boxW, 400);

	setDimensions(boxW, 440);
	addButton(tr::lng_close(), [this] { closeBox(); });
}

} // namespace Plugins
