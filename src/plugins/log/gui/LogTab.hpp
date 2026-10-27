#pragma once
#include <QWidget>
class GuiHostServices;
namespace Ui { class LogTab; }
class LogTab : public QWidget
{
public:
    explicit LogTab(GuiHostServices& services);
    ~LogTab() override;
private:
    Ui::LogTab* ui;
};
