#pragma once

#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QUrl>

namespace charging::server {

// Owns only the Python process it starts. Business networking and database
// writes remain entirely independent of this optional, read-only web service.
class DashboardServiceManager final : public QObject
{
    Q_OBJECT
public:
    enum class State { Idle, Starting, Ready, External, Disabled, Failed, Stopping, Stopped };
    Q_ENUM(State)
    struct Options {
        QString repositoryRoot;
        QString configPath;
        QString pythonExecutable;
        QString externalUrl;
        QString configurationError;
        bool autoStart = true;
        bool discoverOnRetry = false;
        int startupTimeoutMs = 90000;
        int shutdownTimeoutMs = 15000;
    };

    explicit DashboardServiceManager(const Options& options, QObject* parent = nullptr);
    ~DashboardServiceManager() override;
    static Options discoverOptions(bool autoStart = true);
    State state() const { return state_; }
    QString message() const { return message_; }
    QUrl url() const { return url_; }
    QString mode() const { return mode_; }
    bool isStopped() const { return state_ == State::Stopped; }
    bool managesService() const { return options_.autoStart && options_.externalUrl.isEmpty(); }
    qint64 processId() const { return process_.processId(); }

public slots:
    void start(const QString& databasePath);
    void retry();
    void stop();

signals:
    void stateChanged();
    void ready(const QUrl& url);
    void stopped();

private:
    void launch();
    void readOutput(bool standardError);
    void consumeLine(const QByteArray& line, bool standardError);
    void fail(const QString& message);
    void stopOwnedProcess();
    void setState(State state, const QString& message);
    void finishStop();

    Options options_;
    State state_ = State::Idle;
    QString databasePath_;
    QString message_;
    QString mode_;
    QUrl url_;
    QProcess process_;
    QTimer startupTimer_;
    QTimer shutdownTimer_;
    QByteArray stdoutBuffer_;
    QByteArray stderrBuffer_;
    QString lastServiceError_;
    bool stoppingProcess_ = false;
    bool terminateSent_ = false;
    bool retryPending_ = false;
};

} // namespace charging::server
