# AirPlayServer 个人修改版

这是 today080221 自用的 Windows x64 修改版，不是上游官方发布。

上游稳定基线：xenos1337/AirPlayServer v1.2.5，提交
`85bb8dcbe82f75f40f30bb603255668f9162a8f9`。
2026-10-01 刷新时，上游 master 仅多出发布包命名提交 `aa9ce92`，本版不依赖该提交。
本地版本、源码提交和逐文件 SHA256 见便携包的 `BUILD-INFO.json` 与 `SHA256SUMS.txt`。

## 使用

1. 将 ZIP 完整解压到新目录，运行 `AirPlayServer.exe`。不要覆盖旧安装。
2. 需要 Windows 10/11 x64，以及已经安装并运行的 Apple Bonjour 服务。
   本机已有 Bonjour；此包不安装服务，也不修改系统网络配置。
3. 程序显示 `Ready` 后，在同一局域网的 iPhone 控制中心打开“屏幕镜像”，选择电脑。
   `Ready` 只表示接收服务已启动，不表示手机已经连接或音视频已通过验收。
4. 默认优先使用 TCP 5001（RAOP）和 7001（AirPlay）。冲突时自动选择空闲端口；
   不需要关闭代理或改变系统动态端口范围。
5. 若显示 `Offline`，关闭程序后双击 `启动调试.cmd`。
   日志在 `%LOCALAPPDATA%\AirPlayServer\logs`，搜索 `TCP listening` 查看请求端口、实际端口及回退错误码。
   7100 是上游保留的固定兼容入口；该端口不可用仍会导致完整启动失败并回滚。

包内附带应用需要的 DLL 及 MSVC 应用本地运行库。Windows 自带组件和 Bonjour 服务仍由系统提供。
按实际 PE 导入检查，此构建不需要旧发布包中的 `msys-2.0.dll`，因此不附带它。
配置文件保存在解压目录，配对种子沿用上游 `%APPDATA%\MirrorSim` 路径；本版不是完全无痕应用。
新目录不一定匹配旧安装的防火墙“程序路径”规则。若本机监听/Bonjour 查询正常但手机仍不可见，
需另行确认新路径的局域网入站权限；本次没有修改防火墙。

## 修改摘要

- Windows TCP 独占绑定，取消 TCP 的 `SO_REUSEADDR`；只在 bind 返回 10048/10013 时回退一次到端口 0。
  同一个 socket 保持所有权，不做“探测后关闭再绑定”。其他错误直接失败并记录诊断。
- RAOP、AirPlay 的 Bonjour SRV 记录使用实际绑定端口；RAOP `SETUP` 的 `eventPort` 同步实际端口。
  `fgServerGetPorts` 可查询有效端口，`fgServerIsRunning` 检查三个接收入口。
- 修复 HTTP 服务互斥量初始化、线程创建失败回滚、关闭时 socket 所有权和部分初始化的资源清理。
  回调对象保留到工作线程全部停止；修正 `CAirServer` 析构顺序，移除无同步保障的停机等待。
- 修复 SETUP 的 IV/密钥长度、字段类型/缺失字段读取、FairPlay 模式索引检查；控制请求体上限 8 MiB。
- 主窗口标明 Personal，失败显示 Offline，并指向调试日志。
- 保留外部 DNS-SD 模式的固定端口约定：若端口变化则明确失败并清理，避免外部宿主仍广播旧端口。
  便携桌面版默认由程序自行注册 Bonjour，不使用该模式。

## 可复现构建与验收

从本 fork 对应提交运行以下 PowerShell 7 命令。需要已安装的 VS 2026 v145、Windows SDK；
不执行安装、升级或修改系统配置。SDK 本次使用 10.0.26100.0，编译器 14.50.35717。

```powershell
pwsh -NoProfile -File scripts/Build-Windows.ps1 -VisualStudio 'N:\Microsoft Visual Studio\18\Enterprise'
pwsh -NoProfile -File scripts/Test-Windows.ps1 -TestRaopPort 45001 -TestAirplayPort 47001 -ExpectAdapterPortConflict
pwsh -NoProfile -File scripts/Package-Windows.ps1 -VisualStudio 'N:\Microsoft Visual Studio\18\Enterprise'
pwsh -NoProfile -File scripts/Test-Package.ps1 -Archive '<上一行输出的便携 ZIP 路径>'
```

第二行是本机 5001/7001 有其他程序占用时使用的验收方式：完整矩阵在 45001/47001 上执行同一生产绑定代码，
随后使用实际 5001/7001 再做 Bonjour、协议响应和退出测试。若两个默认端口均空闲，去掉第二行三个参数，
即可在真实 5001/7001 上执行完整矩阵以及上游 adapter 的正常启动检查。
`-ExpectAdapterPortConflict` 要求实际出现明确的外部广播端口冲突错误，不能用来忽略其他错误。

脚本依次构建 solution、构建回归程序、执行测试，最后打包已验证且哈希未变化的文件。
编译日志和测试证据在 `build`；ZIP、其 SHA256 和源码快照在 `dist`。
构建参数可复现相同功能产物，不承诺跨机器/工具链逐字节相同的 PE 或 ZIP。

## 验收边界

详见包内 `SOURCE-REVIEW.zh-CN.md`、`test-results`。
没有真实 iPhone 参与，尚未验收手机发现、PIN 配对、连续音视频、旋转、断线重连与长时间播放。
本次源码检查和合成协议测试不等于完整安全审计，也不证明所有投屏兼容性问题已解决。
