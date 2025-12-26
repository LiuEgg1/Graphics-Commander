// graphics_commander.c - 综合图形服务器工具
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <getopt.h>
#include <termios.h>
#include <sys/select.h>
#include <pthread.h>
#include <math.h>

// X11支持
#ifdef USE_X11
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#endif

// Wayland支持
#ifdef USE_WAYLAND
#include <wayland-client.h>
#endif

#define VERSION "2.0.0"
#define MAX_BUFFERS 10
#define MAX_DISPLAYS 10
#define UNICODE_CHARS 256
#define COLOR_TABLE_SIZE 256

// Unicode字符密度级别
static const char* unicode_blocks[] = {
    // 完整方块
    "█", "▓", "▒", "░",
    // 半字符
    "▀", "▄", "▌", "▐",
    // 简单字符
    "@", "#", "8", "&", "o", ":", "*", ".", " ",
    // Braille字符 (简化)
    "⠀", "⠁", "⠂", "⠃", "⠄", "⠅", "⠆", "⠇",
    "⣀", "⣁", "⣂", "⣃", "⣄", "⣅", "⣆", "⣇",
    "⣿"
};

// ANSI颜色代码
typedef struct {
    int code;
    char fg[16];
    char bg[16];
} AnsiColor;

// 像素格式
typedef enum {
    PIXFMT_RGB565 = 0,
    PIXFMT_RGB888,
    PIXFMT_BGR888,
    PIXFMT_RGBA8888,
    PIXFMT_BGRA8888,
    PIXFMT_UNKNOWN
} PixelFormat;

// 颜色模式
typedef enum {
    COLOR_NONE = 0,
    COLOR_BASIC = 1,
    COLOR_256 = 2,
    COLOR_TRUE = 3,
    COLOR_GRAY = 4
} ColorMode;

// 字符集模式
typedef enum {
    CHARSET_SIMPLE = 0,
    CHARSET_BLOCKS = 1,
    CHARSET_HALF = 2,
    CHARSET_BRAILLE = 3,
    CHARSET_ART = 4
} CharsetMode;

// 服务器类型
typedef enum {
    SERVER_FRAMEBUFFER = 0,
    SERVER_X11 = 1,
    SERVER_WAYLAND = 2,
    SERVER_VNC = 3,
    SERVER_RDP = 4
} ServerType;

// 图形缓冲区
typedef struct {
    char device[64];
    int fd;
    void *buffer;
    size_t size;
    int width;
    int height;
    int bpp;
    int line_length;
    PixelFormat format;
    ServerType type;
} GraphicsBuffer;

// 显示配置
typedef struct {
    int output_width;
    int output_height;
    ColorMode color_mode;
    CharsetMode charset;
    float brightness;
    float contrast;
    int dither;
    int fps;
    int continuous;
    int region_x;
    int region_y;
    int region_w;
    int region_h;
} DisplayConfig;

// 服务器连接配置
typedef struct {
    ServerType type;
    char display[32];
    char host[256];
    int port;
    char username[64];
    char password[64];
    int use_ssh;
} ServerConfig;

// 应用程序状态
typedef struct {
    GraphicsBuffer buffers[MAX_BUFFERS];
    int buffer_count;
    DisplayConfig display;
    ServerConfig server;
    int running;
    int verbose;
    int benchmark;
    pthread_t capture_thread;
} AppState;

// 全局变量
static AppState app = {0};
static struct termios original_termios;
static AnsiColor color_table[COLOR_TABLE_SIZE];

// 函数声明
void print_banner();
void print_help();
void setup_terminal();
void restore_terminal();
void clear_screen();
void move_cursor(int x, int y);
void hide_cursor();
void show_cursor();
int get_terminal_size(int *width, int *height);
void init_color_table();
char* get_color_fg(int r, int g, int b, ColorMode mode);
char* get_color_bg(int r, int g, int b, ColorMode mode);
const char* get_unicode_char(int brightness, CharsetMode charset);
int detect_servers();
GraphicsBuffer* open_framebuffer(const char* device);
void close_framebuffer(GraphicsBuffer* buf);
int capture_screen();
void* capture_thread_func(void* arg);
int rgb_to_brightness(int r, int g, int b);
int convert_buffer_to_text(GraphicsBuffer* buf, DisplayConfig* config, char** output);
void display_text(char* text, int width, int height);
void benchmark_mode();
void interactive_mode();
int connect_to_server(ServerConfig* config);
void list_available_devices();
void signal_handler(int sig);

// ANSI颜色函数
void init_color_table() {
    // 基本8色
    strcpy(color_table[0].fg, "\033[30m"); strcpy(color_table[0].bg, "\033[40m");
    strcpy(color_table[1].fg, "\033[31m"); strcpy(color_table[1].bg, "\033[41m");
    strcpy(color_table[2].fg, "\033[32m"); strcpy(color_table[2].bg, "\033[42m");
    strcpy(color_table[3].fg, "\033[33m"); strcpy(color_table[3].bg, "\033[43m");
    strcpy(color_table[4].fg, "\033[34m"); strcpy(color_table[4].bg, "\033[44m");
    strcpy(color_table[5].fg, "\033[35m"); strcpy(color_table[5].bg, "\033[45m");
    strcpy(color_table[6].fg, "\033[36m"); strcpy(color_table[6].bg, "\033[46m");
    strcpy(color_table[7].fg, "\033[37m"); strcpy(color_table[7].bg, "\033[47m");
    
    // 亮色
    for (int i = 8; i < 16; i++) {
        sprintf(color_table[i].fg, "\033[9%dm", i-8);
        sprintf(color_table[i].bg, "\033[10%dm", i-8);
    }
    
    // 256色
    for (int i = 16; i < COLOR_TABLE_SIZE; i++) {
        sprintf(color_table[i].fg, "\033[38;5;%dm", i);
        sprintf(color_table[i].bg, "\033[48;5;%dm", i);
    }
}

char* get_color_fg(int r, int g, int b, ColorMode mode) {
    static char buffer[32];
    
    switch (mode) {
        case COLOR_NONE:
            return "";
            
        case COLOR_BASIC: {
            // 转换为8基本色
            int brightness = (r + g + b) / 3;
            int index = brightness / 32;
            if (index > 7) index = 7;
            return (char*)color_table[index].fg;
        }
            
        case COLOR_256: {
            // 转换为6x6x6立方色
            int ri = r / 51;
            int gi = g / 51;
            int bi = b / 51;
            int index = 16 + 36 * ri + 6 * gi + bi;
            return (char*)color_table[index].fg;
        }
            
        case COLOR_GRAY: {
            // 24级灰度
            int gray = (r + g + b) / 3;
            int index = 232 + (gray * 24 / 256);
            return (char*)color_table[index].fg;
        }
            
        case COLOR_TRUE:
        default:
            // 真彩色
            sprintf(buffer, "\033[38;2;%d;%d;%dm", r, g, b);
            return buffer;
    }
}

char* get_color_bg(int r, int g, int b, ColorMode mode) {
    static char buffer[32];
    
    switch (mode) {
        case COLOR_NONE:
            return "";
            
        case COLOR_BASIC: {
            int brightness = (r + g + b) / 3;
            int index = brightness / 32;
            if (index > 7) index = 7;
            return (char*)color_table[index].bg;
        }
            
        case COLOR_256: {
            int ri = r / 51;
            int gi = g / 51;
            int bi = b / 51;
            int index = 16 + 36 * ri + 6 * gi + bi;
            return (char*)color_table[index].bg;
        }
            
        case COLOR_GRAY: {
            int gray = (r + g + b) / 3;
            int index = 232 + (gray * 24 / 256);
            return (char*)color_table[index].bg;
        }
            
        case COLOR_TRUE:
        default:
            sprintf(buffer, "\033[48;2;%d;%d;%dm", r, g, b);
            return buffer;
    }
}

const char* get_unicode_char(int brightness, CharsetMode charset) {
    int index;
    
    switch (charset) {
        case CHARSET_BLOCKS:
            index = (brightness * 4) / 256;
            if (index > 3) index = 3;
            return unicode_blocks[index];
            
        case CHARSET_HALF:
            index = 4 + (brightness * 4) / 256;
            if (index > 7) index = 7;
            return unicode_blocks[index];
            
        case CHARSET_BRAILLE:
            index = 8 + (brightness * 8) / 256;
            if (index > 15) index = 15;
            return unicode_blocks[index];
            
        case CHARSET_ART:
            index = 16 + (brightness * 9) / 256;
            if (index > 24) index = 24;
            return unicode_blocks[index];
            
        case CHARSET_SIMPLE:
        default:
            index = 25 + (brightness * 9) / 256;
            if (index > 33) index = 33;
            return unicode_blocks[index];
    }
}

void print_banner() {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║           Graphics Commander v%s                   ║\n", VERSION);
    printf("║     综合图形服务器连接、缓冲读取和终端显示工具        ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n");
    printf("\n");
}

void print_help() {
    print_banner();
    printf("用法: graphics_commander [选项]\n\n");
    printf("主要模式:\n");
    printf("  --capture, -c          捕获并显示屏幕\n");
    printf("  --connect, -C          连接到远程图形服务器\n");
    printf("  --interactive, -i      交互式模式\n");
    printf("  --benchmark, -b        性能测试模式\n");
    printf("  --list, -l             列出可用设备\n");
    printf("\n捕获选项:\n");
    printf("  --device DEVICE        帧缓冲区设备 (默认: /dev/fb0)\n");
    printf("  --width WIDTH          输出宽度 (字符数)\n");
    printf("  --height HEIGHT        输出高度 (字符数)\n");
    printf("  --fps FPS              帧率 (默认: 10)\n");
    printf("  --continuous, -R       连续捕获模式\n");
    printf("\n显示选项:\n");
    printf("  --color MODE           颜色模式: none,basic,256,true,gray\n");
    printf("  --charset SET          字符集: simple,blocks,half,braille,art\n");
    printf("  --brightness VAL       亮度调整 (0.5-2.0)\n");
    printf("  --contrast VAL         对比度调整 (0.5-2.0)\n");
    printf("\n连接选项:\n");
    printf("  --server TYPE          服务器类型: fb,x11,wayland,vnc,rdp\n");
    printf("  --display DISP         X11显示 (例如: :0)\n");
    printf("  --host HOST            远程主机\n");
    printf("  --port PORT            端口号\n");
    printf("  --username USER        用户名\n");
    printf("  --password PASS        密码\n");
    printf("\n其他选项:\n");
    printf("  --help, -h             显示此帮助\n");
    printf("  --verbose, -v          详细输出\n");
    printf("  --version              显示版本\n");
    printf("\n示例:\n");
    printf("  graphics_commander -c --color true --charset braille\n");
    printf("  graphics_commander -C --server vnc --host 192.168.1.100\n");
    printf("  graphics_commander -i\n");
    printf("  graphics_commander -l\n");
}

void setup_terminal() {
    struct termios new_termios;
    
    // 保存原始终端设置
    tcgetattr(STDIN_FILENO, &original_termios);
    
    // 设置新终端设置
    new_termios = original_termios;
    new_termios.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);
    
    // 隐藏光标
    hide_cursor();
    
    // 清屏
    clear_screen();
}

void restore_terminal() {
    // 恢复终端设置
    tcsetattr(STDIN_FILENO, TCSANOW, &original_termios);
    
    // 显示光标
    show_cursor();
    
    // 重置颜色
    printf("\033[0m");
}

void clear_screen() {
    printf("\033[2J\033[H");
    fflush(stdout);
}

void move_cursor(int x, int y) {
    printf("\033[%d;%dH", y, x);
    fflush(stdout);
}

void hide_cursor() {
    printf("\033[?25l");
    fflush(stdout);
}

void show_cursor() {
    printf("\033[?25h");
    fflush(stdout);
}

int get_terminal_size(int *width, int *height) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) {
        return -1;
    }
    
    *width = ws.ws_col;
    *height = ws.ws_row;
    return 0;
}

int detect_servers() {
    printf("检测图形服务器...\n\n");
    
    int found = 0;
    
    // 检测帧缓冲区
    for (int i = 0; i < 4; i++) {
        char device[32];
        snprintf(device, sizeof(device), "/dev/fb%d", i);
        
        if (access(device, F_OK) == 0) {
            printf("✓ 帧缓冲区: %s\n", device);
            
            // 尝试打开获取信息
            int fd = open(device, O_RDONLY);
            if (fd >= 0) {
                struct fb_fix_screeninfo fix_info;
                struct fb_var_screeninfo var_info;
                
                if (ioctl(fd, FBIOGET_FSCREENINFO, &fix_info) == 0 &&
                    ioctl(fd, FBIOGET_VSCREENINFO, &var_info) == 0) {
                    printf("   分辨率: %dx%d\n", var_info.xres, var_info.yres);
                    printf("   位深度: %d\n", var_info.bits_per_pixel);
                    printf("   缓冲区大小: %zu 字节\n", fix_info.smem_len);
                }
                close(fd);
            }
            found++;
        }
    }
    
    // 检测X11
    if (getenv("DISPLAY")) {
        printf("✓ X11服务器: DISPLAY=%s\n", getenv("DISPLAY"));
        found++;
    }
    
    // 检测Wayland
    if (getenv("WAYLAND_DISPLAY")) {
        printf("✓ Wayland服务器: WAYLAND_DISPLAY=%s\n", getenv("WAYLAND_DISPLAY"));
        found++;
    }
    
    // 检测VNC服务器
    system("ps aux | grep -E '[x]11vnc|[v]ncserver' > /dev/null 2>&1");
    if (system("ps aux | grep -E '[x]11vnc|[v]ncserver' > /dev/null 2>&1") == 0) {
        printf("✓ VNC服务器正在运行\n");
        found++;
    }
    
    if (found == 0) {
        printf("未检测到图形服务器。\n");
    }
    
    return found;
}

GraphicsBuffer* open_framebuffer(const char* device) {
    GraphicsBuffer* buf = malloc(sizeof(GraphicsBuffer));
    if (!buf) {
        perror("分配内存失败");
        return NULL;
    }
    
    strcpy(buf->device, device);
    buf->type = SERVER_FRAMEBUFFER;
    
    // 打开设备
    buf->fd = open(device, O_RDONLY);
    if (buf->fd < 0) {
        perror("打开帧缓冲区失败");
        free(buf);
        return NULL;
    }
    
    // 获取屏幕信息
    struct fb_fix_screeninfo fix_info;
    struct fb_var_screeninfo var_info;
    
    if (ioctl(buf->fd, FBIOGET_FSCREENINFO, &fix_info) < 0) {
        perror("获取固定屏幕信息失败");
        close(buf->fd);
        free(buf);
        return NULL;
    }
    
    if (ioctl(buf->fd, FBIOGET_VSCREENINFO, &var_info) < 0) {
        perror("获取可变屏幕信息失败");
        close(buf->fd);
        free(buf);
        return NULL;
    }
    
    // 填充缓冲区信息
    buf->width = var_info.xres;
    buf->height = var_info.yres;
    buf->bpp = var_info.bits_per_pixel;
    buf->line_length = fix_info.line_length;
    buf->size = fix_info.smem_len;
    
    // 检测像素格式
    if (buf->bpp == 32) {
        if (var_info.red.offset == 16 && var_info.green.offset == 8 && var_info.blue.offset == 0)
            buf->format = PIXFMT_RGBA8888;
        else if (var_info.red.offset == 0 && var_info.green.offset == 8 && var_info.blue.offset == 16)
            buf->format = PIXFMT_BGRA8888;
        else
            buf->format = PIXFMT_UNKNOWN;
    } else if (buf->bpp == 24) {
        // 假设BGR格式
        buf->format = PIXFMT_BGR888;
    } else if (buf->bpp == 16) {
        buf->format = PIXFMT_RGB565;
    } else {
        buf->format = PIXFMT_UNKNOWN;
    }
    
    // 映射内存
    buf->buffer = mmap(NULL, buf->size, PROT_READ, MAP_SHARED, buf->fd, 0);
    if (buf->buffer == MAP_FAILED) {
        perror("映射帧缓冲区失败");
        close(buf->fd);
        free(buf);
        return NULL;
    }
    
    return buf;
}

void close_framebuffer(GraphicsBuffer* buf) {
    if (buf) {
        if (buf->buffer && buf->buffer != MAP_FAILED) {
            munmap(buf->buffer, buf->size);
        }
        if (buf->fd >= 0) {
            close(buf->fd);
        }
        free(buf);
    }
}

int rgb_to_brightness(int r, int g, int b) {
    // 使用标准亮度公式
    return (int)(0.299 * r + 0.587 * g + 0.114 * b);
}

int get_pixel_color(GraphicsBuffer* buf, int x, int y, int* r, int* g, int* b) {
    if (x < 0 || x >= buf->width || y < 0 || y >= buf->height) {
        return -1;
    }
    
    int offset = y * buf->line_length + x * (buf->bpp / 8);
    unsigned char* pixel = (unsigned char*)buf->buffer + offset;
    
    switch (buf->format) {
        case PIXFMT_RGB565: {
            unsigned short rgb = *(unsigned short*)pixel;
            *r = ((rgb >> 11) & 0x1F) * 8;
            *g = ((rgb >> 5) & 0x3F) * 4;
            *b = (rgb & 0x1F) * 8;
            break;
        }
        case PIXFMT_RGB888:
            *r = pixel[0];
            *g = pixel[1];
            *b = pixel[2];
            break;
        case PIXFMT_BGR888:
            *r = pixel[2];
            *g = pixel[1];
            *b = pixel[0];
            break;
        case PIXFMT_RGBA8888:
            *r = pixel[0];
            *g = pixel[1];
            *b = pixel[2];
            break;
        case PIXFMT_BGRA8888:
            *r = pixel[2];
            *g = pixel[1];
            *b = pixel[0];
            break;
        default:
            // 假设灰度
            *r = *g = *b = pixel[0];
            break;
    }
    
    return 0;
}

int convert_buffer_to_text(GraphicsBuffer* buf, DisplayConfig* config, char** output) {
    if (!buf || !buf->buffer || !config) {
        return -1;
    }
    
    // 计算实际区域
    int region_x = config->region_x;
    int region_y = config->region_y;
    int region_w = config->region_w > 0 ? config->region_w : buf->width;
    int region_h = config->region_h > 0 ? config->region_h : buf->height;
    
    // 边界检查
    if (region_x + region_w > buf->width) region_w = buf->width - region_x;
    if (region_y + region_h > buf->height) region_h = buf->height - region_y;
    if (region_w <= 0 || region_h <= 0) {
        return -1;
    }
    
    // 计算采样步长
    float x_step = (float)region_w / config->output_width;
    float y_step = (float)region_h / config->output_height;
    
    // 分配输出缓冲区
    int line_len = config->output_width * 64; // 预留颜色代码空间
    *output = malloc(config->output_height * line_len);
    if (!*output) {
        return -1;
    }
    
    char* current = *output;
    
    for (int out_y = 0; out_y < config->output_height; out_y++) {
        int in_y = region_y + (int)(out_y * y_step);
        
        char last_fg[32] = "";
        char last_bg[32] = "";
        
        for (int out_x = 0; out_x < config->output_width; out_x++) {
            int in_x = region_x + (int)(out_x * x_step);
            
            // 获取像素颜色
            int r, g, b;
            if (get_pixel_color(buf, in_x, in_y, &r, &g, &b) != 0) {
                r = g = b = 0;
            }
            
            // 应用亮度和对比度调整
            r = (int)((r - 128) * config->contrast + 128 * config->brightness);
            g = (int)((g - 128) * config->contrast + 128 * config->brightness);
            b = (int)((b - 128) * config->contrast + 128 * config->brightness);
            
            // 限制范围
            if (r < 0) r = 0; if (r > 255) r = 255;
            if (g < 0) g = 0; if (g > 255) g = 255;
            if (b < 0) b = 0; if (b > 255) b = 255;
            
            // 获取颜色代码
            char* fg_color = get_color_fg(r, g, b, config->color_mode);
            char* bg_color = config->color_mode == COLOR_NONE ? "" : 
                           get_color_bg(r/2, g/2, b/2, config->color_mode);
            
            // 只有在颜色变化时才输出颜色代码
            if (strcmp(fg_color, last_fg) != 0 || strcmp(bg_color, last_bg) != 0) {
                strcpy(last_fg, fg_color);
                strcpy(last_bg, bg_color);
                current += sprintf(current, "%s%s", fg_color, bg_color);
            }
            
            // 获取字符
            int brightness = rgb_to_brightness(r, g, b);
            const char* ch = get_unicode_char(brightness, config->charset);
            current += sprintf(current, "%s", ch);
        }
        
        // 每行结束重置颜色
        if (config->color_mode != COLOR_NONE) {
            current += sprintf(current, "\033[0m");
        }
        current += sprintf(current, "\n");
    }
    
    return 0;
}

void display_text(char* text, int width, int height) {
    if (!text) return;
    
    // 清屏并移动光标到左上角
    clear_screen();
    
    // 输出文本
    printf("%s", text);
    fflush(stdout);
}

void* capture_thread_func(void* arg) {
    DisplayConfig* config = (DisplayConfig*)arg;
    GraphicsBuffer* buf = NULL;
    char* output = NULL;
    struct timespec start, end;
    long frame_count = 0;
    
    // 打开默认帧缓冲区
    buf = open_framebuffer("/dev/fb0");
    if (!buf) {
        fprintf(stderr, "无法打开帧缓冲区\n");
        return NULL;
    }
    
    if (app.verbose) {
        printf("开始捕获，分辨率: %dx%d\n", buf->width, buf->height);
    }
    
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    while (app.running) {
        // 转换缓冲区为文本
        if (convert_buffer_to_text(buf, config, &output) == 0) {
            // 显示文本
            display_text(output, config->output_width, config->output_height);
            free(output);
        }
        
        frame_count++;
        
        // 控制帧率
        if (config->fps > 0) {
            usleep(1000000 / config->fps);
        }
        
        // 检查按键
        struct timeval tv = {0, 0};
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        
        if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0) {
            char ch;
            read(STDIN_FILENO, &ch, 1);
            if (ch == 'q' || ch == 'Q' || ch == 27) { // ESC键
                app.running = 0;
                break;
            }
        }
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    // 计算统计信息
    double elapsed = (end.tv_sec - start.tv_sec) + 
                    (end.tv_nsec - start.tv_nsec) / 1e9;
    double fps = frame_count / elapsed;
    
    if (app.verbose) {
        printf("\n捕获统计:\n");
        printf("  总帧数: %ld\n", frame_count);
        printf("  总时间: %.2f秒\n", elapsed);
        printf("  平均帧率: %.2f FPS\n", fps);
    }
    
    close_framebuffer(buf);
    return NULL;
}

void benchmark_mode() {
    printf("性能测试模式...\n");
    
    GraphicsBuffer* buf = open_framebuffer("/dev/fb0");
    if (!buf) {
        fprintf(stderr, "无法打开帧缓冲区\n");
        return;
    }
    
    DisplayConfig config = {
        .output_width = 80,
        .output_height = 24,
        .color_mode = COLOR_TRUE,
        .charset = CHARSET_SIMPLE,
        .brightness = 1.0,
        .contrast = 1.0,
        .fps = 0  // 最大速度
    };
    
    char* output = NULL;
    struct timespec start, end;
    const int iterations = 100;
    
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    for (int i = 0; i < iterations; i++) {
        if (convert_buffer_to_text(buf, &config, &output) == 0) {
            free(output);
        }
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    double elapsed = (end.tv_sec - start.tv_sec) + 
                    (end.tv_nsec - start.tv_nsec) / 1e9;
    double fps = iterations / elapsed;
    
    printf("测试结果:\n");
    printf("  迭代次数: %d\n", iterations);
    printf("  总时间: %.3f秒\n", elapsed);
    printf("  处理速度: %.2f FPS\n", fps);
    printf("  每帧时间: %.2f ms\n", 1000.0 / fps);
    
    close_framebuffer(buf);
}

void interactive_mode() {
    print_banner();
    printf("交互式模式\n\n");
    
    int choice;
    do {
        printf("1. 检测图形服务器\n");
        printf("2. 捕获并显示屏幕\n");
        printf("3. 连接到远程服务器\n");
        printf("4. 性能测试\n");
        printf("5. 列出可用设备\n");
        printf("6. 配置显示选项\n");
        printf("0. 退出\n");
        printf("\n请选择: ");
        
        scanf("%d", &choice);
        
        switch (choice) {
            case 1:
                detect_servers();
                break;
                
            case 2: {
                printf("开始捕获屏幕...\n");
                printf("按 Q 键退出捕获模式\n");
                
                // 配置显示
                DisplayConfig config = {
                    .output_width = 80,
                    .output_height = 24,
                    .color_mode = COLOR_TRUE,
                    .charset = CHARSET_BRAILLE,
                    .brightness = 1.0,
                    .contrast = 1.0,
                    .fps = 10,
                    .continuous = 1
                };
                
                // 设置终端
                setup_terminal();
                app.running = 1;
                
                // 启动捕获线程
                pthread_create(&app.capture_thread, NULL, capture_thread_func, &config);
                
                // 等待线程结束
                pthread_join(app.capture_thread, NULL);
                
                // 恢复终端
                restore_terminal();
                break;
            }
                
            case 3:
                printf("连接到远程服务器功能开发中...\n");
                break;
                
            case 4:
                benchmark_mode();
                break;
                
            case 5:
                list_available_devices();
                break;
                
            case 6:
                printf("配置显示选项功能开发中...\n");
                break;
                
            case 0:
                printf("退出\n");
                break;
                
            default:
                printf("无效选择\n");
                break;
        }
        
        printf("\n");
    } while (choice != 0);
}

int connect_to_server(ServerConfig* config) {
    printf("连接到服务器: ");
    
    switch (config->type) {
        case SERVER_FRAMEBUFFER:
            printf("本地帧缓冲区\n");
            // 已经在capture模式中处理
            return 0;
            
        case SERVER_X11:
            printf("X11服务器\n");
            if (strlen(config->display) == 0) {
                const char* env_display = getenv("DISPLAY");
                if (env_display) {
                    strcpy(config->display, env_display);
                } else {
                    strcpy(config->display, ":0");
                }
            }
            printf("显示: %s\n", config->display);
            // 这里可以添加X11连接代码
            return 0;
            
        case SERVER_VNC:
            printf("VNC服务器\n");
            if (strlen(config->host) == 0) {
                printf("需要指定主机名\n");
                return -1;
            }
            printf("主机: %s:%d\n", config->host, config->port);
            // 这里可以添加VNC连接代码
            return 0;
            
        default:
            printf("不支持的服务器类型\n");
            return -1;
    }
}

void list_available_devices() {
    printf("可用设备:\n\n");
    
    // 帧缓冲区
    printf("帧缓冲区:\n");
    for (int i = 0; i < 4; i++) {
        char device[32];
        snprintf(device, sizeof(device), "/dev/fb%d", i);
        
        if (access(device, F_OK) == 0) {
            printf("  %s\n", device);
        }
    }
    
    // X11显示
    printf("\nX11显示:\n");
    const char* display = getenv("DISPLAY");
    if (display) {
        printf("  %s\n", display);
    } else {
        printf("  未设置DISPLAY环境变量\n");
    }
    
    // Wayland显示
    printf("\nWayland显示:\n");
    const char* wayland_display = getenv("WAYLAND_DISPLAY");
    if (wayland_display) {
        printf("  %s\n", wayland_display);
    } else {
        printf("  未设置WAYLAND_DISPLAY环境变量\n");
    }
}

void signal_handler(int sig) {
    printf("\n收到信号 %d，正在退出...\n", sig);
    app.running = 0;
}

int main(int argc, char *argv[]) {
    // 初始化默认配置
    app.display.output_width = 80;
    app.display.output_height = 24;
    app.display.color_mode = COLOR_TRUE;
    app.display.charset = CHARSET_BRAILLE;
    app.display.brightness = 1.0;
    app.display.contrast = 1.0;
    app.display.fps = 10;
    app.display.continuous = 0;
    app.display.region_x = 0;
    app.display.region_y = 0;
    app.display.region_w = 0;
    app.display.region_h = 0;
    
    app.server.type = SERVER_FRAMEBUFFER;
    strcpy(app.server.display, ":0");
    app.server.port = 5900;
    
    app.running = 1;
    app.verbose = 0;
    app.benchmark = 0;
    
    // 初始化颜色表
    init_color_table();
    
    // 设置信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // 解析命令行参数
    static struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 'V'},
        {"capture", no_argument, 0, 'c'},
        {"connect", no_argument, 0, 'C'},
        {"interactive", no_argument, 0, 'i'},
        {"benchmark", no_argument, 0, 'b'},
        {"list", no_argument, 0, 'l'},
        {"verbose", no_argument, 0, 'v'},
        {"device", required_argument, 0, 'd'},
        {"width", required_argument, 0, 'w'},
        {"height", required_argument, 0, 'H'},
        {"fps", required_argument, 0, 'f'},
        {"continuous", no_argument, 0, 'R'},
        {"color", required_argument, 0, 'C'},
        {"charset", required_argument, 0, 's'},
        {"brightness", required_argument, 0, 'B'},
        {"contrast", required_argument, 0, 'T'},
        {"server", required_argument, 0, 'S'},
        {"display", required_argument, 0, 'D'},
        {"host", required_argument, 0, 'H'},
        {"port", required_argument, 0, 'P'},
        {"username", required_argument, 0, 'u'},
        {"password", required_argument, 0, 'p'},
        {0, 0, 0, 0}
    };
    
    int opt;
    int option_index = 0;
    int mode = 0; // 0=help, 1=capture, 2=connect, 3=interactive, 4=benchmark, 5=list
    
    while ((opt = getopt_long(argc, argv, "hVcCiblvd:w:H:f:RC:s:B:T:S:D:H:P:u:p:", 
                              long_options, &option_index)) != -1) {
        switch (opt) {
            case 'h':
                print_help();
                return 0;
            case 'V':
                printf("Graphics Commander v%s\n", VERSION);
                return 0;
            case 'c':
                mode = 1;
                break;
            case 'C':
                if (strcmp(argv[optind-1], "--connect") == 0) {
                    mode = 2;
                } else {
                    // 处理颜色模式
                    if (strcmp(optarg, "none") == 0) app.display.color_mode = COLOR_NONE;
                    else if (strcmp(optarg, "basic") == 0) app.display.color_mode = COLOR_BASIC;
                    else if (strcmp(optarg, "256") == 0) app.display.color_mode = COLOR_256;
                    else if (strcmp(optarg, "true") == 0) app.display.color_mode = COLOR_TRUE;
                    else if (strcmp(optarg, "gray") == 0) app.display.color_mode = COLOR_GRAY;
                }
                break;
            case 'i':
                mode = 3;
                break;
            case 'b':
                mode = 4;
                break;
            case 'l':
                mode = 5;
                break;
            case 'v':
                app.verbose = 1;
                break;
            case 'd':
                // 设备参数
                break;
            case 'w':
                app.display.output_width = atoi(optarg);
                break;
            case 'H':
                if (strcmp(argv[optind-1], "--height") == 0) {
                    app.display.output_height = atoi(optarg);
                }
                break;
            case 'f':
                app.display.fps = atoi(optarg);
                break;
            case 'R':
                app.display.continuous = 1;
                break;
            case 's':
                if (strcmp(optarg, "simple") == 0) app.display.charset = CHARSET_SIMPLE;
                else if (strcmp(optarg, "blocks") == 0) app.display.charset = CHARSET_BLOCKS;
                else if (strcmp(optarg, "half") == 0) app.display.charset = CHARSET_HALF;
                else if (strcmp(optarg, "braille") == 0) app.display.charset = CHARSET_BRAILLE;
                else if (strcmp(optarg, "art") == 0) app.display.charset = CHARSET_ART;
                break;
            case 'B':
                app.display.brightness = atof(optarg);
                break;
            case 'T':
                app.display.contrast = atof(optarg);
                break;
            case 'S':
                if (strcmp(optarg, "fb") == 0) app.server.type = SERVER_FRAMEBUFFER;
                else if (strcmp(optarg, "x11") == 0) app.server.type = SERVER_X11;
                else if (strcmp(optarg, "wayland") == 0) app.server.type = SERVER_WAYLAND;
                else if (strcmp(optarg, "vnc") == 0) app.server.type = SERVER_VNC;
                else if (strcmp(optarg, "rdp") == 0) app.server.type = SERVER_RDP;
                break;
            case 'D':
                strcpy(app.server.display, optarg);
                break;
            case 'P':
                app.server.port = atoi(optarg);
                break;
            default:
                print_help();
                return 1;
        }
    }
    
    // 如果没有指定模式，显示帮助
    if (mode == 0 && argc > 1) {
        print_help();
        return 0;
    }
    
    // 根据模式执行
    switch (mode) {
        case 1: // 捕获模式
            if (!app.verbose) {
                print_banner();
            }
            printf("开始捕获屏幕...\n");
            printf("按 Q 键退出\n\n");
            
            setup_terminal();
            app.running = 1;
            pthread_create(&app.capture_thread, NULL, capture_thread_func, &app.display);
            pthread_join(app.capture_thread, NULL);
            restore_terminal();
            break;
            
        case 2: // 连接模式
            print_banner();
            connect_to_server(&app.server);
            break;
            
        case 3: // 交互模式
            interactive_mode();
            break;
            
        case 4: // 性能测试
            print_banner();
            benchmark_mode();
            break;
            
        case 5: // 列出设备
            print_banner();
            list_available_devices();
            break;
            
        default:
            // 如果没有参数，进入交互模式
            if (argc == 1) {
                interactive_mode();
            } else {
                print_help();
            }
            break;
    }
    
    return 0;
}
