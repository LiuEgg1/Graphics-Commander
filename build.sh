#!/bin/bash
# build_graphics_commander.sh

echo "编译 Graphics Commander..."
echo ""

# 检查系统
if [ "$(id -u)" -ne 0 ]; then
    echo "注意: 部分功能需要root权限"
    echo ""
fi

# 检查依赖
echo "检查依赖..."
if pkg-config --exists x11; then
    echo "✓ 找到 X11 开发库"
    X11_FLAGS="-DUSE_X11 $(pkg-config --cflags --libs x11)"
else
    echo "✗ 未找到 X11 开发库，X11支持将被禁用"
    X11_FLAGS=""
fi

if pkg-config --exists wayland-client; then
    echo "✓ 找到 Wayland 开发库"
    WAYLAND_FLAGS="-DUSE_WAYLAND $(pkg-config --cflags --libs wayland-client)"
else
    echo "✗ 未找到 Wayland 开发库，Wayland支持将被禁用"
    WAYLAND_FLAGS=""
fi

# 编译选项
CFLAGS="-O2 -Wall -Wextra -std=gnu11 -D_GNU_SOURCE"
LDFLAGS="-lpthread -lm"

# 编译
echo ""
echo "编译主程序..."
gcc $CFLAGS $X11_FLAGS $WAYLAND_FLAGS \
    -o graphics_commander \
    graphics_commander.c \
    $LDFLAGS

if [ $? -eq 0 ]; then
    echo ""
    echo "编译成功!"
    echo ""
    echo "执行文件: graphics_commander"
    echo ""
    echo "使用示例:"
    echo "  sudo ./graphics_commander --capture"
    echo "  ./graphics_commander --interactive"
    echo "  ./graphics_commander --list"
    echo "  ./graphics_commander --benchmark"
    echo ""
    echo "权限说明:"
    echo "  读取帧缓冲区需要root权限"
    echo "  X11连接需要DISPLAY环境变量"
else
    echo "编译失败!"
    exit 1
fi
