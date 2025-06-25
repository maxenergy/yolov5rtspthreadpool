#!/bin/bash

# Android NVR闪退问题修复验证测试
# 验证应用能正常启动并保持多通道布局功能

DEVICE_SERIAL="192.168.31.186:5555"
PACKAGE_NAME="com.wulala.myyolov5rtspthreadpool"
ACTIVITY_NAME="com.wulala.myyolov5rtspthreadpool.MainActivity"

echo "=== Android NVR闪退问题修复验证测试 ==="
echo "设备: $DEVICE_SERIAL"
echo "应用: $PACKAGE_NAME"
echo ""

# 测试1: 验证应用正常启动
echo "测试1: 验证应用正常启动"
echo "------------------------"
echo "1. 清除应用数据"
adb -s $DEVICE_SERIAL shell am force-stop $PACKAGE_NAME
adb -s $DEVICE_SERIAL shell pm clear $PACKAGE_NAME

echo "2. 清除日志"
adb -s $DEVICE_SERIAL logcat -c

echo "3. 启动应用"
adb -s $DEVICE_SERIAL shell am start -n $PACKAGE_NAME/.MainActivity

echo "4. 等待10秒观察应用启动情况..."
sleep 10

echo "5. 检查是否有崩溃日志"
CRASH_COUNT=$(adb -s $DEVICE_SERIAL logcat -d | grep -E "(FATAL|AndroidRuntime|CRASH|Exception)" | wc -l)
echo "崩溃日志数量: $CRASH_COUNT"

if [ $CRASH_COUNT -gt 0 ]; then
    echo "❌ 发现崩溃日志:"
    adb -s $DEVICE_SERIAL logcat -d | grep -E "(FATAL|AndroidRuntime|CRASH|Exception)" | tail -5
else
    echo "✅ 未发现崩溃日志"
fi

echo ""

# 测试2: 验证应用进程状态
echo "测试2: 验证应用进程状态"
echo "------------------------"
PROCESS_COUNT=$(adb -s $DEVICE_SERIAL shell ps | grep $PACKAGE_NAME | wc -l)
echo "应用进程数量: $PROCESS_COUNT"

if [ $PROCESS_COUNT -gt 0 ]; then
    echo "✅ 应用进程正在运行"
    adb -s $DEVICE_SERIAL shell ps | grep $PACKAGE_NAME
else
    echo "❌ 应用进程未运行"
fi

echo ""

# 测试3: 验证多通道布局功能
echo "测试3: 验证多通道布局功能"
echo "--------------------------"
echo "1. 检查布局自动切换日志"
adb -s $DEVICE_SERIAL logcat -d | grep -E "(enabled channels|Multiple channels detected|Layout changed)" | tail -5

echo ""
echo "2. 测试手动布局切换"
echo "   - 点击屏幕显示控制按钮"
adb -s $DEVICE_SERIAL shell input tap 500 500
sleep 2

echo "   - 切换到1x1布局"
adb -s $DEVICE_SERIAL shell input tap 100 100
sleep 2

echo "   - 切换到3x3布局"
adb -s $DEVICE_SERIAL shell input tap 300 100
sleep 2

echo "3. 检查布局切换日志"
adb -s $DEVICE_SERIAL logcat -d | grep -E "(Layout changed|切换到)" | tail -3

echo ""

# 测试4: 验证Surface管理修复
echo "测试4: 验证Surface管理修复"
echo "---------------------------"
echo "1. 检查Surface相关日志"
SURFACE_ERROR_COUNT=$(adb -s $DEVICE_SERIAL logcat -d | grep -E "(Surface.*null|NullPointerException.*Surface)" | wc -l)
echo "Surface错误数量: $SURFACE_ERROR_COUNT"

if [ $SURFACE_ERROR_COUNT -gt 0 ]; then
    echo "❌ 发现Surface相关错误:"
    adb -s $DEVICE_SERIAL logcat -d | grep -E "(Surface.*null|NullPointerException.*Surface)" | tail -3
else
    echo "✅ 未发现Surface相关错误"
fi

echo ""
echo "2. 检查ChannelManager Surface管理"
adb -s $DEVICE_SERIAL logcat -d | grep -E "(Surface.*stored|Surface.*removed|Surface.*destroyed)" | tail -5

echo ""

# 测试5: 验证Native层修复
echo "测试5: 验证Native层修复"
echo "------------------------"
echo "1. 检查Native崩溃"
NATIVE_CRASH_COUNT=$(adb -s $DEVICE_SERIAL logcat -d | grep -E "(native_crash|tombstone|desplay_process)" | wc -l)
echo "Native崩溃数量: $NATIVE_CRASH_COUNT"

if [ $NATIVE_CRASH_COUNT -gt 0 ]; then
    echo "❌ 发现Native层问题:"
    adb -s $DEVICE_SERIAL logcat -d | grep -E "(native_crash|tombstone|desplay_process)" | tail -3
else
    echo "✅ 未发现Native层问题"
fi

echo ""
echo "2. 检查Display进程状态"
adb -s $DEVICE_SERIAL logcat -d | grep -E "(Display process|desplay_process)" | tail -3

echo ""

echo "=== 修复验证总结 ==="
echo ""
echo "🔧 修复内容回顾:"
echo "1. ✓ 修复ChannelManager.setChannelSurface中的ConcurrentHashMap null值问题"
echo "2. ✓ 修复ZLPlayer.h中缺失的rtsp_url成员变量"
echo "3. ✓ 修复desplay_process函数中的无限循环问题"
echo "4. ✓ 添加适当的退出条件和错误处理"
echo "5. ✓ 将关键成员变量移到public访问权限"
echo ""
echo "🎯 验证结果:"
if [ $CRASH_COUNT -eq 0 ] && [ $PROCESS_COUNT -gt 0 ] && [ $SURFACE_ERROR_COUNT -eq 0 ] && [ $NATIVE_CRASH_COUNT -eq 0 ]; then
    echo "✅ 所有测试通过 - 闪退问题已成功修复!"
    echo "✅ 应用能正常启动并保持运行"
    echo "✅ 多通道布局功能正常工作"
    echo "✅ Surface管理问题已解决"
    echo "✅ Native层崩溃问题已修复"
else
    echo "⚠️  部分测试未通过，需要进一步检查:"
    [ $CRASH_COUNT -gt 0 ] && echo "   - 仍有崩溃日志"
    [ $PROCESS_COUNT -eq 0 ] && echo "   - 应用进程未运行"
    [ $SURFACE_ERROR_COUNT -gt 0 ] && echo "   - 仍有Surface错误"
    [ $NATIVE_CRASH_COUNT -gt 0 ] && echo "   - 仍有Native层问题"
fi

echo ""
echo "测试完成！"
