#include "plugins/safe_mode_banner.h"
#include "plugins/plugin_manager.h"

#include "ui/widgets/labels.h"
#include "ui/widgets/buttons.h"
#include "ui/painter.h"
#include "styles/style_chat.h"
#include "styles/style_widgets.h"

namespace Plugins {

namespace {
constexpr QColor kBannerBg  { 0xFF, 0xA0, 0x00, 230 }; // amber
constexpr QColor kBannerText{ 0x00, 0x00, 0x00, 255 }; // black on amber
} // namespace

SafeModeBanner::SafeModeBanner(QWidget *parent)
: Ui::RpWidget(parent) {
	setup();
}

void SafeModeBanner::setup() {
	setFixedHeight(kHeight);

	// Build label text once (victim name may be empty at construction time
	// if we are restoring safe mode from persistent state).
	auto labelText = [](const QString &victimId) -> QString {
		if (victimId.isEmpty()) {
			return "⚠  Безопасный режим  —  плагины отключены";
		}
		return QString("⚠  Безопасный режим  —  плагин «%1» вызвал ошибку")
			.arg(victimId);
	};

	_label = Ui::CreateChild<Ui::FlatLabel>(
		this,
		labelText(PluginManager::instance().safeModeVictim()),
		st::defaultFlatLabel);

	_btn = Ui::CreateChild<Ui::RoundButton>(
		this,
		rpl::single(QString("Выключить")),
		st::attentionBoxButton);

	_btn->setClickedCallback([] {
		PluginManager::instance().exitSafeMode();
	});

	// ── React to safe-mode transitions ────────────────────────────────────
	PluginManager::instance().safeModeExited(
	) | rpl::start_with_next([this] {
		hide();
	}, lifetime());

	PluginManager::instance().safeModeEntered(
	) | rpl::start_with_next([this, labelText](const QString &victimId) {
		_label->setText(labelText(victimId));
		show();
		raise();
	}, lifetime());

	// Initial visibility
	setVisible(PluginManager::instance().isSafeMode());
}

void SafeModeBanner::resizeToWidth(int w) {
	resize(w, kHeight);
	// triggers resizeEvent
}

void SafeModeBanner::resizeEvent(QResizeEvent *) {
	const int w    = width();
	const int h    = kHeight;
	const int pad  = 12;
	const int btnW = 110;
	const int btnH = 26;

	if (_label) {
		_label->setGeometry(pad, (h - 20) / 2, w - 2 * pad - btnW - 8, 20);
	}
	if (_btn) {
		_btn->setGeometry(w - btnW - pad, (h - btnH) / 2, btnW, btnH);
	}
}

void SafeModeBanner::paintEvent(QPaintEvent *) {
	Painter p(this);
	p.fillRect(rect(), kBannerBg);
}

} // namespace Plugins
