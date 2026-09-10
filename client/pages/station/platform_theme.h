// platform_theme.h —— 全局 QSS 主题的安装入口（widgets 通道公共底座）。
// 调用方：本目录各 widgets 页面（找站/设置/筛选弹窗等）在构造时各调一次，
// 以兼容页面在单元测试中被独立构造、未经 app/main.cpp 启动的路径。
// 数据流向：纯本机资源读取（:/qss/client_platform.qss → qApp 全局样式表），
// 不经过服务桥、不产生任何 TCP 请求。
#pragma once

namespace charging::client::pages::station {

// 安装平台级 QSS 主题（client_platform.qss，成员 3 维护设计 token）。
//
// 主题资源登记在静态库中，需要在应用或测试启动时显式初始化一次；资源缺失时
// 静默跳过（未主题化的应用仍可运行），不影响业务行为。
void installPlatformTheme();

} // namespace charging::client::pages::station
