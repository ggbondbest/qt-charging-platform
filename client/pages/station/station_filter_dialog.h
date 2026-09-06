#pragma once

#include "services/station/station_query_service.h"

#include <QDialog>

class QPushButton;
class QVBoxLayout;

namespace charging::client::pages::station {

// 高级筛选弹窗（成员 2，迭代 3）：8 组条件（距离/运营状态/运营商/电站类型/
// 停车费/特色功能/充电桩类型/充电桩电压），底部“重置 + 确定”。
//
// 交互口径：
// - 距离组单选（点选另一项自动取消前项，再点自己可取消 = 不限）；其余组
//   多选（组内 OR，组间 AND——匹配语义在服务层 applyStationFilter）；
// - “确定”发 applied(criteria) 并关闭；“重置”仅清空弹窗内勾选（回“未限制”
//   态），不自动下发——用户再点确定才生效（规格：确定下发过滤、重置清空）；
// - 非模态、WA_DeleteOnClose，调用方持 QPointer 去重（同设置页车辆弹窗先例）。
//
// 样式：页面级 QSS 不向 QDialog 级联（Qt 已知口径），本弹窗自带局部样式串，
// token 全部取自全局规范（绿 #00B578 / 浅绿选中 #EAF9F2 / 灰底 #F4F6F8 等）。
class StationFilterDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit StationFilterDialog(const services::station::StationFilterCriteria& initial,
                                 QWidget* parent = nullptr);

    // 测试/回显接缝：弹窗当前勾选状态投影为条件对象。
    services::station::StationFilterCriteria currentCriteria() const;
    int checkedCountForGroup(const QString& groupKey) const;
    void setGroupSelectionForTesting(const QString& groupKey, const QStringList& options);

signals:
    // 点击“确定”：携带最终条件并关闭。
    void applied(const charging::client::services::station::StationFilterCriteria& criteria);

private:
    void onChipClicked(QPushButton* chip);

    // 每组：key（objectName 组成部分）+ 标题 + 选项 + 是否单选。
    // chip 通过动态属性 filterGroup(QString)/filterIndex(int 选项下标) 归属。
    struct GroupSpec
    {
        QString key;
        QString title;
        QStringList options; // 展示文本（非距离组同时是匹配值）
        bool exclusive = false;
    };
    static QVector<GroupSpec> groupSpecs();

    QVector<QPushButton*> chipsOfGroup(const QString& groupKey) const;

    QVBoxLayout* contentLayout_ = nullptr;
};

} // namespace charging::client::pages::station
