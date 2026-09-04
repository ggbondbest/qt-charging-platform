#pragma once

#include <QColor>
#include <QWidget>

namespace charging::server {

class MetricIconWidget final : public QWidget
{
public:
    MetricIconWidget(const QColor& accent, int iconType, QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QColor accent_;
    int iconType_ = 0;
};

class RevenueTrendWidget final : public QWidget
{
public:
    explicit RevenueTrendWidget(QWidget* parent = nullptr);

    void setPeriod(int period);
    void setDisplayMode(int displayMode);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    int period_ = 2;
    int displayMode_ = 0;
};

class DeviceStatusWidget final : public QWidget
{
public:
    explicit DeviceStatusWidget(QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;
};

} // namespace charging::server
