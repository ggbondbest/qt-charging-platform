#include <QtTest>

#include <QGuiApplication>
#include <QPalette>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>

#include <memory>

namespace {

std::unique_ptr<QObject> createFixture(QQmlEngine& engine, QQuickWindow& window)
{
    const QByteArray source = QByteArrayLiteral(
        "import QtQuick\n"
        "import QtQuick.Controls.Basic\n"
        "import \"")
        + QUrl::fromLocalFile(QStringLiteral(CHARGING_QML_SOURCE_DIR)
                             + QStringLiteral("/platform")).toEncoded()
        + QByteArrayLiteral("\" as P\n")
        + QByteArrayLiteral(R"(
Item {
    id: root
    width: 420; height: 540
    property string theme: "light"
    property string fontScale: "standard"
    property int cardClicks: 0
    property int actionClicks: 0
    property alias pricePopup: price.popup
    onThemeChanged: P.Style.theme = theme
    onFontScaleChanged: P.Style.fontScale = fontScale
    Column {
        anchors.fill: parent
        spacing: 12
        P.TextField { objectName: "field"; width: 340; placeholderText: "输入起始地址" }
        P.ComboBox {
            id: price; objectName: "price"; width: 180
            model: ["全部电价", "1元以下", "1-2元"]
        }
        P.ComboBox {
            objectName: "city"; width: 280; editable: true
            model: ["大连市", "北京市", "沈阳市"]
        }
        P.ClickableCard {
            objectName: "card"; width: 340
            onClicked: root.cardClicks++
            Text { text: "测试电站"; color: P.Style.ink }
            P.ActionButton {
                objectName: "book"; text: "预约"
                onClicked: root.actionClicks++
            }
        }
    }
}
)");
    QQmlComponent component(&engine);
    component.setData(source, QUrl(QStringLiteral("file:///platform-controls-test.qml")));
    if (component.isError()) {
        qWarning().noquote() << component.errorString();
        return nullptr;
    }
    std::unique_ptr<QObject> result(component.create());
    auto* item = qobject_cast<QQuickItem*>(result.get());
    if (item)
        item->setParentItem(window.contentItem());
    return result;
}

QObject* objectProperty(QObject* object, const char* name)
{
    return object->property(name).value<QObject*>();
}

QPoint centerOf(QQuickItem* item)
{
    return item->mapToScene(QPointF(item->width() / 2.0, item->height() / 2.0)).toPoint();
}

} // namespace

class QmlPlatformControlsTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QQuickStyle::setStyle(QStringLiteral("Basic"));
    }

    void lightControlsIgnoreDarkSystemPalette()
    {
        struct PaletteRestore {
            QPalette previous = QGuiApplication::palette();
            ~PaletteRestore() { QGuiApplication::setPalette(previous); }
        } restore;
        QPalette dark = restore.previous;
        dark.setColor(QPalette::Base, Qt::black);
        dark.setColor(QPalette::Button, Qt::black);
        dark.setColor(QPalette::Text, Qt::white);
        dark.setColor(QPalette::ButtonText, Qt::white);
        QGuiApplication::setPalette(dark);
        QQmlEngine engine;
        QQuickWindow window;
        auto fixture = createFixture(engine, window);
        QVERIFY(fixture);
        auto* field = fixture->findChild<QQuickItem*>(QStringLiteral("field"));
        auto* price = fixture->findChild<QQuickItem*>(QStringLiteral("price"));
        QVERIFY(field);
        QVERIFY(price);
        QCOMPARE(objectProperty(field, "background")->property("color").value<QColor>(),
                 QColor(Qt::white));
        QCOMPARE(field->property("color").value<QColor>(), QColor(QStringLiteral("#1F2937")));
        QCOMPARE(objectProperty(price, "background")->property("color").value<QColor>(),
                 QColor(Qt::white));
        QCOMPARE(objectProperty(price, "contentItem")->property("color").value<QColor>(),
                 QColor(QStringLiteral("#1F2937")));
        QCOMPARE(price->implicitHeight(), 44.0);
    }

    void darkAppThemeKeepsConsistentInputsAndWhitePrimaryText()
    {
        QQmlEngine engine;
        QQuickWindow window;
        auto fixture = createFixture(engine, window);
        QVERIFY(fixture);
        fixture->setProperty("theme", QStringLiteral("dark"));
        auto* field = fixture->findChild<QQuickItem*>(QStringLiteral("field"));
        auto* price = fixture->findChild<QQuickItem*>(QStringLiteral("price"));
        auto* book = fixture->findChild<QQuickItem*>(QStringLiteral("book"));
        QVERIFY(field && price && book);
        QCOMPARE(objectProperty(field, "background")->property("color").value<QColor>(),
                 QColor(QStringLiteral("#1E242C")));
        QCOMPARE(objectProperty(price, "background")->property("color").value<QColor>(),
                 QColor(QStringLiteral("#1E242C")));
        // 2026-09-09 图标批适配：ActionButton contentItem 由 Text 改 Row{Image,Text}
        // （无 glyph 时 Image 隐藏，语义不变）——取字色下钻到行内 Text。
        auto* bookContent = objectProperty(book, "contentItem");
        QVERIFY(bookContent);
        QQuickItem* bookLabel = nullptr;
        const auto contentKids = bookContent->findChildren<QQuickItem*>();
        for (auto* kid : contentKids) {
            if (kid->property("color").isValid() && kid->property("text").isValid()) {
                bookLabel = kid;
                break;
            }
        }
        QVERIFY(bookLabel);
        QCOMPARE(bookLabel->property("color").value<QColor>(), QColor(Qt::white));
    }

    void comboFieldOpensAndSelectsWithKeyboard()
    {
        QQmlEngine engine;
        QQuickWindow window;
        window.resize(420, 540);
        auto fixture = createFixture(engine, window);
        QVERIFY(fixture);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* price = fixture->findChild<QQuickItem*>(QStringLiteral("price"));
        auto* popup = objectProperty(fixture.get(), "pricePopup");
        QVERIFY(price && popup);
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, centerOf(price));
        QTRY_VERIFY(popup->property("visible").toBool());
        QTest::keyClick(&window, Qt::Key_Down);
        QTest::keyClick(&window, Qt::Key_Return);
        QTRY_VERIFY(!popup->property("visible").toBool());
        QCOMPARE(price->property("currentIndex").toInt(), 1);
        QCOMPARE(price->property("displayText").toString(), QStringLiteral("1元以下"));
    }

    void editableCityKeepsEditTextContract()
    {
        QQmlEngine engine;
        QQuickWindow window;
        window.resize(420, 540);
        auto fixture = createFixture(engine, window);
        QVERIFY(fixture);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* city = fixture->findChild<QQuickItem*>(QStringLiteral("city"));
        QVERIFY(city);
        auto* input = qobject_cast<QQuickItem*>(objectProperty(city, "contentItem"));
        QVERIFY(input);
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, centerOf(input));
        QVERIFY(QMetaObject::invokeMethod(input, "selectAll"));
        for (Qt::Key key : {Qt::Key_T, Qt::Key_E, Qt::Key_S, Qt::Key_T})
            QTest::keyClick(&window, key);
        QCOMPARE(city->property("editText").toString(), QStringLiteral("test"));
    }

    void cardDoesNotSwallowChildActions()
    {
        QQmlEngine engine;
        QQuickWindow window;
        window.resize(420, 540);
        auto fixture = createFixture(engine, window);
        QVERIFY(fixture);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* book = fixture->findChild<QQuickItem*>(QStringLiteral("book"));
        auto* card = fixture->findChild<QQuickItem*>(QStringLiteral("card"));
        QVERIFY(book && card);
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, centerOf(book));
        QCOMPARE(fixture->property("actionClicks").toInt(), 1);
        QCOMPARE(fixture->property("cardClicks").toInt(), 0);
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
                         card->mapToScene(QPointF(card->width() - 8, 8)).toPoint());
        QCOMPARE(fixture->property("cardClicks").toInt(), 1);
    }

    void largeFontFitsControlHeight()
    {
        QQmlEngine engine;
        QQuickWindow window;
        auto fixture = createFixture(engine, window);
        QVERIFY(fixture);
        fixture->setProperty("fontScale", QStringLiteral("extraLarge"));
        auto* field = fixture->findChild<QQuickItem*>(QStringLiteral("field"));
        auto* book = fixture->findChild<QQuickItem*>(QStringLiteral("book"));
        QVERIFY(field && book);
        const qreal inputContentHeight = field->property("contentHeight").toReal();
        const qreal inputPadding = field->property("topPadding").toReal()
                                  + field->property("bottomPadding").toReal();
        QVERIFY(field->height() >= inputContentHeight + inputPadding);
        auto* label = qobject_cast<QQuickItem*>(objectProperty(book, "contentItem"));
        QVERIFY(label);
        const qreal buttonPadding = book->property("topPadding").toReal()
                                   + book->property("bottomPadding").toReal();
        QVERIFY(book->height() >= label->implicitHeight() + buttonPadding);
    }
};

QTEST_MAIN(QmlPlatformControlsTest)
#include "tst_qml_platform_controls.moc"
