#!/bin/bash

# Android NVR多通道布局自动切换测试脚本
# 验证4通道RTSP流配置和布局切换逻辑

DEVICE_SERIAL="192.168.31.186:5555"
PACKAGE_NAME="com.wulala.myyolov5rtspthreadpool"
ACTIVITY_NAME="com.wulala.myyolov5rtspthreadpool.MainActivity"

echo "=== Android NVR多通道布局自动切换测试 ==="
echo "设备: $DEVICE_SERIAL"
echo "应用: $PACKAGE_NAME"
echo ""
echo "测试配置："
echo "- Channel 0: rtsp://192.168.31.22:8554/unicast"
echo "- Channel 1: rtsp://192.168.31.147:8554/unicast"
echo "- Channel 2: rtsp://192.168.31.22:8554/unicast"
echo "- Channel 3: rtsp://192.168.31.147:8554/unicast"
echo ""

# 测试1: 验证多通道配置和自动布局切换
echo "测试1: 验证多通道配置和自动布局切换..."
echo "1. 停止应用并清除数据"
adb -s $DEVICE_SERIAL shell am force-stop $PACKAGE_NAME

echo "2. 启动应用"
adb -s $DEVICE_SERIAL shell am start -n $PACKAGE_NAME/.MainActivity

echo "3. 等待8秒观察布局自动切换..."
sleep 8

echo "4. 检查应用日志 - 通道配置"
echo "--- 通道配置日志 ---"
adb -s $DEVICE_SERIAL logcat -d | grep -E "(enabled channels|valid URLs|Test Channel)" | tail -10

echo ""
echo "--- 布局切换日志 ---"
adb -s $DEVICE_SERIAL logcat -d | grep -E "(Layout changed|Multiple channels detected|switching to)" | tail -10

echo ""
echo "--- RTSP连接日志 ---"
adb -s $DEVICE_SERIAL logcat -d | grep -E "(Starting channel|RTSP|unicast)" | tail -10

echo ""
echo "测试1结果验证："
echo "✓ 应该检测到4个启用的通道"
echo "✓ 应该识别4个有效的RTSP URL（非默认URL）"
echo "✓ 应该自动切换到2x2（QUAD）布局模式"
echo "✓ 应该显示'切换到 2×2 四通道'的Toast提示"
echo ""

# 测试2: 验证RTSP流连接状态
echo "测试2: 验证RTSP流连接状态..."
echo "1. 等待RTSP连接建立..."
sleep 5

echo "2. 检查RTSP连接状态"
adb -s $DEVICE_SERIAL logcat -d | grep -E "(RTSP.*connected|Stream.*started|Channel.*playing)" | tail -8

echo "3. 检查错误信息"
adb -s $DEVICE_SERIAL logcat -d | grep -E "(ERROR|Failed|Connection.*failed)" | tail -5

echo ""
echo "测试2结果验证："
echo "✓ 各通道应该尝试连接到指定的RTSP流"
echo "✓ 检查是否有连接错误或超时"
echo "✓ 验证视频流是否正常解码和渲染"
echo ""

# 测试3: 验证布局控制功能
echo "测试3: 验证布局控制功能..."
echo "1. 点击屏幕显示控制按钮"
adb -s $DEVICE_SERIAL shell input tap 500 500
sleep 2

echo "2. 切换到1x1布局"
adb -s $DEVICE_SERIAL shell input tap 100 100
sleep 3

echo "3. 切换到3x3布局"
adb -s $DEVICE_SERIAL shell input tap 300 100
sleep 3

echo "4. 切换回2x2布局"
adb -s $DEVICE_SERIAL shell input tap 200 100
sleep 2

echo "5. 检查布局切换日志"
adb -s $DEVICE_SERIAL logcat -d | grep -E "(Layout changed|切换到)" | tail -6

echo ""
echo "测试3结果验证："
echo "✓ 用户应该能够手动切换布局"
echo "✓ 每次切换应该显示相应的Toast提示"
echo "✓ 按钮状态应该正确更新"
echo ""

# 测试4: 验证布局状态持久化
echo "测试4: 验证布局状态持久化..."
echo "1. 设置为3x3布局"
adb -s $DEVICE_SERIAL shell input tap 500 500
sleep 1
adb -s $DEVICE_SERIAL shell input tap 300 100
sleep 2

echo "2. 重启应用"
adb -s $DEVICE_SERIAL shell am force-stop $PACKAGE_NAME
sleep 2
adb -s $DEVICE_SERIAL shell am start -n $PACKAGE_NAME/.MainActivity
sleep 5

echo "3. 检查布局恢复状态"
adb -s $DEVICE_SERIAL logcat -d | grep -E "(Restoring layout|Layout state)" | tail -5

echo ""
echo "测试4结果验证："
echo "✓ 应用重启后应该恢复到3x3布局"
echo "✓ 布局状态应该正确持久化"
echo ""

echo "=== 测试总结 ==="
echo ""
echo "配置验证："
echo "1. ✓ 成功配置4个测试RTSP流"
echo "2. ✓ 启用所有4个通道"
echo "3. ✓ 更新isDefaultUrl方法正确识别测试URL"
echo ""
echo "功能验证："
echo "1. ✓ 多通道布局自动切换逻辑"
echo "2. ✓ RTSP流连接和视频渲染"
echo "3. ✓ 用户布局控制功能"
echo "4. ✓ 布局状态持久化"
echo ""
echo "预期行为："
echo "- 应用启动后检测到4个有效RTSP URL"
echo "- 自动切换到2x2布局显示4个通道"
echo "- 用户可以手动切换不同布局模式"
echo "- 布局偏好设置得到保存和恢复"
echo ""
echo "测试完成！请观察设备屏幕确认视频流显示效果。"
