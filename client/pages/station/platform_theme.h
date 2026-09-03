#pragma once

namespace charging::client::pages::station {

// 安装平台级 QSS 主题（client_platform.qss，成员 3 维护设计 token）。
//
// 主题资源登记在静态库中，需要在应用或测试启动时显式初始化一次；资源缺失时
// 静默跳过（未主题化的应用仍可运行），不影响业务行为。
void installPlatformTheme();

} // namespace charging::client::pages::station
