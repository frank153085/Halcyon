# Halcyon

Halcyon 是一个面向个人学习的 实时渲染器。当前仓库
处于 M0/M1 纵向切片：先把生命周期、同步和错误路径做可靠，再逐步加入
更复杂的渲染算法。

## 当前里程碑：M0/M1

- MSVC v143 + Ninja 的可复现 Debug、RelWithDebInfo Preset；
- `HalcyonCore`、`HalcyonRenderer`、`HalcyonSandbox`、`HalcyonCooker` 分层；
- Vulkan 1.3 实例/设备选择，Validation Messenger，Dynamic Rendering、
  Synchronization2 和 Timeline Semaphore；
- 三帧 Frame Context、Swapchain 重建、最小化/Resize/Out-of-Date 处理；
- Reversed-Z 相机约定（D32 深度路径在 M1 中启用时使用 `GREATER_OR_EQUAL`）；
- HLSL + DXC 的构建期 SPIR-V 检查，并保留内嵌三角形 shader 作为离线回退；
- CPU 侧稳定 generation handle、延迟删除和上传环基础测试。

高级 GPU-driven、Visibility Buffer、光追和帧预算控制器会在 M2 以后实现。仓库中已有的 RenderGraph、Bindless 和
帧预算原型默认不参与 M0/M1 构建；需要单独研究时可配置
`-DHALCYON_BUILD_EXPERIMENTAL_M2=ON`。

## 构建

要求 Windows 11、Visual Studio 2022 C++ Build Tools（MSVC v143）、CMake
3.28 或更新版本、Ninja、Vulkan SDK 1.3 或更新版本。请从 Developer
PowerShell/Command Prompt（或配置好 MSVC 环境的 CLion）执行：

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug
```

性能配置使用：

```powershell
cmake --preset windows-msvc-relwithdebinfo
cmake --build --preset windows-msvc-relwithdebinfo
ctest --preset windows-msvc-relwithdebinfo
```

如果找不到 DXC，构建会给出 warning，但仍可使用源码中内嵌的 M1 shader
运行；安装 Vulkan SDK 后重新配置即可启用 HLSL 校验目标。

## 运行 Sandbox

```powershell
out\build\windows-msvc-debug\Halcyon.exe --frames 300
```

可用选项：`--width N`、`--height N`、`--frames N`、`--no-validation` 和
`--help`。窗口最小化时渲染循环暂停获取图像，恢复后自动重建 Swapchain。

## 目录

```text
Source/Core                 与后端无关的 Result、日志和稳定句柄
Source/Renderer/Scene       相机与 FramePacket 数据约定
Source/Renderer/Graph       M2 RenderGraph 原型（默认不构建）
Source/Renderer/Resources   M1 上传/延迟删除与 M2 Bindless 原型
Source/Renderer/Vulkan      Vulkan 1.3 后端
Source/Sandbox              可运行演示
Source/Cooker               确定性资源清单工具
Tests                       CPU 单元测试
```

每次里程碑验收都应保存一次 RenderDoc capture、截图和性能 CSV；性能只在
同一硬件、驱动、分辨率和场景条件下比较。
