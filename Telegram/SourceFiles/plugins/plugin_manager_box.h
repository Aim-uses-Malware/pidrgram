#pragma once

#include "ui/layers/box_content.h"
#include <QtCore/QPointer>

namespace Plugins {

class PluginManagerBox final : public Ui::BoxContent {
public:
	explicit PluginManagerBox(QWidget *parent = nullptr);

protected:
	void prepare() override;

private:
	class Inner;
	QPointer<Inner> _inner;
};

} // namespace Plugins
