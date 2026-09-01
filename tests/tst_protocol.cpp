#include "charging/common/protocol/frame_codec.h"
#include "charging/common/protocol/protocol.h"

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QtTest>

class ProtocolTest final : public QObject
{
    Q_OBJECT

private slots:
    void requestAndResponsesRoundTrip();
    void decoderHandlesFragmentedAndCoalescedFrames();
    void decoderRejectsInvalidLengths();
    void parserRejectsInvalidEnvelope();
};

void ProtocolTest::requestAndResponsesRoundTrip()
{
    charging::protocol::RequestEnvelope request;
    request.type = QString::fromLatin1(charging::protocol::request_type::kUserLogin);
    request.requestId = QStringLiteral("request-001");
    request.data.insert(QStringLiteral("phone"), QStringLiteral("13800138000"));

    charging::protocol::ProtocolError parseError;
    charging::protocol::RequestEnvelope parsedRequest;
    QVERIFY(charging::protocol::parseRequestPayload(charging::protocol::serializePayload(request),
                                                    &parsedRequest, &parseError));
    QVERIFY(parseError.isEmpty());
    QCOMPARE(parsedRequest.protocolVersion, charging::protocol::kProtocolVersion);
    QVERIFY(parsedRequest.kind == charging::protocol::MessageKind::Request);
    QCOMPARE(parsedRequest.type, request.type);
    QCOMPARE(parsedRequest.requestId, request.requestId);
    QCOMPARE(parsedRequest.data.value(QStringLiteral("phone")).toString(),
             QStringLiteral("13800138000"));

    QJsonObject responseData;
    responseData.insert(QStringLiteral("created"), true);
    const charging::protocol::ResponseEnvelope success =
        charging::protocol::makeSuccessResponse(request, responseData);
    charging::protocol::ResponseEnvelope parsedSuccess;
    QVERIFY(charging::protocol::parseResponsePayload(charging::protocol::serializePayload(success),
                                                     &parsedSuccess, &parseError));
    QVERIFY(parsedSuccess.success);
    QVERIFY(parsedSuccess.error.isEmpty());
    QCOMPARE(parsedSuccess.requestId, request.requestId);
    QCOMPARE(parsedSuccess.data.value(QStringLiteral("created")).toBool(), true);

    charging::protocol::ProtocolError businessError;
    businessError.code = QString::fromLatin1(charging::protocol::error_code::kInvalidPhone);
    businessError.message = QStringLiteral("手机号格式错误");
    businessError.details.insert(QStringLiteral("field"), QStringLiteral("phone"));
    const charging::protocol::ResponseEnvelope failure =
        charging::protocol::makeErrorResponse(request, businessError);
    charging::protocol::ResponseEnvelope parsedFailure;
    QVERIFY(charging::protocol::parseResponsePayload(charging::protocol::serializePayload(failure),
                                                     &parsedFailure, &parseError));
    QVERIFY(!parsedFailure.success);
    QCOMPARE(parsedFailure.error.code, businessError.code);
    QCOMPARE(parsedFailure.error.details.value(QStringLiteral("field")).toString(),
             QStringLiteral("phone"));
}

void ProtocolTest::decoderHandlesFragmentedAndCoalescedFrames()
{
    const QByteArray firstPayload = QByteArrayLiteral("{\"message\":\"first\"}");
    const QByteArray secondPayload = QByteArrayLiteral("{\"message\":\"second\"}");
    QByteArray firstFrame;
    QByteArray secondFrame;
    charging::protocol::ProtocolError error;
    QVERIFY(charging::protocol::encodeFrame(firstPayload, &firstFrame, &error));
    QVERIFY(charging::protocol::encodeFrame(secondPayload, &secondFrame, &error));

    charging::protocol::FrameDecoder decoder;
    QList<QByteArray> completedPayloads;
    QVERIFY(decoder.append(firstFrame.left(1), &completedPayloads, &error));
    QVERIFY(completedPayloads.isEmpty());
    QVERIFY(decoder.append(firstFrame.mid(1, 2), &completedPayloads, &error));
    QVERIFY(completedPayloads.isEmpty());
    QVERIFY(decoder.append(firstFrame.mid(3, 5), &completedPayloads, &error));
    QVERIFY(completedPayloads.isEmpty());
    QVERIFY(decoder.append(firstFrame.mid(8), &completedPayloads, &error));
    QCOMPARE(completedPayloads.size(), 1);
    QCOMPARE(completedPayloads.constFirst(), firstPayload);

    decoder.reset();
    completedPayloads.clear();
    QVERIFY(decoder.append(firstFrame + secondFrame, &completedPayloads, &error));
    QCOMPARE(completedPayloads.size(), 2);
    QCOMPARE(completedPayloads.at(0), firstPayload);
    QCOMPARE(completedPayloads.at(1), secondPayload);
}

void ProtocolTest::decoderRejectsInvalidLengths()
{
    charging::protocol::FrameDecoder decoder;
    charging::protocol::ProtocolError error;
    QList<QByteArray> completedPayloads;

    QVERIFY(!decoder.append(QByteArray::fromHex("00000000"), &completedPayloads, &error));
    QCOMPARE(error.code, QString::fromLatin1(charging::protocol::error_code::kInvalidFrame));
    QCOMPARE(decoder.bufferedByteCount(), 0);

    QVERIFY(!decoder.append(QByteArray::fromHex("00100001"), &completedPayloads, &error));
    QCOMPARE(error.code, QString::fromLatin1(charging::protocol::error_code::kPayloadTooLarge));
    QCOMPARE(decoder.bufferedByteCount(), 0);
}

void ProtocolTest::parserRejectsInvalidEnvelope()
{
    charging::protocol::RequestEnvelope request;
    charging::protocol::ProtocolError error;

    QVERIFY(
        !charging::protocol::parseRequestPayload(QByteArrayLiteral("not-json"), &request, &error));
    QCOMPARE(error.code, QString::fromLatin1(charging::protocol::error_code::kInvalidJson));

    const QByteArray wrongVersion =
        QByteArrayLiteral("{\"protocolVersion\":2,\"kind\":\"REQUEST\",\"type\":\"USER_LOGIN\","
                          "\"requestId\":\"request-002\",\"data\":{}}");
    QVERIFY(!charging::protocol::parseRequestPayload(wrongVersion, &request, &error));
    QCOMPARE(error.code,
             QString::fromLatin1(charging::protocol::error_code::kUnsupportedProtocolVersion));
}

QTEST_GUILESS_MAIN(ProtocolTest)

#include "tst_protocol.moc"
