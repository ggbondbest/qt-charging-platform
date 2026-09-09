#pragma once

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

class QComboBox;

namespace charging::server {
class AdminRequestGateway;

// Searchable, incremental server options. The sentinel loads another page;
// it is never exposed as an entity ID to a business query.
class AdminOptionLoader final : public QObject
{
public:
    AdminOptionLoader(AdminRequestGateway* gateway, QComboBox* combo,
                      const QString& action, const QString& placeholder,
                      const QString& scope, QObject* parent);
    void reload(const QJsonObject& filters = {}, bool keepSelection = true);
    // Periodic refresh must not discard a successful search/pagination state.
    // Only repeat the failed request, with its original filters/keyword/page.
    void retryIfFailed();

private:
    void requestPage(int page);
    void receive(const QJsonObject& response);
    QPointer<AdminRequestGateway> gateway_;
    QComboBox* combo_;
    QString action_, placeholder_, scope_, requestId_, keyword_;
    QString selectedId_;
    QJsonObject filters_;
    QTimer debounce_;
    int nextPage_ = 0;
    int requestedPage_ = 1;
    bool requestFailed_ = false;
};
} // namespace charging::server
