# macOS 构建

本目录基于 `dev` 分支提交 `803e61adb8283f9090170c7f34fd1a163dc1c78f`。
2026-10-07 已在 Apple Silicon 上用 Apple Clang 17.0.0、CMake 4.4.2 和 Ninja 构建 Release。

## 重新构建

需要 Xcode Command Line Tools、Git、CMake；下面命令使用 Ninja。
依赖下载沿用当前 shell 的 `https_proxy`、`http_proxy` 和 `all_proxy`。

```sh
cd /Users/west/Developer/simple_CAR-dev
JOBS=8 ./setup.sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 8
./build/simpleCAR model.btor2
```

源码支持 `.aig`、`.aag` 和 `.btor2` 输入。默认使用 IC3 和 minicore。
独立 SAT 命令行程序为 `build/minicore`。
若选择其他 CMake generator，请使用新的构建目录，避免与已有 Ninja 缓存冲突。

## 本地适配

- macOS 默认使用 `/usr/bin/clang` 和 `/usr/bin/clang++`，关闭 LLD、ThinLTO 及其隐式 IPO，避免混用 Homebrew LLVM 头文件和系统 libc++。显式编译器参数仍受尊重；本次只验证 Apple Clang。
- macOS 不传 Linux 的静态链接、strip 和 ThinLTO jobs 参数。
- 从公共 include 路径移除 `src/model/aiger`，项目内改用 `aiger/aiger.h`，避免 AIGER 的 `version` 文件遮蔽标准头 `<version>`。
- 启用 libc++ 的 `_LIBCPP_ENABLE_CXX17_REMOVED_RANDOM_SHUFFLE` 兼容开关，使现有 FCAR/IC3 调用可以在 C++17 下编译；仍有弃用警告。
- `setup-btor2-deps.sh` 使用 curl 和固定源码版本，配置 CMake 4 的旧项目兼容策略，不依赖 wget、nproc 或未使用的 AIGER 下载。已有依赖目录保留。
- `setup.sh` 为依赖选择同一套 Apple Clang，保留上游 MiniSAT 补丁和 CaDiCaL 的 kitten 符号前缀处理。

依赖子模块保持仓库指定提交；MiniSAT 的工作区修改来自上游 `src/sat/minisat.patch`。
`setup-btor2-deps.sh` 中固定了 Boolector bitblast-api、内部 btor2parser 和 Lingeling 的版本。

## 验证与已知限制

两个主程序均为 Mach-O arm64；simpleCAR 的动态依赖仅为系统 libSystem 和 libc++。
构建日志、源码提交和子模块版本保存在 `build/logs/`。

冒烟验证共 22 项，21 项通过：

- minicore、MiniSAT、CaDiCaL 的 AIGER IC3 Safe/Unsafe，以及 Kissat 的常量安全情况；
- 默认后端的 BTOR2 IC3 Safe/Unsafe、AIGER 和 BTOR2 FCAR Safe/Unsafe；
- 原生数组 WLBMC：有反例返回 Unsafe，深度 3 无反例返回 Unknown；
- minicore 独立 CNF SAT/UNSAT；
- 4 个 BTOR2 模型通过 catbtor parser/type 检查。

未通过项是 `-s kissat` 的 AIGER IC3 反例：上游 `KissatSolver::GetAssignment` 明确抛出 unsupported，进程因此异常终止。
这是现有适配器能力限制，未在本次 macOS 编译任务中扩展它。原生数组 WLBMC 直接使用 Kissat 的验证已通过。
`simpleCAR -h` 可以打印帮助，但上游 CLI 处理会返回退出码 1。

完整命令、模型和输出位于 `build/smoke/`；`results.json` 保留失败项。
可复现本次检查：

```sh
python3 build/smoke/run.py
```

该脚本因保留 Kissat/IC3 失败项，预期返回 1。这些是小模型的构建/运行检查，不代表完整算法回归或语义正确性证明。
