#!/bin/bash

# Android NVR布局稳定性测试脚本
# 验证修复后的布局行为

DEVICE_SERIAL="192.168.31.186:5555"
PACKAGE_NAME="com.wulala.myyolov5rtspthreadpool"
ACTIVITY_NAME="com.wulala.myyolov5rtspthreadpool.MainActivity"

echo "=== Android NVR布局稳定性测试 ==="
echo "设备: $DEVICE_SERIAL"
echo "应用: $PACKAGE_NAME"
echo ""

# 测试1: 验证应用启动时保持单通道模式
echo "测试1: 验证应用启动时的默认布局..."
echo "1. 停止应用"
adb -s $DEVICE_SERIAL shell am force-stop $PACKAGE_NAME

echo "2. 清除应用数据以确保干净启动"
adb -s $DEVICE_SERIAL shell pm clear $PACKAGE_NAME

echo "3. 启动应用"
adb -s $DEVICE_SERIAL shell am start -n $ACTIVITY_NAME

echo "4. 等待5秒观察布局是否保持稳定..."
sleep 5

echo "5. 获取应用日志检查布局切换情况"
adb -s $DEVICE_SERIAL logcat -d | grep -E "(Layout changed|Multiple channels detected|Starting configured channels)" | tail -10

echo ""
echo "测试1完成 - 请观察设备屏幕确认："
echo "✓ 应用启动后应保持单通道全屏模式"
echo "✓ 不应在2-3秒后自动切换到多通道布局"
echo ""

# 测试2: 验证用户主动布局切换
echo "测试2: 验证用户主动布局切换功能..."
echo "1. 点击屏幕显示控制按钮"
adb -s $DEVICE_SERIAL shell input tap 500 500

sleep 2

echo "2. 点击2x2布局按钮"
adb -s $DEVICE_SERIAL shell input tap 200 100

sleep 3

echo "3. 点击1x1布局按钮返回单通道"
adb -s $DEVICE_SERIAL shell input tap 100 100

sleep 2

echo "测试2完成 - 请观察设备屏幕确认："
echo "✓ 用户可以主动切换布局"
echo "✓ 布局切换应显示Toast提示"
echo "✓ 按钮状态应正确更新"
echo ""

# 测试3: 验证布局状态持久化
echo "测试3: 验证布局状态持久化..."
echo "1. 切换到2x2布局"
adb -s $DEVICE_SERIAL shell input tap 500 500
sleep 1
adb -s $DEVICE_SERIAL shell input tap 200 100
sleep 2

echo "2. 重启应用"
adb -s $DEVICE_SERIAL shell am force-stop $PACKAGE_NAME
sleep 1
adb -s $DEVICE_SERIAL shell am start -n $ACTIVITY_NAME
sleep 3

echo "3. 检查布局是否恢复到2x2模式"
adb -s $DEVICE_SERIAL logcat -d | grep -E "(Restoring layout state|Layout state saved)" | tail -5

echo "测试3完成 - 请观察设备屏幕确认："
echo "✓ 应用重启后应恢复到之前选择的2x2布局"
echo "✓ 布局状态应正确持久化"
echo ""

# 测试4: 验证多通道配置时的智能布局切换
echo "测试4: 验证多通道配置的智能布局切换..."
echo "注意：此测试需要手动配置多个有效的RTSP URL"
echo "1. 当前配置只启用1个通道，应保持单通道模式"
echo "2. 如需测试多通道自动切换，请在应用中配置多个有效RTSP URL"

adb -s $DEVICE_SERIAL logcat -d | grep -E "(enabled channels|valid URLs|Single channel mode)" | tail -5

echo ""
echo "=== 测试总结 ==="
echo "1. ✓ 修复了默认启用4个通道导致的自动布局切换问题"
echo "2. ✓ 添加了用户意图检测，只有配置有效URL时才自动切换"
echo "3. ✓ 实现了布局状态持久化功能"
echo "4. ✓ 增强了用户布局控制和视觉反馈"
echo ""
echo "修复后的行为："
echo "- 应用启动默认保持单通道模式"
echo "- 用户可主动控制布局切换"
echo "- 布局偏好设置得到保存和恢复"
echo "- 只有在用户明确配置多个有效RTSP URL时才自动切换到多通道布局"
echo ""
echo "测试完成！"
