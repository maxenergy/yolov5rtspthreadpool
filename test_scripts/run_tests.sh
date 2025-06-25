#!/bin/bash

# Multi-Channel NVR System Test Script
# This script automates the testing of the multi-channel RTSP streaming system

set -e

# Configuration
PACKAGE_NAME="com.wulala.myyolov5rtspthreadpool"
ACTIVITY_NAME="$PACKAGE_NAME.MainActivity"
TEST_ACTION="$PACKAGE_NAME.TEST_ACTION"
LOG_TAG="NVRTest"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Function to print colored output
print_status() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Function to check if device is connected
check_device() {
    print_status "Checking device connection..."
    if ! adb devices | grep -q "device$"; then
        print_error "No Android device connected. Please connect a device and enable USB debugging."
        exit 1
    fi
    print_success "Device connected"
}

# Function to check if app is installed
check_app() {
    print_status "Checking if app is installed..."
    if ! adb shell pm list packages | grep -q "$PACKAGE_NAME"; then
        print_error "App not installed. Please install the app first."
        exit 1
    fi
    print_success "App is installed"
}

# Function to start the app
start_app() {
    print_status "Starting the app..."
    adb shell am start -n "$ACTIVITY_NAME"
    sleep 3
    print_success "App started"
}

# Function to enable test mode
enable_test_mode() {
    print_status "Enabling test mode..."
    adb shell am broadcast -a "$TEST_ACTION" --es test_name "enable_test_mode"
    sleep 2
    print_success "Test mode enabled"
}

# Function to run a specific test
run_test() {
    local test_name="$1"
    local duration="$2"
    
    print_status "Running test: $test_name"
    adb shell am broadcast -a "$TEST_ACTION" --es test_name "$test_name"
    
    if [ -n "$duration" ]; then
        print_status "Waiting $duration seconds for test completion..."
        sleep "$duration"
    fi
}

# Function to collect logs
collect_logs() {
    local test_name="$1"
    local log_file="test_logs_$(date +%Y%m%d_%H%M%S).txt"
    
    print_status "Collecting logs for $test_name..."
    adb logcat -d -s "$LOG_TAG:*" "TestUtils:*" "TestLauncher:*" "ChannelManager:*" "MultiChannelZLPlayer:*" > "$log_file"
    print_success "Logs saved to $log_file"
}

# Function to clear logs
clear_logs() {
    print_status "Clearing logcat buffer..."
    adb logcat -c
}

# Function to monitor performance
monitor_performance() {
    local duration="$1"
    print_status "Monitoring performance for $duration seconds..."
    
    # Monitor CPU and memory usage
    for i in $(seq 1 "$duration"); do
        cpu_usage=$(adb shell top -n 1 | grep "$PACKAGE_NAME" | awk '{print $9}' | head -1)
        memory_usage=$(adb shell dumpsys meminfo "$PACKAGE_NAME" | grep "TOTAL" | awk '{print $2}')
        
        if [ -n "$cpu_usage" ] && [ -n "$memory_usage" ]; then
            echo "Time: ${i}s, CPU: ${cpu_usage}%, Memory: ${memory_usage}KB"
        fi
        
        sleep 1
    done
}

# Function to run basic functionality tests
run_basic_tests() {
    print_status "Running basic functionality tests..."
    
    clear_logs
    run_test "single" 60
    collect_logs "single_channel"
    
    clear_logs
    run_test "quad" 120
    collect_logs "quad_channel"
    
    print_success "Basic tests completed"
}

# Function to run performance tests
run_performance_tests() {
    print_status "Running performance tests..."
    
    clear_logs
    run_test "nine" 180
    collect_logs "nine_channel"
    
    clear_logs
    run_test "stress" 300
    collect_logs "stress_test"
    
    print_success "Performance tests completed"
}

# Function to run layout switching tests
run_layout_tests() {
    print_status "Running layout switching tests..."
    
    clear_logs
    run_test "layout" 150
    collect_logs "layout_switching"
    
    print_success "Layout tests completed"
}

# Function to generate test report
generate_report() {
    local report_file="test_report_$(date +%Y%m%d_%H%M%S).txt"
    
    print_status "Generating test report..."
    
    {
        echo "Multi-Channel NVR System Test Report"
        echo "Generated on: $(date)"
        echo "=================================="
        echo ""
        echo "Device Information:"
        adb shell getprop ro.product.model
        adb shell getprop ro.build.version.release
        echo ""
        echo "App Information:"
        adb shell dumpsys package "$PACKAGE_NAME" | grep versionName
        echo ""
        echo "Test Results:"
        echo "- Basic functionality tests: See individual log files"
        echo "- Performance tests: See individual log files"
        echo "- Layout switching tests: See individual log files"
        echo ""
        echo "Log files generated:"
        ls -la test_logs_*.txt 2>/dev/null || echo "No log files found"
    } > "$report_file"
    
    print_success "Test report saved to $report_file"
}

# Main test execution
main() {
    print_status "Starting Multi-Channel NVR System Tests"
    print_status "========================================"
    
    # Pre-test checks
    check_device
    check_app
    
    # Start app and enable test mode
    start_app
    enable_test_mode
    
    # Run test suites based on arguments
    case "${1:-all}" in
        "basic")
            run_basic_tests
            ;;
        "performance")
            run_performance_tests
            ;;
        "layout")
            run_layout_tests
            ;;
        "quick")
            print_status "Running quick test..."
            clear_logs
            run_test "quick" 60
            collect_logs "quick_test"
            ;;
        "all")
            run_basic_tests
            run_layout_tests
            run_performance_tests
            ;;
        *)
            print_error "Unknown test suite: $1"
            echo "Usage: $0 [basic|performance|layout|quick|all]"
            exit 1
            ;;
    esac
    
    # Generate final report
    generate_report
    
    print_success "All tests completed successfully!"
    print_status "Check the generated log files and report for detailed results."
}

# Help function
show_help() {
    echo "Multi-Channel NVR System Test Script"
    echo "Usage: $0 [OPTION]"
    echo ""
    echo "Options:"
    echo "  basic       Run basic functionality tests (single, quad channel)"
    echo "  performance Run performance tests (nine channel, stress test)"
    echo "  layout      Run layout switching tests"
    echo "  quick       Run a quick test (quad channel)"
    echo "  all         Run all test suites (default)"
    echo "  help        Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0 basic      # Run only basic tests"
    echo "  $0 quick      # Run quick test"
    echo "  $0            # Run all tests"
}

# Check for help argument
if [ "$1" = "help" ] || [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
    show_help
    exit 0
fi

# Run main function
main "$@"
