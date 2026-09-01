#include <QByteArray>
#include <QString>
#include <QVersionNumber>
#include <QtGlobal>
#include <QtTest>

class QtBaselineTest final : public QObject
{
    Q_OBJECT

private slots:
    void runtimeVersionMeetsBaseline();
    void compilerUsesCxx17();
};

void QtBaselineTest::runtimeVersionMeetsBaseline()
{
    const QVersionNumber runtimeVersion =
        QVersionNumber::fromString(QString::fromLatin1(qVersion()));
    const QVersionNumber baselineVersion =
        QVersionNumber::fromString(QStringLiteral(CHARGING_PLATFORM_QT_BASELINE_VERSION));
    const QByteArray failureMessage =
        QStringLiteral("Qt runtime %1 is older than the required baseline %2")
            .arg(runtimeVersion.toString(), baselineVersion.toString())
            .toLocal8Bit();

    QVERIFY2(QVersionNumber::compare(runtimeVersion, baselineVersion) >= 0,
             failureMessage.constData());
}

void QtBaselineTest::compilerUsesCxx17()
{
    QVERIFY(__cplusplus >= 201703L);
}

QTEST_GUILESS_MAIN(QtBaselineTest)

#include "tst_qt_baseline.moc"
