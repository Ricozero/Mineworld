# Mineworld

Mineworld 是一个使用 C++20 开发的多人在线体素游戏实验项目

## 已实现功能

- 基于 16×16×16 区块的体素世界和基础地形生成
- 基于 EnTT 的玩家、机器人和物理实体
- 支持本地游戏、远程客户端和专用服务器
- 基于 Asio、UDP 和 KCP 的可靠网络通信
- 使用 FlatBuffers 序列化握手、输入、实体和区块数据
- 基于 bgfx、GLFW 和 ImGui 的渲染及界面
- 客户端连接状态、超时处理和返回主菜单
- 初始核心区块加载及 ClientReady 流程
- 集成 Tracy 性能分析

## 构建

项目使用 C++20、CMake、Ninja 和 vcpkg，并已在 MSVC 下测试通过

```console
cmake --build build --config Debug
```

可执行文件和运行配置生成到 `bin/`。

## 运行

启动客户端：

```console
bin/mineworld.exe [client]
```

启动专用服务器：

```console
bin/mineworld.exe server
```

## 开发环境

- 使用 clangd 提供代码分析和格式化，需要手动安装
- 已关闭 VSCode cpptools 的 IntelliSense，`c_cpp_properties.json` 不再使用
- FlatBuffers 生成文件位于 `build/src/generated/`，不要直接修改
- 使用 Tracy 查看客户端帧、服务端 tick 和自定义性能指标

## 计划

### 当前

客户端连接状态机、初始核心区块、ChunkData 数据契约、协议拆分和客户端预算应用已经完成。下一阶段集中实现服务端区块状态机和多线程区块生成管线。

当前区块仍由服务端主线程同步生成，但实体已经通过 EntitySnapshot 同步，区块通过独立的 ChunkUpdate 同步。客户端按 chunkPos 去重，并按每帧数量和耗时预算应用 Upsert 或 Unload；网格重建继续使用独立预算。

#### 第一阶段：建立区块数据契约（已完成）

1. 增加独立的纯数据 ChunkData，包含 chunkPos、固定大小方块数组和 revision
2. 为 Chunk 和 VoxelWorld 增加从 ChunkData 提交区块、从已加载区块导出 ChunkData 的接口
3. 将地形生成逻辑从 ServerWorld 分离为只依赖区块坐标和不可变世界参数的纯生成器
4. 明确 revision 规则：首次生成从 1 开始，方块修改后递增，旧消息不能覆盖更新内容

完成标准：同一套 ChunkData 可以供生成线程、世界提交和网络序列化使用。

#### 第二阶段：拆分区块协议（已完成）

1. Snapshot 改成 EntitySnapshot，只同步完整的可见实体快照
2. 增加独立的 ChunkUpdate 消息，包含 chunkPos、revision、operation 和可选的完整区块数据
3. operation 使用 Upsert 和 Unload：Upsert 负责首次加载或替换已有区块，Unload 负责删除区块
4. 服务端暂时可以继续同步生成，但区块必须通过独立的 ChunkUpdate 发送
5. 客户端增加按 chunkPos 去重的待应用表，只保留最后收到的操作和最新 revision
6. Unload 取消尚未应用的 Upsert；旧 revision 的 Upsert 不能覆盖客户端已经应用的新版本
7. 客户端按每帧区块数量和耗时双预算应用，应用后使自身及相邻区块网格失效

完成标准：实体更新不再受到区块消息大小影响，单帧应用区块数量受到明确限制。

#### 第三阶段：服务端区块状态机

新增独立的 ChunkManager，世界区块状态和各会话的发送状态分开维护。

状态：Unloaded → Queued → Generating → ReadyToCommit → Loaded → UnloadPending → Unloaded。

每个区块记录：

- state
- requesterCount
- priority
- generationId
- revision
- unloadTime

主要规则：

1. 每个会话和机器人分别计算需求集合，通过集合差异增加或移除 requester
2. 多个 requester 请求同一区块时，只创建一个世界任务
3. Queued 且 requesterCount 归零时直接取消任务
4. Generating 无法强制取消，通过 generationId 使完成后的过期结果失效
5. Loaded 且 requesterCount 归零后进入延迟卸载
6. UnloadPending 收到新 requester 时恢复 Loaded
7. Loading 会话的核心区块使用最高优先级，并显式关联对应会话

优先级从高到低：

1. Loading 玩家的核心区块
2. 玩家移动方向前方的新进入区块
3. 玩家附近的其他可见区块
4. 玩家远端可见区块
5. 机器人 3×3×3 范围区块

同级按距离排序，并优先复用已加载区块或已有生成任务。

完成标准：快速移动和反复进出视距时不会重复生成同一坐标，也不会立即卸载再加载。

#### 第四阶段：服务端区块生成管线

1. 增加固定大小的工作线程池
2. 主线程负责计算需求、更新状态机、投递任务和提交完成结果
3. 工作线程只读取 chunkPos、generationId 和不可变世界生成参数
4. 工作线程生成纯 ChunkData，不访问 ServerWorld、VoxelWorld、EnTT、会话或网络对象
5. 完成结果写入线程安全完成队列
6. 主线程按每 tick 数量和耗时双预算提交结果
7. 提交前校验区块状态、requesterCount 和 generationId，过期结果直接丢弃
8. 单个区块不拆成分 tick 生成
9. 服务器关闭时停止接收任务，清空未开始任务，等待工作线程退出并丢弃无效结果

完成标准：大量新区块同时进入视距时，服务端 tick 不再同步执行地形生成。

#### 第五阶段：服务端区块同步调度

每个会话维护：

- desiredChunks
- sentRevisions
- pendingChunkUpdates
- coreChunksRemaining

发送规则：

1. 区块提交后，只向仍然需要该区块的会话加入 operation 为 Upsert 的 ChunkUpdate
2. 区块离开会话视距时加入 operation 为 Unload 的 ChunkUpdate
3. pendingChunkUpdates 按 chunkPos 去重，只保留最后操作和最新 revision
4. Loading 会话的核心区块优先于所有普通区块
5. 普通区块按新进入视距、移动方向、距离排序
6. 每 tick 使用消息数量和字节数双预算发送
7. ClientReady 只表示客户端已经应用全部核心区块，不表示服务端生成任务完成

完成标准：核心区块不会被普通视距区块阻塞，Upsert 和 Unload 可以在同一队列中正确覆盖，区块发送不再依赖 Snapshot 的数量限制。

#### 第六阶段：客户端应用和网格管线收尾

1. 网络层只负责校验、反序列化 ChunkUpdate 和加入待应用队列
2. 世界应用队列和网格重建队列保持分离
3. Loading 阶段优先应用和重建核心区块
4. 核心区块全部应用且网格就绪后发送 ClientReady 并进入 Running
5. Running 阶段按玩家距离排序应用普通区块
6. 卸载区块时删除世界数据、清除自身网格缓存并使相邻区块网格失效
7. 为待应用队列设置容量上限，溢出时优先保留核心区块、最后操作和最新 revision

完成标准：大量区块到达不会造成单帧卡顿，过期区块不会被短暂应用后再卸载。

#### 验证顺序

1. 单玩家出生时，全部 3×3×3 核心区块完成后进入 Running
2. 降低服务端 tick 频率并快速移动，确认不会出现明显长 tick
3. 反复跨越视距边界，确认不会重复生成或应用旧 revision
4. 两名玩家请求重叠区块，确认只生成和存储一份
5. 玩家断开后正确减少 requester，并在延迟结束后卸载区块
6. 生成过程中玩家离开，确认完成结果被安全丢弃
7. 区块持续生成和发送时，实体同步与客户端输入仍能正常更新
8. 为队列长度、生成耗时、提交耗时、发送字节数、客户端应用耗时和网格重建积压增加 Profiler 数据

### 架构

- 事件系统
- 输入系统
- Lua，Sol2，协程
- Handle资源管理系统热加载（音乐，音效，材质，贴图集，着色器，脚本）
- 永久区块
- 区块增量修改

### 玩法

- RayCast选择方块
- 破坏和放置方块
- 方块定义数据化
- 地形生成：Perlin Noise，Simplex Noise，3D Noise，Wave Function Collapse
- 世界生成：温度场/湿度场/大陆性/侵蚀度/山脉强度，河流，建筑
- 寻路算法：NavMesh，Jump Point Search
- 类似红石的门电路
- NPC实体
- 删除spawn配置，改成自动寻找最高点

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
