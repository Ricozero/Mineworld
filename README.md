# Mineworld

## 功能

- 基于立方体区块和EnTT实体的体素引擎
- 基于Asio和KCP的网络编程，支持多人在线
- 内置性能分析工具，支持Tracy

## 开发

- 已关闭cpptools提供的IntelliSense功能，因此c_cpp_properties.json已被弃用
- 使用clangd代码高亮和格式化，需要手动安装

## 计划

### 当前

#### 目标

- 区块生成、发送和应用不能长时间阻塞服务端 tick 或客户端帧
- 客户端应用完核心区块后立即进入游戏，其余视距后台加载
- 连接失败、超时或关闭后必须返回主菜单

#### 连接状态

- Disconnected：主菜单
- Connecting：等待 UDP/KCP 握手
- Awaiting：已连接，等待 ServerHello
- Loading：接收并应用初始核心区块
- Running：允许输入和物理，后台加载剩余视距
- Disconnecting：正常退出并清理资源
- Failed：记录失败原因并返回主菜单

进入 Running 前必须已创建本地玩家，并应用全部核心区块。服务端在此之前忽略该玩家的移动输入。

#### 初始核心区块

ServerHello 包含出生位置和经过世界边界裁剪的核心区块列表，默认为玩家周围 3×3×3。

服务端一次性将核心区块加入最高优先级任务，但仍按多个 ChunkData 和每 tick 预算发送，避免单个超大消息。客户端记录尚未应用的核心区块；全部应用完成后发送 ClientReady 并进入 Running。

#### 服务端区块状态机

世界区块状态与各会话的发送状态分开维护。
状态：Unloaded → Queued → Generating → ReadyToCommit → Loaded → UnloadPending。
每个区块记录 requesterCount、priority、generationId 和 revision。任务按 chunkPos 去重；Queued 任务可取消；过期结果直接丢弃；无人请求时延迟卸载。

优先级从高到低：

1. Loading 玩家的核心区块
2. 玩家移动方向前方的新进入区块
3. 玩家附近的其他可见区块
4. 玩家远端可见区块
5. 机器人 3×3×3 范围区块

同级按距离排序，并优先复用已有区块或任务。

#### 服务端区块生成管线

1. 主线程根据区块状态和优先级将任务加入生成队列
2. 固定工作线程池读取任务并生成纯 ChunkData，不访问 ServerWorld、VoxelWorld、EnTT 或网络对象
3. 生成结果写入完成队列，已取消或 generationId 过期的结果直接丢弃
4. 主线程按时间和数量预算将有效结果提交到 VoxelWorld，并将区块标记为 Loaded

单个区块不拆成分 tick 生成。服务器关闭时停止提交新任务，清空等待队列并等待工作线程退出。

#### 区块同步与客户端应用

Snapshot 改成 EntityData，只同步实体，区块使用 ChunkData 和 ChunkUnload 独立同步：

1. 区块提交完成后，服务端为需要该区块的会话加入 ChunkData；离开视距时加入 ChunkUnload
2. 会话发送队列按 chunkPos 去重，只保留最新操作和 revision，并按每 tick 数量预算发送
3. 客户端网络层完成校验和反序列化，将消息放入待应用队列
4. 待应用队列按 chunkPos 去重，ChunkUnload 会取消对应的待应用 ChunkData
5. 客户端按每帧预算应用区块，再将受影响区块加入独立的网格重建队列

Loading 阶段优先处理核心区块，继续泵网络和渲染，但不运行玩家输入与物理。界面只显示正在加载世界、取消操作和失败原因，不显示具体进度。进入 Running 后，客户端继续服从服务端的 ChunkData 和 ChunkUnload。

#### 实施顺序

1. 连接状态机：实现失败、超时和返回主菜单
2. 初始核心区块：ServerHello 携带核心区块列表，完成后发送 ClientReady
3. 客户端预算：区块接收入队，按帧预算应用并分离网格重建
4. 服务端状态机：加入任务去重、优先级、取消、卸载延迟和主线程提交预算
5. 多线程生成：把纯 ChunkData 生成移入固定工作线程池
6. 协议拆分：Snapshot 拆分成 EntityData 和 ChunkData
7. Profiler：按需记录数据

### 架构

- 事件系统
- 输入系统
- Lua，Sol2，协程
- Handle资源管理系统热加载（音乐，音效，材质，贴图集，着色器，脚本）

### 玩法

- RayCast选择方块
- 破坏和放置方块
- 方块定义数据化
- 地形生成：Perlin Noise，Simplex Noise，3D Noise，Wave Function Collapse
- 寻路算法：NavMesh，Jump Point Search
- 类似红石的门电路
- NPC实体

### 网络

- 控制、实体、必需区块和普通区块使用独立队列，按顺序发送，存在队头阻塞后再考虑 KCP 多通道
- 字节令牌桶限制区块进入 KCP 的速率，超过 KCP 高水位时暂停普通区块
- Snapshot 和输入都可能停止发送时，增加 KeepAlive
- 区块压缩，palette + bit packing，RLE，zstd/lz4
- 区块共享编码缓存，由容量上限和 LRU 策略回收
- 考虑公网：三次握手，NAT
- 增加服务器错误，当前handleHandshake和onClientHello等地方出错会等待超时

### 渲染

- 线框模式
- 贪婪网格
- 贴图集，材质
- 阴影映射
- 环境光遮蔽
- 方块光照传播算法
- 区块更新平滑显示
- 远距离区块景深
- 区块视野
- 显示实体名称
- 热缓存+冷缓存，对于冷缓存，需要自身版本和相邻区块签名一致
- 多线程

### 存储

- leveldb存储区块
- 游程编码和区间树

### 工具

- 加入调试命令行和界面
- 多机器人压力测试
- 游戏内更改设置，客户端设置/世界设置
