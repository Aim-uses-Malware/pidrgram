#pragma once

#include "ui/rp_widget.h"

namespace Ui {
class FlatLabel;
class RoundButton;
} // namespace Ui

namespace Plugins {

// ─────────────────────────────────────────────────────────────────────────────
// SafeModeBanner
//
// A full-width horizontal strip placed at the very top of the main window
// content area whenever the plugin system is in safe mode.
// Shows a warning text and a "Выключить" (disable) button that calls
// PluginManager::exitSafeMode() when clicked.
//
// Lifecycle: create once, attach to the main window resize signal, call
// resizeToWidth() whenever the window width changes.
// ─────────────────────────────────────────────────────────────────────────────
class SafeModeBanner final : public Ui::RpWidget {
public:
	explicit SafeModeBanner(QWidget *parent);

	// Must be called whenever the parent changes width.
	void resizeToWidth(int width);

	static constexpr int kHeight = 36;

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;

private:
	void setup();

	Ui::FlatLabel   *_label = nullptr;
	Ui::RoundButton *_btn   = nullptr;
};

} // namespace Plugins
