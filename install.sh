#!/bin/bash
# install_graphics_commander.sh

echo "安装 Graphics Commander..."
echo ""

# 检查是否以root运行
if [ "$(id -u)" -ne 0 ]; then
    echo "需要root权限进行安装"
    echo "请使用: sudo $0"
    exit 1
fi

# 编译
echo "编译程序..."
./build_graphics_commander.sh

if [ $? -ne 0 ]; then
    echo "编译失败，安装中止"
    exit 1
fi

# 安装到系统
echo ""
echo "安装到系统..."
install -m 755 graphics_commander /usr/local/bin/
install -m 644 graphics_commander.1 /usr/local/share/man/man1/

# 创建配置文件目录
mkdir -p /etc/graphics_commander

# 安装示例配置文件
if [ -f graphics_commander.conf ]; then
    install -m 644 graphics_commander.conf /etc/graphics_commander/
fi

echo ""
echo "安装完成!"
echo ""
echo "使用方法:"
echo "  1. 交互模式: graphics_commander"
echo "  2. 捕获屏幕: sudo graphics_commander --capture"
echo "  3. 列出设备: graphics_commander --list"
echo ""
echo "查看帮助: graphics_commander --help"
