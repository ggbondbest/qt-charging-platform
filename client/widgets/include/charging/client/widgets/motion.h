#pragma once

#include <QWidget>

class QAbstractButton;

namespace charging::client::motion {

// 动效令牌层：QSS 定形、这里定"呼吸"。与色 token 同一哲学——页面引用
// 具名语义（时长/步进），不写裸毫秒、不各自挑缓动曲线，全端动效才是一致
// 的。实现只用 Qt 6.2 文档化 API（QPropertyAnimation + QGraphicsOpacityEffect）。
//
// 关闭开关（animationsEnabled()==false 时所有 helper 直接跳过，控件保持
// 最终态）：offscreen 测试平台自动关闭（避免逐帧时序让 ctest 变 flaky、
// 让截图回归稳定），另可用环境变量 MOTION_REDUCED=1 全局关停——这同时
// 就是"减少动态效果"无障碍开关。

namespace duration {
inline constexpr int micro = 80;     // 按压等即时反馈
inline constexpr int enter = 180;    // 入场（淡入/滑入）
inline constexpr int exit = 120;     // 退场
inline constexpr int value = 140;    // 数值刷新的一次性提示脉冲
inline constexpr int breathe = 1600; // 循环呼吸的一个周期
} // namespace duration

inline constexpr int staggerStep = 40; // 列表逐卡入场的级间延迟
inline constexpr int staggerMax = 8;   // 超过 8 行不再累加延迟（首屏后的行不排队）

bool animationsEnabled();

// 淡入（可从延迟开始，供 stagger 用）。动画结束后自动摘除特效。
void fadeIn(QWidget* widget, int ms = duration::enter, int delayMs = 0);

// 数值变了给一次短促的透明度脉冲——比"每秒重播入场"克制得多。
// 只在文本确实变化处调用。
void valueUpdate(QWidget* widget);

// 缓慢呼吸（循环透明度微沉）。重复调用幂等；stopBreathing 撤销。
void startBreathing(QWidget* widget);
void stopBreathing(QWidget* widget);

// 按钮按下微沉/抬起回弹（透明度路线，不改几何、不动布局）。
// 在控件构造处调用一次即可；未启动动效时不装滤镜、零成本。
void attachPressDip(QAbstractButton* button);

} // namespace charging::client::motion
