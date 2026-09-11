#pragma once

#include <QWidget>

namespace charging::client {

class Spinner;

// Translucent busy blocker painted over a page while a request is in flight.
// Attach one per page; call showFor()/hideFor() around async work.
class LoadingOverlay final : public QWidget
{
    Q_OBJECT

public:
    explicit LoadingOverlay(QWidget* parent);

    void showFor();
    void hideFor();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void centerSpinner();

    Spinner* spinner_ = nullptr;
};

} // namespace charging::client
