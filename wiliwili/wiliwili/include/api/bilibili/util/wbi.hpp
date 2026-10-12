//
// Created for wiliwili based on bilibili-API-collect documentation
//

#pragma once

#include <cpr/cpr.h>
#include <functional>

#include "bilibili.h"

namespace bilibili {
/**
 * WBI签名模块
 *
 * 自2023年3月起，B站部分接口开始采用WBI签名鉴权，作为一种Web端风控手段
 * 该模块根据 bilibili-API-collect 文档实现WBI签名算法
 *
 * 签名过程：
 * 1. 获取最新的 img_key 和 sub_key
 * 2. 通过混淆算法计算出 mixin_key
 * 3. 将请求参数按照字典序排序并拼接，再附加 mixin_key 后计算MD5值，得到w_rid
 * 4. 将w_rid和wts(时间戳)添加到原始请求参数中
 */
namespace wbi {
/**
 * @brief 从API获取最新的 img_key 和 sub_key 以生成 wbi签名需要的 mixin_key
 *        每次调用此函数会检查缓存，如果距离上次更新时间不足1小时，则使用缓存值
 *        否则会从B站API获取最新的值
 * @param success 成功回调函数
 * @param error 错误回调函数
 */
void updateWbiKeys(const std::function<void()>& success, const ErrorCallback& error);

/**
 * 对参数进行 WBI 签名
 * 流程：
 * 1. 添加 wts 时间戳参数
 * 2. 对参数排序并过滤特殊字符
 * 3. 根据 mixin_key 计算签名并添加 w_rid 参数
 */
void encWbi(cpr::Parameters& params);
} // namespace wbi
} // namespace bilibili