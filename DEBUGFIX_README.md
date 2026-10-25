# 3.5 Beta 2 调试修复与升级安装包

基线：官方 master 提交 `cbe084d`，原始 [version.json](https://raw.githubusercontent.com/royqh1979/RedPanda-CPP/master/version.json) 为 `3.5.0-beta.2`。界面版本统一显示为 `3.5 Beta 2`，主标题为“小熊猫C++ 3.5 Beta 2”；机器可读的预发布字段仍为 `beta.2`。这是本地修复包，不是官方发布包。

## 安装

运行 `dist/RedPanda-3.5-beta2-r5-Setup.exe`。本次仅发布 Windows x64 安装 EXE。安装器及 IDE 均显示 `3.5 Beta 2`，不显示构建计数或修复版后缀。安装升级流程沿用原版：

1. 根据安装范围和 32/64 位注册表记录自动找到旧安装目录。
2. 安装前检查 IDE 是否占用程序文件；提示先保存工程并关闭 IDE，然后重试。不会强行终止 IDE。
3. 静默卸载旧版，检查返回状态，成功后安装新版程序和所选组件。
4. 默认保留配置。选中原版的“删除原有配置文件”或“安全”安装类型仍会清除配置。

没有新增备份或恢复机制。原卸载器会删除自带工具链；本包重新安装 MinGW-w64 GCC 11.5.0 / GDB 10.2 的 64 位工具链。原安装的 `mingw32` 目录缺少 GCC/GDB，故未作为组件提供。旧工具链目录内用户增加的文件可能被原卸载流程删除；外部工具链不由安装器处理。

常规 `.dev` 工程和用户源码不在卸载清单内。不要把需保留的自定义文件放在原卸载器会递归删除的 `templates` 或工具链目录内。默认配置仍使用原来的 AppData 位置，安装包不包含本地验证用的便携配置。

## 调试修复

- Windows 默认开启 GDB Server。首次读取旧配置时，无论原选项缺失、关闭还是开启，均启用并持久保存；`gdb_server_default_version=1` 标记此次迁移。迁移只改变此开关和标记，保留原 TCP 端口、工具链路径及其他配置。迁移后用户手动关闭仍会保留，不会每次启动强制开启。
- 调试会话中新增、清除当前文件和清除全部断点同步到 GDB。
- GDB Server 模式下，运行中修改断点先中断并等待实际停止事件，同步成功后恢复原运行状态；用户主动暂停、真实断点/异常和同步失败均保持暂停。
- 暂停时可双击监视窗口的“值”列修改有效标量或结构成员。运行中、无效值和聚合对象不可编辑。
- 赋值表达式按 GDB/MI 转义，界面由调试器读回值更新；非法赋值不伪造成功值。
- 保留 3.5 的从 0 开始的编辑器行号约定、监视表达式格式和新版停止信号接口。

## r4 问题核对与修复

- #676：原帖所示的 `setFocus` 条件逻辑在 3.5 基线已经移除。用生产停止事件和主窗口验证：从另一个文件暂停到有效源码位置时，正确切换文件并定位行号。本次未改动暂停逻辑；GDB 停在无源码的系统 DLL 时仍按原逻辑显示调用栈。
- #621：仅修正安装器的八类文件关联命令，为程序路径加引号；保留 IDE 内已有的关联设置逻辑。
- #766：仅对单行剪贴板粘贴保留空格、Tab 和原缩进，支持撤销/重做；多行粘贴和键盘输入的自动缩进逻辑不变。
- #609：仅使位宽选项 `-m32/-m64/-mx32` 同时进入 C/C++ 编译和链接参数，不改变其他选项的分类。这不等于安装包增加了 32 位工具链。
- #671：仅将简体中文菜单的 C/C++ 在线参考地址改为 `cppreference.cn`；本地 CHM 优先逻辑及英文地址不变。

## r5 随包构建工具

- 随包部署官方 Windows x64 CMake 4.4.4 完整便携目录及 Ninja 1.13.2，保留各自许可文件。版本、官方地址和 SHA256 固定在 `platform/windows/build-tools.json`；开发构建会自动下载并校验到工作区 `.tools` 缓存，二进制不提交到 Git。
- 安装目录为 `tools/cmake`、`tools/ninja`，工具版本信息保存在 `tools/build-tools-manifest.json`。卸载只移除这两个由安装器管理的目录和标记文件，保留其他用户工具。
- IDE 的构建子进程及命令行窗口按当前 IDE 目录自动定位。仅调整子进程的 PATH，并在未指定时提供 CMAKE_HOME；不改系统或用户环境变量，不保存绝对工具路径。已有有效 CMAKE_HOME 优先，显式无效值也不会被擅自覆盖。
- `.dev/Makefile` 入口、已选 Make 程序、编译器及生成器保持原样，不把 Make 替换成 Ninja，也不新增原生 CMake 项目类型。项目脚本中原有的显式路径和项目自带工具优先规则继续有效；通过 PATH/CMAKE_HOME 查找工具的脚本可直接使用随包工具。
- 官方 CMake 4.3+ Windows 二进制要求 Windows 10 以上，因此该工具包在 Windows 10/11 部署并自动启用；较早系统保留原有 IDE/Makefile 及工具查找流程，不提高 IDE 原安装系统门槛。
- Python、PLECS 等项目依赖仍沿用原配置。MinGW 中的 `mingw32-make.exe` 继续保留，供既有工程及 LTO 脚本使用。

## 构建与验证

```powershell
.\build_debugfix.ps1 -RunTests -Installer
# 已完成构建、部署和测试后，仅重新打包：
.\build_debugfix.ps1 -PackageOnly -Installer
```

使用本机 Qt 5.15.2 / MSVC 2022 编译 IDE，本包中的目标工程仍使用 MinGW/GDB。可通过 `-QtDir`、`-ToolchainRoot` 指定依赖。

验证记录：

- `artifacts/debugwatch-beta35-tests.txt`：29/29 通过，无跳过。新增工具缺失回退、含空格/中文目录与移动后的定位、CMAKE_HOME 保留、不完整包不接管，以及通过生产构建进程执行 GNU Make → CMake → Ninja → 测试程序的真实集成验证。
- `build-beta35-r5-regression.log`：定向回归及其余三组 CTest 全部通过；共享编辑器 363 项检查使用产品相同的 Windows 原生字体后端。
- `artifacts/gdb-server-startup-migration.json`：实际启动新版 IDE，将独立旧配置中的 `false` 迁移为 `true` 并落盘，保留自定义端口 `41239`。主窗口标题为“小熊猫C++ 3.5 Beta 2”。
- `artifacts/live-gdb-beta35-smoke.log`：真实独立 GDB/MI 会话通过赋值读回、非法值、条件断点、删除、运行中暂停后新增并命中断点、正常退出。
- `artifacts/upgrade-tests-r5b/verification.json`：隔离注册表与测试载荷覆盖旧版升级、重复安装、八类文件关联，以及工具包和许可文件安装/卸载、用户自定义 tools 子目录保留。
- `artifacts/upgrade-tests-r5b/uninstall-failure.json`：旧卸载器失败时，在覆盖程序文件前中止。
- `artifacts/installer-r5-integrity.txt` 与 `artifacts/installer-r5-payload-verification.json`：最终安装包完整性及完整工具目录文件校验。
- `tests/verify_bundled_tools.py`：针对最终解包后的工具运行版本、x64 架构、模块/许可文件、CMake+Ninja 配置/编译/运行检查。验证目录含空格和中文，记录于 `artifacts/bundled-tools-r5-native-smoke/verification.json`。
- `artifacts/pmsm-server-running-breakpoint.log`：已有 PMSM Windows SIL EXE 在独立 GDB Server 会话中，运行后暂停、插入 `AD_IO_DA.c:164`、恢复并成功命中。未改动该仿真工程。

升级回归使用测试载荷和专用产品标识，没有覆盖本机正在使用的正式安装。新版 IDE 已独立启动并打开测试源码；这不等同于全部 IDE 功能或用户工程的完整回归。安装提示的截图检查因界面授权超时未完成。
