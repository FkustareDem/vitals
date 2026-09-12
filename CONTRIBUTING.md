# 参与开发

## 环境

| 需要 | 说明 |
|---|---|
| Visual Studio 2022 Build Tools | 勾选「使用 C++ 的桌面开发」 |
| Windows 10 SDK | 装 VS 时一起勾 |
| Windows 10 1809+ | 用到 `CreateWaitableTimerEx(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)`，更老的系统会自动退化成 `WM_TIMER` 驱动 |

不需要 CMake、不需要 vcpkg、**零第三方依赖** —— 只用 `user32/gdi32/advapi32/shell32/dwmapi`。

## 编译 / 运行

```bat
build.bat            :: 编译
vitals.exe           :: 运行
pack.bat             :: 编译 + 打包成 dist\vitals-win64.zip
```

`vitals.exe` 第一次运行会在同目录自动生成 `vitals.ini`（默认配置编译在 exe 资源里）。

## 改完代码请务必先跑这几个检查

这几个是踩坑踩出来的，**光靠肉眼看界面是发现不了这些问题的**：

```bat
vitals.exe --selftest   :: 字体体检 + 整行宽度恒定 + 箭头频率
vitals.exe --bench      :: 各段耗时 / 实跑帧率 / 帧间隔标准差 / 掉帧次数
vitals.exe --ftrace     :: 逐帧时间戳写进 ftrace.txt(跑6秒自动退出)
vitals.exe --setlog     :: 记录设置界面的点击坐标和命中行
```

- **改了字体或模板** → 看 `--selftest` 的「中/英比」是不是 **2.00**、以及「宽度变化」是不是 **0**
- **改了渲染或时序** → 看 `--bench` 的「帧间隔标准差」（应 < 0.5ms）和「明显掉帧」（应为 0）
- **界面上"看着卡但说不清"** → 上 `--ftrace`，把每帧间隔导出来看，别猜

## 几条硬规矩

1. **`cl` 必须带 `/utf-8`** —— 源码里有中文，不加会报 `C2001 常量中有换行符`
2. **`.bat` 里只写 ASCII** —— 中文注释会被 cmd 按 GBK 解码，能把整个脚本搞崩
3. **`.rc` 必须先编译成 `.res`** 再链接（`build.bat` 已经处理了）
4. **别用文本模式以外的模式打开 ini** —— `_wfopen(..., L"wb, ccs=UTF-8")` 里的 `b` 会让 CRT 忽略 `ccs`，结果写出 UTF-16
5. **单文件**：所有代码都在 `vitals.c`，请保持这个结构（分发和编译都省事）

## 代码风格

- C99，4 空格缩进，不写 tab
- 注释用中文，**说明"为什么"而不是"做了什么"** —— 踩过的坑尤其要写清楚
- 巨硬的命名沿用现有风格：`g_` 前缀是全局，`static` 能加就加

## 提交

1. Fork → 新建分支（`fix/xxx` 或 `feat/xxx`）
2. 改完跑一遍上面那四个检查
3. 提交信息写清楚**现象 → 根因 → 修法**，比只写「修复bug」有用得多
4. 开 PR，贴上 `--bench` / `--selftest` 的输出
