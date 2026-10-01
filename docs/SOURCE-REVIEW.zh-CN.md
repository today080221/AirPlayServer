# 个人修改版源码检查与验收记录

检查日期：2026-10-01。基线为 xenos1337/AirPlayServer v1.2.5，提交
`85bb8dcbe82f75f40f30bb603255668f9162a8f9`；个人 fork 为 today080221/AirPlayServer。
本报告属于本次启动、端口、退出和输入边界专项检查，不是完整安全审计。
交付提交、二进制哈希、测试时间以包内 BUILD-INFO.json 和 test-results/results.json 为准。

## 已确认问题与修改

| 位置 | 源码或运行证据 | 本次处理 |
|---|---|---|
| netutils / httpd | 原实现固定绑定，失败直接返回；Windows TCP 使用 SO_REUSEADDR，存在共享/抢占语义 | TCP 使用 SO_EXCLUSIVEADDRUSE；仅 bind 的 10048/10013 回退端口 0，沿用同一 socket；socket/选项/listen 等错误不重试 |
| RAOP / AirPlay / DNS-SD | 两个入口都由调用方传入固定首选端口；RAOP SETUP 读取 raop->port | 两个入口均返回 getsockname 的实际端口；在工作线程放行前赋值；Bonjour SRV 与 eventPort 使用实际值 |
| httpd | run_mutex 未初始化；失败和退出有不完整清理及跨线程关闭 socket | 初始化/销毁互斥量，初始化无效句柄，处理 listen/线程启动失败；由工作线程关闭监听和已接收连接，停止方 join |
| airplay / raop 部分初始化 | 分配/配对/httpd 失败路径遗漏 logger、pairing、Winsock 配对清理 | 补齐对应释放；固定 7100 失败回滚主监听 |
| CAirServer 析构 | 原顺序是 delete 回调后 stop，若仍有后台服务即可能使用已释放回调 | 改为 stop 后 delete；DLL 同时停止并 join 两类网络生产者，再释放通道、回调引用；移除用于猜测同步完成的 Sleep |
| UI 状态 | 非空服务句柄不能证明工作线程仍然运行 | 查询真实 httpd running 状态；失败显示 Offline，主窗口注明 Personal，日志记录请求/实际端口和错误码 |
| SETUP 输入 | 原 IV memcpy 固定读取 16 字节但未检查外部缓冲长度；密钥长度、字段类型和缺失字段读取不足；合成合法最小 SETUP 曾触发旧 libplist 的缺失字符串读取崩溃 | 校验 IV=16、key=72、timingPort 范围，按字段类型读取，释放临时密钥缓冲；检查解密结果与根节点类型 |
| FairPlay / HTTP | FairPlay 模式字节直接索引 4 项表；HTTP body 持续 realloc 无上限 | 模式限制 0..3；请求体 8 MiB 上限并处理分配失败 |
| 构建依赖 | vcxproj 实际使用 SDL2、SDLRenderer2 与 vendored libplist；CLAUDE 仍写 SDL1/MSYS2 绝对路径 | 更新构建说明，移除无效 MSYS2 搜索路径，现代 MSVC 使用标准 stdint；保留第三方二进制版本 |

CAirServer 原析构顺序是类边界上的确定性生命周期缺陷，但正常 CSDLPlayer
析构原本已先调用 stopServerForShutdown。因此不能据此声称本机曾发生过该 UAF，
也不能把用户的端口冲突故障归因为此。端口占用失败有单独的 bind 10048 证据。

raop_handlers.h 原文件是 UTF-16LE；为维护和编译检查转换为 UTF-8，无意图删除原逻辑。
检查时应按文本比较其内容，不能只看 Git 默认的 binary changed 提示。

## 已执行的自动验收

完整结果与实际端口逐项保存在 test-results 日志中，失败会令脚本返回非零并阻止打包。

- Release x64 solution 和新增回归项目构建通过；不是零警告构建，上游仍有类型转换等警告。
- 上游 AirPlayLibSmokeTests 通过：包括协议信息、显示参数、PIN 拒绝以及镜像状态相关检查。
- 上游 MirrorSimAdapter 测试保留正常启动分支；本机默认端口有真实占用，执行的是
  明确的冲突分支，验证外部 DNS-SD 模式拒绝错误端口、无遗留监听并正常退出。
  不把该结果记成“外部宿主正常投屏已通过”。
- 9 组生产 DLL 集成测试：均空闲；仅 RAOP Listen；仅 AirPlay Listen；同时 Listen；
  同时 Bound；仅 RAOP 出站；仅 AirPlay 出站；同时出站；两个 SO_REUSEADDR 监听占用。
  出站案例由测试自身 bind/connect/accept，验证 SO_ACCEPTCONN=0，并在接收器启动及停止后
  双向传送数据，确认未破坏原连接。
- 每组实际调用 DNSServiceResolve 检查 _raop._tcp 与 _airplay._tcp 的 SRV 端口，
  连接实际端口请求信息；合成 FairPlay/SETUP 检查 eventPort 和动态镜像 dataPort。
- 已接收连接存在时停止、连续 4 次重启、监听关闭、停止后的日志回调稳定性、句柄数量检查。
  句柄断言检查同一负载下 File（包括 socket）/Mutant（互斥量）计数不增长，
  并检查停止后无 airplay2dll 网络工作线程句柄。进程总句柄数只记录诊断，
  因为后台系统/运行库的线程、事件、ETW 注册等会独立变化；不等同于严格内存泄漏分析。
- 固定 7100 绑定失败、Bonjour 名称注册失败、非法端口参数、外部广播端口不匹配均回滚。
- 8 MiB 请求体边界、非法 FairPlay 模式、错误 IV 长度和缺失可选 SETUP 元数据测试通过。
- 桌面程序从隔离目录/配置启动、再次启动、7100 故障启动；比较日志端口与该 PID 的 netstat
  监听，向所属主窗口发送 WM_CLOSE，验收退出码 0、无 crash 日志、无残留监听。
  SDL 无标题辅助窗口不能用作主窗口退出测试目标。

**端口数字的覆盖边界：**没有中断 Clash 或其他占用者。本机完整可控矩阵使用
45001/47001 调用同一生产路径，另以真实 5001/7001 执行发现、协议和退出检查。
实际 7001 出站占用时能回退；早期运行还观察到 5001/7001 同时真实占用时都回退。
不能把这个结果写成“在真实 5001/7001 上逐项完成了全部受控矩阵”。
默认端口空闲的机器可不传参数，直接在 5001/7001 跑同一完整矩阵。

## 便携包、依赖和许可证

脚本只打包通过验收且 SHA256 未变化的 Release x64 文件。应用本地 MSVC CRT 来自
已安装 VS 2026 的可再分发目录，未安装/升级系统组件。PE 导入检查确认本构建不依赖
msys-2.0.dll；Windows 系统 DLL、UCRT 与已运行的 Bonjour 服务仍是运行前提。
许可证、第三方声明、逐文件哈希、源码提交和与二进制对应的源码 ZIP 一并交付。
FFmpeg/SDL2 是上游预编译文件，未在本次从第三方源码重建，不能声称供应链完整可复现。

## 已知限制与待验证事项

1. **没有真实 iPhone 验收。**手机发现、PIN/配对、音视频、旋转、断线重连和长时间播放
   需要用户在同一局域网验证。合成协议成功不能证明加密媒体和全部设备兼容。
2. 7100 仍是固定兼容入口，不在可变 Bonjour 主端口契约内；它被占用会明确失败并清理。
   没有假定可以直接改动旧设备使用该入口的协议约定。
3. 外部 DNS-SD 宿主尚不支持回传动态端口，故端口冲突时明确失败。桌面版的内部 Bonjour
   注册路径支持动态回退。IPv6、Win32、Debug、非 Windows 和多个接收器并发未验收。
4. 未做全面模糊测试、内存/竞态检测、加密审计或所有失败点故障注入；其他网络解析、
   媒体解码、恶意客户端资源耗尽和异常回调阻塞仍可能有缺陷。8 MiB 限制也需要真实媒体路径验证。
5. Windows TCP 独占绑定可能遇到 TIME_WAIT 而选择新端口，这是预期回退；没有修改系统
   动态端口范围。新解压路径可能不匹配旧防火墙程序规则，本次没有更改规则。
6. 配置/配对存储仍沿用上游行为；个人包不替换 K:\AirPlay2-Win-x64，也不改变 Clash 配置。

参考：[上游 v1.2.5](https://github.com/xenos1337/AirPlayServer/releases/tag/v1.2.5)、
[Windows SO_EXCLUSIVEADDRUSE](https://learn.microsoft.com/en-us/windows/win32/winsock/so-exclusiveaddruse)。
