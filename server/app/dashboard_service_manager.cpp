#include "dashboard_service_manager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QStandardPaths>

namespace charging::server {
namespace {
QString findRepository()
{
    QDir directory(QCoreApplication::applicationDirPath());
    do {
        if (QFileInfo::exists(directory.filePath(QStringLiteral("scripts/managed_web_service.py"))))
            return directory.absolutePath();
    } while (directory.cdUp());
    const QString sourceRoot = QStringLiteral(CHARGING_DASHBOARD_SOURCE_ROOT);
    if (QFileInfo::exists(QDir(sourceRoot).filePath(QStringLiteral("scripts/managed_web_service.py"))))
        return sourceRoot;
    return {};
}

QString executablePath(const QString& candidate, const QString& base)
{
    if (candidate.isEmpty()) return {};
    if (!candidate.contains(QLatin1Char('/')) && !candidate.contains(QLatin1Char('\\')))
        return QStandardPaths::findExecutable(candidate);
    const QFileInfo file(QDir(base).absoluteFilePath(candidate));
    // Keep a venv interpreter's symlink path: resolving it to the base Python
    // executable discards pyvenv.cfg discovery and its installed dependencies.
    return file.isFile() && file.isExecutable() ? file.absoluteFilePath() : QString();
}

bool validWebUrl(const QUrl& url)
{
    return url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty()
        && (url.scheme() == QStringLiteral("http") || url.scheme() == QStringLiteral("https"));
}
} // namespace

DashboardServiceManager::Options DashboardServiceManager::discoverOptions(bool autoStart)
{
    Options options;
    options.autoStart = autoStart;
    options.discoverOnRetry = true;
    options.externalUrl = qEnvironmentVariable("CHARGING_DASHBOARD_URL").trimmed();
    // Explicit external URLs work even in packaged installs without Python or
    // a source tree. Do not inspect unrelated local service configuration.
    if (!options.externalUrl.isEmpty() || !autoStart) return options;
    options.repositoryRoot = findRepository();
    options.configPath = qEnvironmentVariable("CHARGING_DASHBOARD_CONFIG").trimmed();
    const bool explicitConfig = !options.configPath.isEmpty();
    if (!explicitConfig && !options.repositoryRoot.isEmpty()) {
        const QString candidate = QDir(options.repositoryRoot).filePath(QStringLiteral("runtime/dashboard-service.json"));
        if (QFileInfo::exists(candidate)) options.configPath = candidate;
    }
    QJsonObject config;
    if (!options.configPath.isEmpty()) {
        QFile file(options.configPath);
        QJsonParseError error;
        if (!file.open(QIODevice::ReadOnly)) {
            options.configurationError = tr("网页服务配置无法读取，请检查本机服务配置后重试。");
            return options;
        }
        const auto document = QJsonDocument::fromJson(file.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            options.configurationError = tr("网页服务配置格式不正确，请检查本机服务配置后重试。");
            return options;
        }
        config = document.object();
        options.configPath = QFileInfo(file).canonicalFilePath();
    }
    QString requested = qEnvironmentVariable("CHARGING_WEB_PYTHON").trimmed();
    QString base = options.repositoryRoot;
    if (requested.isEmpty()) {
        requested = config.value(QStringLiteral("pythonExecutable")).toString();
        if (!options.configPath.isEmpty()) base = QFileInfo(options.configPath).absolutePath();
    }
    if (!requested.isEmpty()) {
        options.pythonExecutable = executablePath(requested, base);
        if (options.pythonExecutable.isEmpty())
            options.configurationError = tr("配置的网页运行环境不可用，请检查安装后重试。");
        return options;
    }
    for (const QString& relative : {QStringLiteral("data_analysis/.venv/bin/python3"),
                                     QStringLiteral("data_analysis/.venv/bin/python"),
                                     QStringLiteral("data_analysis/.venv/Scripts/python.exe")}) {
        const QString candidate = executablePath(relative, options.repositoryRoot);
        if (!candidate.isEmpty()) { options.pythonExecutable = candidate; return options; }
    }
    for (const QString& name : {QStringLiteral("python3"), QStringLiteral("python")}) {
        options.pythonExecutable = QStandardPaths::findExecutable(name);
        if (!options.pythonExecutable.isEmpty()) break;
    }
    return options;
}

DashboardServiceManager::DashboardServiceManager(const Options& options, QObject* parent)
    : QObject(parent), options_(options)
{
    startupTimer_.setSingleShot(true);
    shutdownTimer_.setSingleShot(true);
    connect(&startupTimer_, &QTimer::timeout, this, [this] {
        fail(tr("网页服务启动超时。充电业务可继续使用，请稍后重试。"));
    });
    connect(&shutdownTimer_, &QTimer::timeout, this, [this] {
        if (process_.state() == QProcess::NotRunning) return;
        if (!terminateSent_) {
            terminateSent_ = true;
            process_.terminate();
            shutdownTimer_.start(3000);
        } else process_.kill();
    });
    connect(&process_, &QProcess::readyReadStandardOutput, this, [this] { readOutput(false); });
    connect(&process_, &QProcess::readyReadStandardError, this, [this] { readOutput(true); });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            if (state_ == State::Stopping) { finishStop(); return; }
            fail(tr("网页运行环境无法启动。充电业务可继续使用，请检查安装后重试。"));
            if (retryPending_) { retryPending_ = false; retry(); }
        }
    });
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int, QProcess::ExitStatus) {
        readOutput(false);
        readOutput(true);
        startupTimer_.stop();
        shutdownTimer_.stop();
        stoppingProcess_ = false;
        if (state_ == State::Stopping) { finishStop(); return; }
        if (retryPending_) { retryPending_ = false; retry(); return; }
        if (state_ != State::Failed)
            fail(lastServiceError_.isEmpty() ? tr("网页服务已停止。充电业务可继续使用，点击重试即可恢复。") : lastServiceError_);
    });
    setState(State::Idle, tr("正在等待充电业务服务就绪…"));
}

DashboardServiceManager::~DashboardServiceManager()
{
    // Normal shutdown is asynchronous via stopped(). This fallback is for an
    // owner destroyed without completing that lifecycle; never touch external services.
    if (process_.state() != QProcess::NotRunning) {
        process_.disconnect(this);
        process_.write("CHARGING_WEB_STOP\n");
        process_.closeWriteChannel();
        if (!process_.waitForFinished(options_.shutdownTimeoutMs)) {
            process_.kill();
            process_.waitForFinished(1000);
        }
    }
}

void DashboardServiceManager::setState(State state, const QString& message)
{
    state_ = state;
    message_ = message;
    emit stateChanged();
}

void DashboardServiceManager::start(const QString& databasePath)
{
    if (state_ != State::Idle) return;
    databasePath_ = QFileInfo(databasePath).canonicalFilePath();
    if (!options_.externalUrl.isEmpty()) {
        const QUrl external(options_.externalUrl, QUrl::StrictMode);
        if (!validWebUrl(external)) {
            fail(tr("外部网页地址无效，请配置完整的 HTTP 或 HTTPS 地址。"));
            return;
        }
        url_ = external;
        setState(State::External, tr("正在使用配置的外部网页服务。"));
        emit ready(url_);
        return;
    }
    if (!options_.autoStart) {
        setState(State::Disabled, tr("已关闭网页服务自动启动，可填写已启动的服务地址。"));
        return;
    }
    launch();
}

void DashboardServiceManager::launch()
{
    shutdownTimer_.stop();
    url_.clear();
    mode_.clear();
    stdoutBuffer_.clear();
    stderrBuffer_.clear();
    lastServiceError_.clear();
    stoppingProcess_ = false;
    terminateSent_ = false;
    if (!options_.configurationError.isEmpty()) { fail(options_.configurationError); return; }
    if (databasePath_.isEmpty() || !QFileInfo(databasePath_).isFile()) {
        fail(tr("网页服务需要可读取的业务数据库。充电业务可继续使用。"));
        return;
    }
    const QString script = QDir(options_.repositoryRoot).filePath(QStringLiteral("scripts/managed_web_service.py"));
    if (options_.repositoryRoot.isEmpty() || !QFileInfo(script).isFile()) {
        fail(tr("未找到网页服务组件，请检查完整项目安装后重试。"));
        return;
    }
    if (options_.pythonExecutable.isEmpty()) {
        fail(tr("未找到网页运行环境，请完成本机环境配置后重试。"));
        return;
    }
    QStringList arguments{script, QStringLiteral("--database"), databasePath_,
                          QStringLiteral("--parent-pid"), QString::number(QCoreApplication::applicationPid())};
    if (!options_.configPath.isEmpty()) arguments << QStringLiteral("--config") << options_.configPath;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    process_.setProcessEnvironment(environment);
    process_.setWorkingDirectory(options_.repositoryRoot);
    process_.setProgram(options_.pythonExecutable);
    process_.setArguments(arguments);
    setState(State::Starting, tr("正在准备智能运营中心，首次加载可能需要一些时间…"));
    startupTimer_.start(options_.startupTimeoutMs);
    process_.start();
}

void DashboardServiceManager::retry()
{
    if (state_ != State::Failed || !options_.externalUrl.isEmpty() || !options_.autoStart) return;
    if (process_.state() != QProcess::NotRunning) {
        retryPending_ = true;
        setState(State::Failed, tr("正在清理上一次网页服务，随后自动重试…"));
        stopOwnedProcess();
        return;
    }
    // Re-read user configuration so fixing Python/config does not require a Qt restart.
    // Explicitly injected options (including tests) retain their chosen repository.
    if (options_.discoverOnRetry) options_ = discoverOptions(options_.autoStart);
    if (!options_.externalUrl.isEmpty()) {
        state_ = State::Idle;
        start(databasePath_);
        return;
    }
    launch();
}

void DashboardServiceManager::readOutput(bool standardError)
{
    auto& buffer = standardError ? stderrBuffer_ : stdoutBuffer_;
    buffer += standardError ? process_.readAllStandardError() : process_.readAllStandardOutput();
    int newline;
    while ((newline = buffer.indexOf('\n')) >= 0) {
        const QByteArray line = buffer.left(newline).trimmed();
        buffer.remove(0, newline + 1);
        consumeLine(line, standardError);
    }
    // Ignore verbose third-party output without retaining an unbounded buffer.
    if (buffer.size() > 65536) buffer.clear();
}

void DashboardServiceManager::consumeLine(const QByteArray& line, bool standardError)
{
    if (stoppingProcess_ || state_ == State::Stopping || state_ == State::Stopped) return;
    if (standardError) {
        if (!line.startsWith("CHARGING_WEB_ERROR=")) return;
        const auto error = QJsonDocument::fromJson(line.mid(sizeof("CHARGING_WEB_ERROR=") - 1)).object();
        // Only the structured safe-message contract is user-visible. Never show
        // raw stderr, tracebacks, command lines, environment values or passwords.
        const QString message = error.value(QStringLiteral("message")).toString();
        if (!message.isEmpty() && message.size() <= 300) lastServiceError_ = message;
        fail(lastServiceError_.isEmpty() ? tr("网页服务启动失败。充电业务可继续使用，请重试。") : lastServiceError_);
        return;
    }
    if (state_ != State::Starting || !line.startsWith("CHARGING_WEB_READY=")) return;
    const auto result = QJsonDocument::fromJson(line.mid(sizeof("CHARGING_WEB_READY=") - 1)).object();
    const QUrl url(result.value(QStringLiteral("url")).toString(), QUrl::StrictMode);
    const QString mode = result.value(QStringLiteral("mode")).toString();
    const QString database = QFileInfo(result.value(QStringLiteral("database")).toString()).canonicalFilePath();
    if (!validWebUrl(url) || url.scheme() != QStringLiteral("http") || url.host() != QStringLiteral("127.0.0.1")
        || url.port() < 1 || url.port() > 65535 || database != databasePath_
        || (mode != QStringLiteral("full") && mode != QStringLiteral("qt-live-only"))) {
        fail(tr("网页服务就绪信息与当前业务不匹配，请重试。"));
        return;
    }
    startupTimer_.stop();
    url_ = url;
    mode_ = mode;
    setState(State::Ready, mode == QStringLiteral("full") ? tr("智能运营中心已就绪。")
             : tr("当前业务看板已就绪；历史分析尚未配置。"));
    emit ready(url_);
}

void DashboardServiceManager::fail(const QString& message)
{
    if (state_ == State::Stopping || state_ == State::Stopped) return;
    startupTimer_.stop();
    url_.clear();
    setState(State::Failed, message);
    stopOwnedProcess();
}

void DashboardServiceManager::stopOwnedProcess()
{
    if (stoppingProcess_ || process_.state() == QProcess::NotRunning) return;
    stoppingProcess_ = true;
    process_.write("CHARGING_WEB_STOP\n");
    process_.closeWriteChannel();
    shutdownTimer_.start(options_.shutdownTimeoutMs);
}

void DashboardServiceManager::stop()
{
    if (state_ == State::Stopped || state_ == State::Stopping) return;
    retryPending_ = false;
    startupTimer_.stop();
    url_.clear();
    setState(State::Stopping, tr("正在关闭网页服务…"));
    if (process_.state() == QProcess::NotRunning) finishStop();
    else stopOwnedProcess();
}

void DashboardServiceManager::finishStop()
{
    startupTimer_.stop();
    shutdownTimer_.stop();
    setState(State::Stopped, tr("网页服务已停止。"));
    emit stopped();
}
} // namespace charging::server
