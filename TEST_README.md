# Multi-Channel NVR System Testing Guide

This document describes how to test the multi-channel RTSP streaming system with YOLOv5 object detection.

## Overview

The testing system provides comprehensive validation of:
- Multi-channel RTSP streaming (up to 16 channels)
- YOLOv5 object detection performance
- UI layout switching (1x1, 2x2, 3x3, 4x4)
- System performance and resource usage
- Thread safety and memory management

## Test Configuration

Test scenarios are defined in `app/src/main/assets/test_config.json`:

### Available Test Scenarios

1. **Single Channel Test** - Tests basic single channel streaming
2. **Quad Channel Test** - Tests 4-channel streaming in 2x2 layout
3. **Nine Channel Test** - Tests 9-channel streaming in 3x3 layout
4. **Full Channel Test** - Tests all 16 channels in 4x4 layout
5. **Layout Switching Test** - Tests dynamic layout switching
6. **Performance Stress Test** - Maximum load test with all features

### Performance Targets

- **Target FPS**: 30 FPS
- **Minimum FPS**: 25 FPS
- **Maximum Memory**: 512 MB
- **Maximum CPU**: 80%

## Running Tests

### Method 1: Automated Script (Recommended)

```bash
# Make script executable
chmod +x test_scripts/run_tests.sh

# Run all tests
./test_scripts/run_tests.sh

# Run specific test suite
./test_scripts/run_tests.sh basic
./test_scripts/run_tests.sh performance
./test_scripts/run_tests.sh layout
./test_scripts/run_tests.sh quick
```

### Method 2: Manual ADB Commands

```bash
# Enable test mode
adb shell am broadcast -a com.wulala.myyolov5rtspthreadpool.TEST_ACTION --es test_name "enable_test_mode"

# Run specific tests
adb shell am broadcast -a com.wulala.myyolov5rtspthreadpool.TEST_ACTION --es test_name "single"
adb shell am broadcast -a com.wulala.myyolov5rtspthreadpool.TEST_ACTION --es test_name "quad"
adb shell am broadcast -a com.wulala.myyolov5rtspthreadpool.TEST_ACTION --es test_name "stress"
```

### Method 3: Programmatic Testing

```java
// In your test code or activity
MainActivity activity = getCurrentActivity();
activity.enableTestMode();
activity.runTestScenario("Quad Channel Test");
```

## Test Commands

| Command | Description |
|---------|-------------|
| `single` | Single Channel Test |
| `quad` | Quad Channel Test |
| `nine` | Nine Channel Test |
| `full` | Full Channel Test |
| `layout` | Layout Switching Test |
| `stress` | Performance Stress Test |
| `quick` | Quick test (same as quad) |
| `enable_test_mode` | Enable test mode |

## Monitoring and Logs

### Real-time Monitoring

```bash
# Monitor application logs
adb logcat -s NVRTest TestUtils TestLauncher ChannelManager MultiChannelZLPlayer

# Monitor system performance
adb shell top | grep com.wulala.myyolov5rtspthreadpool
```

### Log Analysis

Test logs are automatically collected and saved with timestamps:
- `test_logs_YYYYMMDD_HHMMSS.txt` - Application logs
- `test_report_YYYYMMDD_HHMMSS.txt` - Summary report

## Validation Criteria

### Performance Validation

Tests automatically validate:
- **FPS Stability**: Minimum 25 FPS, variance < 5 FPS
- **Memory Usage**: Maximum 512 MB, leak threshold 50 MB
- **Detection Accuracy**: Minimum 80% detection rate
- **UI Responsiveness**: Touch response < 100ms, layout switch < 500ms

### Success Criteria

A test passes if:
1. All channels start successfully
2. FPS remains above minimum threshold
3. Memory usage stays within limits
4. No crashes or errors occur
5. UI remains responsive

## Troubleshooting

### Common Issues

1. **App not starting**
   - Ensure device is connected and USB debugging enabled
   - Check if app is installed: `adb shell pm list packages | grep myyolov5`

2. **Tests not responding**
   - Check if test mode is enabled
   - Verify broadcast receiver is registered
   - Check logcat for error messages

3. **Poor performance**
   - Verify RTSP URLs are accessible
   - Check network connectivity
   - Monitor system resources

### Debug Commands

```bash
# Check app status
adb shell dumpsys activity activities | grep myyolov5

# Check memory usage
adb shell dumpsys meminfo com.wulala.myyolov5rtspthreadpool

# Check CPU usage
adb shell top -n 1 | grep myyolov5

# Force stop app
adb shell am force-stop com.wulala.myyolov5rtspthreadpool
```

## Test Environment Setup

### Prerequisites

1. Android device with USB debugging enabled
2. ADB installed and device connected
3. Network access to RTSP streams
4. Sufficient device resources (RAM, CPU)

### RTSP Stream Configuration

Update test configuration with your RTSP URLs:

```json
{
  "test_channels": [
    {
      "channel_id": 0,
      "rtsp_url": "rtsp://your-camera-ip:554/stream1",
      "detection_enabled": true
    }
  ]
}
```

### Performance Optimization

For best test results:
- Close other applications
- Ensure stable network connection
- Use high-performance device settings
- Disable power saving modes

## Continuous Integration

### Automated Testing

The test system can be integrated into CI/CD pipelines:

```bash
# Example CI script
./test_scripts/run_tests.sh quick
if [ $? -eq 0 ]; then
    echo "Tests passed"
else
    echo "Tests failed"
    exit 1
fi
```

### Test Reports

Test reports include:
- System information
- Test configuration
- Performance metrics
- Pass/fail status
- Detailed logs

## Advanced Testing

### Custom Test Scenarios

Create custom test scenarios by modifying `test_config.json`:

```json
{
  "name": "Custom Test",
  "active_channels": [0, 1, 2, 3, 4, 5],
  "layout": "3x3",
  "duration_seconds": 300
}
```

### Performance Profiling

For detailed performance analysis:

```bash
# CPU profiling
adb shell simpleperf record -p $(adb shell pidof com.wulala.myyolov5rtspthreadpool)

# Memory profiling
adb shell dumpsys meminfo com.wulala.myyolov5rtspthreadpool --package
```

## Support

For issues or questions:
1. Check the troubleshooting section
2. Review application logs
3. Verify test configuration
4. Check device compatibility

## Test Results Interpretation

### Performance Metrics

- **FPS**: Frames per second for each channel and overall system
- **Memory**: RAM usage in MB
- **CPU**: Processor usage percentage
- **Detections**: Object detection count and accuracy

### Status Indicators

- ✓ **PASSED**: Test completed successfully
- ✗ **FAILED**: Test failed validation criteria
- ⚠ **WARNING**: Test completed with warnings
- ℹ **INFO**: Informational message
