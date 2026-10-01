# ASCII Art Player

控制台 ASCII 艺术动画播放器：播放帧序列（TXT/RTF）+ 音频的同步动画，支持多歌曲、SRT 字幕、帧字符覆盖、暂停/跳转等特性。

## 功能特性

- **帧动画渲染**：自动检测 `frame(N).txt` 与 `frame(N).rtf`；RTF 支持真彩色 VT100 输出，自动隐藏 "ASCII Generator" 水印
- **多歌曲支持**：exe 同目录下每个子文件夹为一首歌，内含音频、帧文件、配置
- **两种音频模式**：MCI 播放音频文件（mp3/wav/mid/ogg/flac/aac/m4a）；BGM 类播放简谱乐谱（`use_bgm=1`）
- **播放控制**：暂停/恢复（空格/ESC）、暂停后输入帧号跳转、mm:ss 时间显示、进度条
- **SRT 字幕**：可选，支持中文、双行、时间偏移
- **帧字符覆盖**：override 配置可对指定帧/行/列替换字符，支持保护帧与 RGB 颜色
- **跳帧机制**：基于真实时钟（GetTickCount64）同步音画，帧丢失自动用上一帧代替；调试模式显示跳帧统计
- **全链路宽字符**：emoji / 中文路径 / 中文与表情字符渲染

## 目录结构

```
ASCII Art Player/
├─ start.exe            编译产物（发布时放在歌曲父目录）
├─ autoplay.txt         自动播放配置（0=选歌界面，N=直接播第N首）
├─ override.txt         全局帧字符覆盖（对所有歌曲生效，单独配置优先）
├─ start.cpp            主程序源码
├─ music.h              BGM 简谱播放器源码
└─ 歌曲1/               每个子文件夹一首歌
   ├─ 音乐.mp3          音频（自动检测）
   ├─ config.ini        歌曲配置
   ├─ override.txt      单独帧字符覆盖（优先于全局）
   ├─ music.txt         use_bgm=1 时的简谱乐谱
   ├─ 歌词.srt          可选字幕
   └─ resource/
      ├─ frame(0).txt / .rtf
      └─ frame(1).txt ...
```

## 配置说明

**config.ini**（每首歌一份，默认值）：

```
width=79           窗口宽度（实际 = width+10）
height=24          窗口高度（实际 = height+10）
debug=0            调试模式（1=显示帧格式检测/跳帧统计）
fps=25             播放帧率（1~120）
use_bgm=0          0=MCI音频, 1=BGM简谱乐谱
total_frames=7739  总帧数（帧编号 0~total_frames）
font_size=10       播放时字号
subtitle_offset=0  字幕偏移(秒)，正数=延迟，负数=提前
```

**override.txt**（每行一个条目）：

```
帧表达式 行表达式 列表达式 字符 [protected] [color]
```

- 帧/行/列表达式支持：单值 `100`、区间 `100-200`、取模 `%2==0`、取模+区间 `%2==0(10-20)`
- 字符可用英文双引号包裹以包含空格，如 `"hello world"`
- `protected=1` 使该帧不可被跳帧机制跳过
- `color=R,G,B`（0-255）指定覆盖字符颜色，不填则继承原字符颜色

**autoplay.txt**（exe 同目录）：第一个有效整数，`0`=正常选歌界面，`N`=直接进入第 N 首歌曲播放；支持 `#` 注释与空行。

**选歌界面**：输入歌曲编号开始播放，输入 `0` 查看帮助。

## 编译

```bash
g++ start.cpp -o start.exe -static -static-libgcc -static-libstdc++ -lwinmm -lpthread
```

`-static` 全静态链接后，exe 仅依赖 Windows 10 自带系统库（KERNEL32/WINMM/UCRT），可拷贝到任意未安装编译器的 Windows 10 上直接运行。

## 运行

1. 将 `start.exe` 放在歌曲目录的父目录
2. 双击运行，选择歌曲编号（或由 `autoplay.txt` 直接播放）
3. 空格/ESC 暂停，暂停后输入帧数+回车跳转

## 平台

Windows（MSVC / MinGW）。
