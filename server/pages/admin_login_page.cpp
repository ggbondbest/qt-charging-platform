#include "admin_login_page.h"

#include <QCheckBox>
#include <QEasingCurve>
#include <QEnterEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QParallelAnimationGroup>
#include <QPropertyAnimation>
#include <QPolygonF>
#include <QPushButton>
#include <QRadialGradient>
#include <QSettings>
#include <QStringList>
#include <QTimer>
#include <QVector>
#include <QVBoxLayout>
#include <QtMath>

namespace charging::server {

namespace {

// Draw the visual in code so the page stays self-contained, original, and
// consistent on the Ubuntu presentation VM without a raster asset dependency.
class LoginIllustrationWidget final : public QWidget
{
public:
    explicit LoginIllustrationWidget(QWidget* parent = nullptr) : QWidget(parent)
    {
        setMinimumHeight(210);
        setAccessibleName(QObject::tr("充电运营管理示意图"));
    }

    void setSlide(int slide)
    {
        if (slide_ == slide) {
            return;
        }
        slide_ = slide;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const qreal scale = qMin(width() / 680.0, height() / 300.0);
        painter.translate((width() - 680.0 * scale) / 2.0, (height() - 300.0 * scale) / 2.0);
        painter.scale(scale, scale);

        const QColor accent = slide_ == 1 ? QColor("#2fbdb3")
                              : slide_ == 2 ? QColor("#7a72e6") : QColor("#2b78f0");
        const QColor secondaryAccent = slide_ == 1 ? QColor("#56d4c9")
                                       : slide_ == 2 ? QColor("#a18dff") : QColor("#68a0f6");
        QRadialGradient glow(QPointF(465, 150), 270);
        glow.setColorAt(0.0, QColor(255, 255, 255, 175));
        glow.setColorAt(1.0, QColor(255, 255, 255, 0));
        painter.setPen(Qt::NoPen);
        painter.setBrush(glow);
        painter.drawEllipse(QRectF(160, 0, 510, 300));

        painter.setPen(QPen(QColor(255, 255, 255, 95), 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawArc(QRectF(355, -95, 520, 340), 196 * 16, 105 * 16);
        painter.drawArc(QRectF(280, 18, 410, 250), 202 * 16, 86 * 16);

        // Charging canopy and vehicle are a light background layer.
        painter.setBrush(QColor(239, 248, 255, 105));
        painter.setPen(QPen(QColor(255, 255, 255, 150), 2));
        painter.drawRoundedRect(QRectF(505, 22, 178, 34), 7, 7);
        painter.drawRect(QRectF(520, 56, 12, 104));
        painter.drawRect(QRectF(660, 56, 12, 116));
        painter.setBrush(QColor(248, 253, 255, 180));
        painter.drawRoundedRect(QRectF(507, 122, 142, 57), 23, 23);
        painter.drawRoundedRect(QRectF(544, 104, 76, 33), 13, 13);
        painter.setBrush(QColor(180, 211, 242, 200));
        painter.drawRoundedRect(QRectF(550, 109, 60, 20), 8, 8);
        painter.setBrush(QColor(115, 145, 184, 210));
        painter.drawEllipse(QRectF(531, 166, 25, 25));
        painter.drawEllipse(QRectF(611, 166, 25, 25));

        // Charger behind the dashboard.
        painter.setBrush(QColor(224, 241, 255, 175));
        painter.setPen(QPen(QColor(255, 255, 255, 180), 2));
        painter.drawRoundedRect(QRectF(277, 78, 64, 124), 11, 11);
        painter.setBrush(QColor(accent.red(), accent.green(), accent.blue(), 125));
        painter.drawRoundedRect(QRectF(291, 94, 36, 59), 5, 5);
        painter.setPen(QPen(QColor(255, 255, 255, 205), 3));
        painter.drawLine(QPointF(309, 105), QPointF(302, 126));
        painter.drawLine(QPointF(302, 126), QPointF(316, 124));
        painter.drawLine(QPointF(316, 124), QPointF(308, 144));

        // Foreground analytics dashboard.
        painter.save();
        painter.translate(374, 165);
        painter.rotate(4);
        painter.setBrush(QColor(253, 254, 255, 238));
        painter.setPen(QPen(QColor(255, 255, 255, 220), 2));
        painter.drawRoundedRect(QRectF(0, 0, 270, 154), 13, 13);
        painter.setBrush(QColor(31, 49, 77, 235));
        painter.drawRoundedRect(QRectF(0, 0, 43, 154), 13, 13);
        painter.fillRect(QRectF(34, 0, 10, 154), QColor(31, 49, 77, 235));
        painter.setPen(QPen(QColor(159, 191, 234, 190), 2));
        for (int row = 0; row < 5; ++row) {
            painter.drawLine(QPointF(12, 27 + row * 19), QPointF(30, 27 + row * 19));
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(237, 245, 255));
        painter.drawRoundedRect(QRectF(56, 15, 56, 32), 5, 5);
        painter.drawRoundedRect(QRectF(121, 15, 56, 32), 5, 5);
        painter.drawRoundedRect(QRectF(186, 15, 56, 32), 5, 5);
        painter.setBrush(accent);
        painter.drawRoundedRect(QRectF(62, 21, 18, 18), 4, 4);
        painter.setBrush(QColor(68, 199, 188));
        painter.drawRoundedRect(QRectF(127, 21, 18, 18), 4, 4);
        painter.setBrush(secondaryAccent);
        painter.drawRoundedRect(QRectF(192, 21, 18, 18), 4, 4);
        painter.setBrush(QColor(248, 251, 255));
        painter.drawRoundedRect(QRectF(56, 57, 125, 72), 6, 6);
        painter.drawRoundedRect(QRectF(189, 57, 56, 72), 6, 6);
        painter.setPen(QPen(accent, 2));
        QPainterPath chart;
        chart.moveTo(63, 112);
        chart.lineTo(78, 102);
        chart.lineTo(92, 106);
        chart.lineTo(109, 88);
        chart.lineTo(125, 96);
        chart.lineTo(143, 78);
        chart.lineTo(171, 84);
        painter.drawPath(chart);
        painter.setPen(Qt::NoPen);
        painter.setBrush(slide_ == 1 ? accent : QColor(67, 198, 186));
        painter.drawPie(QRectF(199, 72, 35, 35), 20 * 16, 140 * 16);
        painter.setBrush(QColor(250, 177, 55));
        painter.drawPie(QRectF(199, 72, 35, 35), 165 * 16, 88 * 16);
        painter.setBrush(QColor(140, 188, 255));
        painter.drawPie(QRectF(199, 72, 35, 35), 258 * 16, 82 * 16);
        painter.restore();
    }

private:
    int slide_ = 0;
};

QLabel* createLabel(const QString& text, const QString& style, QWidget* parent);

class LoginFeatureIconWidget final : public QWidget
{
public:
    explicit LoginFeatureIconWidget(int type, QWidget* parent = nullptr) : QWidget(parent), type_(type)
    {
        setFixedSize(76, 76);
        setAccessibleName(QObject::tr("平台能力图标"));
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 255, 255, 72));
        painter.drawRoundedRect(QRectF(0, 0, width(), height()), 17, 17);

        const QColor accent = type_ < 3 ? QColor("#dbe8ff")
                             : type_ < 6 ? QColor("#bcfff2") : QColor("#ede6ff");
        painter.setPen(QPen(accent, 4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const qreal centerX = width() / 2.0;
        const qreal centerY = height() / 2.0;
        switch (type_) {
        case 0:
            painter.drawLine(QPointF(centerX - 15, centerY + 14), QPointF(centerX - 15, centerY + 2));
            painter.drawLine(QPointF(centerX, centerY + 14), QPointF(centerX, centerY - 11));
            painter.drawLine(QPointF(centerX + 15, centerY + 14), QPointF(centerX + 15, centerY - 3));
            break;
        case 1:
        {
            QPolygonF trend;
            trend << QPointF(centerX - 17, centerY + 10) << QPointF(centerX - 5, centerY - 2)
                  << QPointF(centerX + 4, centerY + 5) << QPointF(centerX + 18, centerY - 12);
            painter.drawPolyline(trend);
            painter.drawLine(QPointF(centerX + 18, centerY - 12), QPointF(centerX + 10, centerY - 12));
            painter.drawLine(QPointF(centerX + 18, centerY - 12), QPointF(centerX + 18, centerY - 4));
            break;
        }
        case 2:
            painter.drawRoundedRect(QRectF(centerX - 13, centerY - 14, 26, 26), 4, 4);
            painter.drawLine(QPointF(centerX - 13, centerY - 5), QPointF(centerX, centerY + 1));
            painter.drawLine(QPointF(centerX, centerY + 1), QPointF(centerX + 13, centerY - 5));
            painter.drawLine(QPointF(centerX, centerY + 1), QPointF(centerX, centerY + 12));
            break;
        case 3:
            painter.drawRoundedRect(QRectF(centerX - 11, centerY - 16, 22, 31), 5, 5);
            painter.drawLine(QPointF(centerX - 4, centerY - 7), QPointF(centerX + 3, centerY - 7));
            painter.drawLine(QPointF(centerX - 4, centerY), QPointF(centerX + 3, centerY));
            painter.drawArc(QRectF(centerX + 5, centerY - 4, 15, 20), 275 * 16, 180 * 16);
            break;
        case 4:
            painter.drawEllipse(QRectF(centerX - 11, centerY - 15, 22, 22));
            painter.drawEllipse(QRectF(centerX - 3, centerY - 7, 6, 6));
            painter.drawLine(QPointF(centerX - 7, centerY + 4), QPointF(centerX, centerY + 16));
            painter.drawLine(QPointF(centerX + 7, centerY + 4), QPointF(centerX, centerY + 16));
            break;
        case 5:
            painter.drawLine(QPointF(centerX - 13, centerY + 13), QPointF(centerX + 10, centerY - 10));
            painter.drawEllipse(QRectF(centerX - 17, centerY + 9, 8, 8));
            painter.drawArc(QRectF(centerX + 1, centerY - 18, 21, 21), 35 * 16, 240 * 16);
            break;
        case 6:
            painter.drawEllipse(QRectF(centerX - 7, centerY - 16, 14, 14));
            painter.drawArc(QRectF(centerX - 17, centerY - 1, 34, 28), 25 * 16, 130 * 16);
            break;
        case 7:
            painter.drawRoundedRect(QRectF(centerX - 13, centerY - 16, 26, 32), 4, 4);
            for (int line = 0; line < 3; ++line) {
                painter.drawLine(QPointF(centerX - 6, centerY - 8 + line * 8), QPointF(centerX + 7, centerY - 8 + line * 8));
            }
            break;
        default:
            painter.drawRoundedRect(QRectF(centerX - 17, centerY - 11, 34, 23), 5, 5);
            painter.drawLine(QPointF(centerX - 12, centerY + 3), QPointF(centerX + 1, centerY + 3));
            painter.drawLine(QPointF(centerX + 8, centerY - 11), QPointF(centerX + 8, centerY + 12));
            break;
        }
    }

private:
    int type_ = 0;
};

QWidget* createFeatureRow(int iconType, const QString& title,
                          const QString& description, QWidget* parent)
{
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(16);
    layout->addWidget(new LoginFeatureIconWidget(iconType, row));
    auto* textLayout = new QVBoxLayout();
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(4);
    textLayout->addWidget(createLabel(title,
                                      QStringLiteral("color:#ffffff; font-size:17px; font-weight:700;"), row));
    auto* descriptionLabel = createLabel(description,
                                         QStringLiteral("color:rgba(255,255,255,0.84); font-size:14px;"), row);
    descriptionLabel->setWordWrap(true);
    textLayout->addWidget(descriptionLabel);
    layout->addLayout(textLayout, 1);
    return row;
}

void setCarouselDotActive(QPushButton* button, bool active)
{
    button->setStyleSheet(active
                              ? QStringLiteral("QPushButton { background:rgba(255,255,255,0.96); border:none;"
                                               " border-radius:2px; min-width:22px; max-width:22px;"
                                               " min-height:4px; max-height:4px; padding:0; }")
                              : QStringLiteral("QPushButton { background:rgba(255,255,255,0.54); border:none;"
                                               " border-radius:2px; min-width:6px; max-width:6px;"
                                               " min-height:4px; max-height:4px; padding:0; }"));
}

// The carousel is intentionally composed from native Qt widgets and the
// original LoginIllustrationWidget.  It remains within member 4's pages
// boundary and does not add shared image or resource dependencies.
class LoginCarouselWidget final : public QWidget
{
public:
    explicit LoginCarouselWidget(QWidget* parent = nullptr)
        : QWidget(parent), viewport_(new QWidget(this)), transition_(new QParallelAnimationGroup(this)), autoTimer_(this)
    {
        setMouseTracking(true);
        setAccessibleName(QObject::tr("管理平台优势轮播"));
        viewport_->setAttribute(Qt::WA_TransparentForMouseEvents);
        struct SlideContent {
            QString eyebrow;
            QString title;
            QString description;
            QStringList featureTitles;
            QStringList featureDescriptions;
        };
        const QVector<SlideContent> slides = {
            {tr("数据驱动 · 智能运营"), tr("全局运营，一屏掌控"),
             tr("实时聚合营收、设备、订单与连接数据，\n帮助运营团队快速发现趋势并做出决策。"),
             {tr("核心指标实时监控"), tr("营收趋势与异常预警"), tr("订单与设备联动分析")},
             {tr("营收、设备、连接等关键数据实时掌握。"), tr("智能识别波动，及时预警运营风险。"),
              tr("从订单到设备，打通业务数据洞察。")}},
            {tr("设备协同 · 站点可视"), tr("电桩电站，统一管理"),
             tr("统一查看电桩状态、充电站分布、告警与运维任务，\n让设备运营更及时、更有序。"),
             {tr("电桩状态一目了然"), tr("电站分布与容量管理"), tr("告警联动与运维效率")},
             {tr("在线、离线、故障状态实时可见。"), tr("站点布局、车位与负载情况集中掌握。"),
              tr("异常预警、工单处理与维护闭环高效协同。")}},
            {tr("用户增长 · 订单闭环"), tr("用户订单，高效协同"),
             tr("连接用户、车辆、订单与支付流程，\n帮助平台沉淀用户资产并提升运营效率。"),
             {tr("用户画像与分层运营"), tr("订单流程全程追踪"), tr("支付结算与服务体验")},
             {tr("车主信息、活跃度与偏好集中管理。"), tr("从发起、充电到结算，订单状态清晰可控。"),
              tr("支付记录、退款处理与会员权益统一管理。")}},
        };

        for (int index = 0; index < slides.size(); ++index) {
            const SlideContent& slide = slides.at(index);
            auto* page = new QWidget(viewport_);
            page->setAttribute(Qt::WA_TransparentForMouseEvents);
            auto* layout = new QVBoxLayout(page);
            layout->setContentsMargins(54, 70, 54, 34);
            layout->setSpacing(0);
            layout->addWidget(createLabel(slide.eyebrow,
                                          QStringLiteral("color:rgba(255,255,255,0.88); font-size:17px; font-weight:600;"), page));
            auto* title = createLabel(slide.title,
                                      QStringLiteral("color:#ffffff; font-size:38px; font-weight:700;"), page);
            title->setContentsMargins(0, 20, 0, 0);
            layout->addWidget(title);
            auto* description = createLabel(slide.description,
                                            QStringLiteral("color:rgba(255,255,255,0.90); font-size:17px;"), page);
            description->setContentsMargins(0, 20, 0, 0);
            layout->addWidget(description);
            layout->addSpacing(30);

            auto* visualLayout = new QHBoxLayout();
            visualLayout->setContentsMargins(0, 0, 0, 0);
            visualLayout->setSpacing(20);
            auto* featureColumn = new QVBoxLayout();
            featureColumn->setContentsMargins(0, 0, 0, 0);
            featureColumn->setSpacing(22);
            for (int featureIndex = 0; featureIndex < slide.featureTitles.size(); ++featureIndex) {
                featureColumn->addWidget(createFeatureRow(index * 3 + featureIndex,
                                                          slide.featureTitles.at(featureIndex),
                                                          slide.featureDescriptions.at(featureIndex), page));
            }
            featureColumn->addStretch();
            auto* featureWidget = new QWidget(page);
            featureWidget->setLayout(featureColumn);
            featureWidget->setMinimumWidth(235);
            featureWidget->setMaximumWidth(335);
            visualLayout->addWidget(featureWidget, 5);
            auto* illustration = new LoginIllustrationWidget(page);
            illustration->setSlide(index);
            illustration->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
            illustration->setMinimumWidth(220);
            visualLayout->addWidget(illustration, 6);
            layout->addLayout(visualLayout, 1);

            auto* footer = new QHBoxLayout();
            footer->setContentsMargins(0, 0, 0, 0);
            footer->addWidget(createLabel(tr("连接能源 · 服务未来"),
                                          QStringLiteral("color:rgba(255,255,255,0.82); font-size:13px; letter-spacing:2px;"), page));
            footer->addStretch();
            footer->addWidget(createLabel(tr("充电平台运营管理系统"),
                                          QStringLiteral("color:rgba(255,255,255,0.68); font-size:13px;"), page));
            layout->addLayout(footer);
            pages_.append(page);
        }

        connect(transition_, &QParallelAnimationGroup::finished, this, [this]() {
            currentSlide_ = targetSlide_;
            isTransitioning_ = false;
            updateIndicators(currentSlide_);
            arrangePages();
            if (!underMouse()) {
                autoTimer_.start();
            }
        });

        for (int index = 0; index < pages_.size(); ++index) {
            auto* indicator = new QPushButton(this);
            indicator->setFocusPolicy(Qt::NoFocus);
            indicator->setCursor(Qt::PointingHandCursor);
            indicator->setAccessibleName(QObject::tr("切换到第 %1 张运营展示").arg(index + 1));
            connect(indicator, &QPushButton::clicked, this, [this, index]() { transitionTo(index); });
            indicators_.append(indicator);
        }
        updateIndicators(0);
        autoTimer_.setInterval(4500);
        connect(&autoTimer_, &QTimer::timeout, this, [this]() {
            transitionTo((currentSlide_ + 1) % pages_.size());
        });
        autoTimer_.start();
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QWidget::resizeEvent(event);
        viewport_->setGeometry(rect());
        if (!isTransitioning_) {
            arrangePages();
        }
        layoutIndicators(isTransitioning_ ? targetSlide_ : currentSlide_);
    }

    void enterEvent(QEnterEvent* event) override
    {
        autoTimer_.stop();
        QWidget::enterEvent(event);
    }

    void leaveEvent(QEvent* event) override
    {
        QWidget::leaveEvent(event);
        if (!isTransitioning_) {
            autoTimer_.start();
        }
    }

private:
    void arrangePages()
    {
        for (int index = 0; index < pages_.size(); ++index) {
            pages_.at(index)->setGeometry(index == currentSlide_ ? 0 : width(), 0, width(), height());
        }
    }

    void layoutIndicators(int activeIndex)
    {
        const int activeWidth = 22;
        const int inactiveWidth = 6;
        const int spacing = 8;
        int totalWidth = activeWidth + inactiveWidth * 2 + spacing * 2;
        int x = (width() - totalWidth) / 2;
        const int y = height() - 27;
        for (int index = 0; index < indicators_.size(); ++index) {
            const int indicatorWidth = index == activeIndex ? activeWidth : inactiveWidth;
            indicators_.at(index)->setGeometry(x, y, indicatorWidth, 4);
            x += indicatorWidth + spacing;
        }
    }

    void transitionTo(int nextSlide)
    {
        if (isTransitioning_ || nextSlide == currentSlide_ || nextSlide < 0 || nextSlide >= pages_.size()) {
            return;
        }
        autoTimer_.stop();
        targetSlide_ = nextSlide;
        isTransitioning_ = true;
        updateIndicators(targetSlide_);
        transition_->clear();
        auto* currentAnimation = new QPropertyAnimation(pages_.at(currentSlide_), "pos", transition_);
        currentAnimation->setDuration(500);
        currentAnimation->setEasingCurve(QEasingCurve::OutCubic);
        currentAnimation->setStartValue(QPoint(0, 0));
        currentAnimation->setEndValue(QPoint(-width(), 0));
        auto* incomingAnimation = new QPropertyAnimation(pages_.at(targetSlide_), "pos", transition_);
        incomingAnimation->setDuration(500);
        incomingAnimation->setEasingCurve(QEasingCurve::OutCubic);
        incomingAnimation->setStartValue(QPoint(width(), 0));
        incomingAnimation->setEndValue(QPoint(0, 0));
        pages_.at(targetSlide_)->move(width(), 0);
        transition_->addAnimation(currentAnimation);
        transition_->addAnimation(incomingAnimation);
        transition_->start();
    }

    void updateIndicators(int activeIndex)
    {
        for (int index = 0; index < indicators_.size(); ++index) {
            const bool active = index == activeIndex;
            auto* indicator = indicators_.at(index);
            indicator->setEnabled(!isTransitioning_);
            setCarouselDotActive(indicator, active);
        }
        layoutIndicators(activeIndex);
    }

    QWidget* viewport_ = nullptr;
    QVector<QWidget*> pages_;
    QVector<QPushButton*> indicators_;
    QParallelAnimationGroup* transition_ = nullptr;
    QTimer autoTimer_;
    int currentSlide_ = 0;
    int targetSlide_ = 0;
    bool isTransitioning_ = false;
};

QLabel* createLabel(const QString& text, const QString& style, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setStyleSheet(style);
    return label;
}

QFrame* createInputShell(QLineEdit* lineEdit, const QString& iconText, QWidget* parent)
{
    auto* shell = new QFrame(parent);
    shell->setObjectName(QStringLiteral("loginInputShell"));
    auto* layout = new QHBoxLayout(shell);
    layout->setContentsMargins(14, 0, 9, 0);
    layout->setSpacing(8);
    auto* icon = createLabel(iconText, QStringLiteral("color:#7184a0; font-size:21px; font-weight:600;"), shell);
    icon->setFixedWidth(22);
    icon->setAlignment(Qt::AlignCenter);
    lineEdit->setParent(shell);
    lineEdit->setObjectName(QStringLiteral("loginInput"));
    layout->addWidget(icon);
    layout->addWidget(lineEdit, 1);
    return shell;
}

} // namespace

AdminLoginPage::AdminLoginPage(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("adminLoginPage"));
    setStyleSheet(QStringLiteral(
        "QWidget#adminLoginPage { background:qlineargradient(x1:0, y1:0, x2:1, y2:1,"
        " stop:0 #f7fbff, stop:0.48 #e9f4ff, stop:1 #d6eaff); }"
        "QFrame#loginSurface { background:#ffffff; border:none; }"
        "QFrame#loginFormPanel { background:#ffffff; }"
        "QFrame#loginBrandPanel { background:qlineargradient(x1:0, y1:0, x2:1, y2:1,"
        " stop:0 #1376e8, stop:0.54 #198fe9, stop:1 #4ccdde); }"
        "QFrame#loginInputShell { background:#ffffff; border:1px solid #d6e0ef; border-radius:9px; min-height:50px; }"
        "QLineEdit#loginInput { background:transparent; border:none; color:#1e3152; font-size:15px; min-height:48px; }"
        "QLineEdit#loginInput:focus { border:none; }"
        "QCheckBox { color:#5f7190; font-size:14px; spacing:8px; }"
        "QCheckBox::indicator { width:19px; height:19px; border:1px solid #9aacc6; border-radius:4px; background:#ffffff; }"
        "QCheckBox::indicator:checked { background:#2878f0; border-color:#2878f0; }"
        "QPushButton#loginButton { background:#2878f0; border:none; border-radius:9px; color:#ffffff;"
        " font-size:16px; font-weight:700; min-height:48px; padding:0 18px; }"
        "QPushButton#loginButton:hover { background:#1769e8; }"
        "QPushButton#loginButton:disabled { background:#9abfe7; }"
        "QPushButton#passwordVisibilityButton { color:#7184a0; border:none; background:transparent;"
        " min-width:38px; min-height:38px; font-size:13px; }"
        "QPushButton#passwordVisibilityButton:hover { color:#2878f0; }"
        "QLabel#loginErrorLabel { color:#c9372c; font-size:13px; padding:2px 0; }"));

    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    auto* surface = new QFrame(this);
    surface->setObjectName(QStringLiteral("loginSurface"));
    outerLayout->addWidget(surface, 1);
    auto* rootLayout = new QHBoxLayout(surface);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto* formPanel = new QFrame(surface);
    formPanel->setObjectName(QStringLiteral("loginFormPanel"));
    auto* formPanelLayout = new QVBoxLayout(formPanel);
    formPanelLayout->setContentsMargins(66, 44, 66, 34);
    auto* card = new QFrame(formPanel);
    card->setMaximumWidth(430);
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(12);

    auto* productRow = new QHBoxLayout();
    auto* lightning = createLabel(tr("ϟ"), QStringLiteral("color:#2878f0; font-size:48px; font-weight:700;"), card);
    lightning->setFixedWidth(48);
    lightning->setAlignment(Qt::AlignCenter);
    auto* productText = new QVBoxLayout();
    productText->setSpacing(0);
    productText->addWidget(createLabel(tr("充电平台"),
                                       QStringLiteral("color:#172b53; font-size:20px; font-weight:700;"), card));
    productText->addWidget(createLabel(tr("运营管理系统"),
                                       QStringLiteral("color:#607493; font-size:15px;"), card));
    productRow->addWidget(lightning);
    productRow->addLayout(productText);
    productRow->addStretch();

    auto* titleLabel = createLabel(tr("欢迎登录"),
                                   QStringLiteral("color:#11274d; font-size:31px; font-weight:700;"), card);
    auto* subtitleLabel = createLabel(tr("充电平台 PC 运营管理端"),
                                      QStringLiteral("color:#506584; font-size:17px;"), card);
    auto* valueLabel = createLabel(tr("高效 · 安全 · 智能 · 便捷"),
                                   QStringLiteral("color:#8a9ab2; font-size:14px;"), card);

    usernameLineEdit_ = new QLineEdit(card);
    usernameLineEdit_->setPlaceholderText(tr("管理员账号"));
    usernameLineEdit_->setAccessibleName(tr("管理员账号"));
    auto* usernameShell = createInputShell(usernameLineEdit_, tr("♙"), card);
    // Keep the stable object name used by the admin login integration test.
    usernameLineEdit_->setObjectName(QStringLiteral("usernameLineEdit"));
    passwordLineEdit_ = new QLineEdit(card);
    passwordLineEdit_->setPlaceholderText(tr("密码"));
    passwordLineEdit_->setEchoMode(QLineEdit::Password);
    passwordLineEdit_->setAccessibleName(tr("管理员密码"));
    auto* passwordShell = createInputShell(passwordLineEdit_, tr("♧"), card);
    // Keep the stable object name used by the admin login integration test.
    passwordLineEdit_->setObjectName(QStringLiteral("passwordLineEdit"));
    auto* passwordShellLayout = qobject_cast<QHBoxLayout*>(passwordShell->layout());
    auto* visibilityButton = new QPushButton(tr("显示"), passwordShell);
    visibilityButton->setObjectName(QStringLiteral("passwordVisibilityButton"));
    visibilityButton->setCheckable(true);
    visibilityButton->setAccessibleName(tr("显示管理员密码"));
    passwordShellLayout->addWidget(visibilityButton);
    connect(visibilityButton, &QPushButton::toggled, this, [this, visibilityButton](bool visible) {
        passwordLineEdit_->setEchoMode(visible ? QLineEdit::Normal : QLineEdit::Password);
        visibilityButton->setText(visible ? tr("隐藏") : tr("显示"));
        visibilityButton->setAccessibleName(visible ? tr("隐藏管理员密码") : tr("显示管理员密码"));
    });

    auto* accountRow = new QHBoxLayout();
    rememberAccountCheckBox_ = new QCheckBox(tr("记住账号"), card);
    rememberAccountCheckBox_->setObjectName(QStringLiteral("rememberAccountCheckBox"));
    rememberAccountCheckBox_->setAccessibleName(tr("记住管理员账号"));
    QSettings loginSettings;
    const bool shouldRememberAccount = loginSettings.value(
        QStringLiteral("adminLogin/rememberAccount"), false).toBool();
    rememberAccountCheckBox_->setChecked(shouldRememberAccount);
    if (shouldRememberAccount) {
        usernameLineEdit_->setText(loginSettings.value(QStringLiteral("adminLogin/username")).toString());
    }
    auto* passwordHint = createLabel(tr("密码由管理员统一维护"),
                                     QStringLiteral("color:#2878f0; font-size:13px;"), card);
    accountRow->addWidget(rememberAccountCheckBox_);
    accountRow->addStretch();
    accountRow->addWidget(passwordHint);
    connect(rememberAccountCheckBox_, &QCheckBox::toggled, this, [](bool checked) {
        QSettings settings;
        settings.setValue(QStringLiteral("adminLogin/rememberAccount"), checked);
        if (!checked) {
            settings.remove(QStringLiteral("adminLogin/username"));
        }
    });

    errorLabel_ = new QLabel(card);
    errorLabel_->setObjectName(QStringLiteral("loginErrorLabel"));
    errorLabel_->setWordWrap(true);
    errorLabel_->hide();
    loginButton_ = new QPushButton(tr("登录   →"), card);
    loginButton_->setObjectName(QStringLiteral("loginButton"));
    loginButton_->setDefault(true);

    auto* dividerRow = new QHBoxLayout();
    auto* leftLine = new QFrame(card);
    leftLine->setFrameShape(QFrame::HLine);
    leftLine->setStyleSheet(QStringLiteral("color:#e4ebf4;"));
    auto* dividerText = createLabel(tr("其他登录方式"), QStringLiteral("color:#8b9ab1; font-size:13px;"), card);
    auto* rightLine = new QFrame(card);
    rightLine->setFrameShape(QFrame::HLine);
    rightLine->setStyleSheet(QStringLiteral("color:#e4ebf4;"));
    dividerRow->addWidget(leftLine, 1);
    dividerRow->addWidget(dividerText);
    dividerRow->addWidget(rightLine, 1);
    auto* ssoHint = createLabel(tr("统一身份登录将在管理员认证服务接入后启用"),
                                QStringLiteral("color:#637795; font-size:13px;"), card);
    ssoHint->setAlignment(Qt::AlignCenter);

    auto* footerRow = new QHBoxLayout();
    footerRow->setContentsMargins(0, 0, 0, 0);
    footerRow->addWidget(createLabel(tr("© 2026 充电平台运营管理系统 · v1.0.0"),
                                     QStringLiteral("color:#8b9ab1; font-size:12px;"), card));
    footerRow->addStretch();
    footerRow->addWidget(createLabel(tr("帮助中心　|　联系我们"),
                                     QStringLiteral("color:#8b9ab1; font-size:12px;"), card));

    cardLayout->addLayout(productRow);
    cardLayout->addSpacing(35);
    cardLayout->addWidget(titleLabel);
    cardLayout->addWidget(subtitleLabel);
    cardLayout->addWidget(valueLabel);
    cardLayout->addSpacing(20);
    cardLayout->addWidget(usernameShell);
    cardLayout->addWidget(passwordShell);
    cardLayout->addLayout(accountRow);
    cardLayout->addWidget(errorLabel_);
    cardLayout->addSpacing(5);
    cardLayout->addWidget(loginButton_);
    cardLayout->addSpacing(18);
    cardLayout->addLayout(dividerRow);
    cardLayout->addWidget(ssoHint);
    cardLayout->addStretch();
    cardLayout->addLayout(footerRow);
    formPanelLayout->addWidget(card, 1, Qt::AlignHCenter);

    auto* brandPanel = new QFrame(surface);
    brandPanel->setObjectName(QStringLiteral("loginBrandPanel"));
    brandPanel->setMinimumWidth(570);
    auto* brandLayout = new QVBoxLayout(brandPanel);
    brandLayout->setContentsMargins(0, 0, 0, 0);
    brandLayout->setSpacing(0);
    brandLayout->addWidget(new LoginCarouselWidget(brandPanel), 1);

    rootLayout->addWidget(formPanel, 4);
    rootLayout->addWidget(brandPanel, 7);

    connect(loginButton_, &QPushButton::clicked, this, &AdminLoginPage::handleLoginClicked);
    connect(passwordLineEdit_, &QLineEdit::returnPressed, this,
            &AdminLoginPage::handleLoginClicked);
}

void AdminLoginPage::setBusy(bool busy)
{
    usernameLineEdit_->setEnabled(!busy);
    passwordLineEdit_->setEnabled(!busy);
    rememberAccountCheckBox_->setEnabled(!busy);
    loginButton_->setEnabled(!busy);
    loginButton_->setText(busy ? tr("正在验证…") : tr("登录   →"));
}

void AdminLoginPage::showError(const QString& message)
{
    errorLabel_->setText(message);
    errorLabel_->show();
}

void AdminLoginPage::resetForm()
{
    passwordLineEdit_->clear();
    errorLabel_->clear();
    errorLabel_->hide();
    usernameLineEdit_->setFocus();
}

void AdminLoginPage::handleLoginClicked()
{
    errorLabel_->hide();
    const QString username = usernameLineEdit_->text().trimmed();
    const QString password = passwordLineEdit_->text();
    if (username.isEmpty() || password.isEmpty()) {
        showError(tr("请输入管理员账号和密码。"));
        return;
    }
    if (rememberAccountCheckBox_->isChecked()) {
        QSettings settings;
        settings.setValue(QStringLiteral("adminLogin/username"), username);
    }
    emit loginSubmitted(username, password);
}

} // namespace charging::server
