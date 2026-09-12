#pragma once
#include <QWidget>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
class QComboBox;
class QTabWidget;
class QTableWidget;
class QLabel;
class QPushButton;
class QTextEdit;
namespace charging::server {
class AdminRequestGateway;
class AdminOptionLoader;
class WorkflowManagementPage final : public QWidget
{
    Q_OBJECT
public:
    explicit WorkflowManagementPage(AdminRequestGateway* gateway, QWidget* parent = nullptr);
    void refreshData();
private:
    friend class WorkflowManagementRacesTest;
    void receive(const QString& id, const QJsonObject& response);
    void selectReport();
    void renderDetail();
    void mutate(const QString& action);
    void updateActions();
    QPointer<AdminRequestGateway> gateway_;
    AdminOptionLoader* stationLoader_;
    QComboBox* station_;
    QTabWidget* tabs_;
    QTableWidget* queueTable_;
    QTableWidget* repairTable_;
    QLabel* message_;
    QLabel* paging_;
    QTextEdit* timeline_;
    QPushButton* previous_;
    QPushButton* next_;
    QPushButton* accept_;
    QPushButton* start_;
    QPushButton* resolve_;
    QPushButton* retry_;
    QTimer refreshTimer_;
    QJsonArray repairs_;
    QJsonObject selected_, retryPayload_;
    QString listRequest_, detailRequest_, mutationRequest_, retryAction_;
    int page_ = 1;
    int total_ = 0;
    quint64 authenticationGeneration_ = 0;
};
}
