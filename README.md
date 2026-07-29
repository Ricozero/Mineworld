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

- 区块同步
  - 多线程生成，区块状态机，优先任务队列，按数量/时间预算提交，任务取消/去重
  - 分tick生成区块，拆分 connected 与 worldPlayable
  - 增加 InitialChunkPlan
  - 客户端加载界面显示应用进度
  - 客户端应用区块增加每帧时间预算
  - 增加客户端断开连接，被服务器断开或正常关闭

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

- 优化Snapshot再加KeepAlive
- 缓存区块编码结果，然后加入区块压缩，palette + bit packing，RLE，zstd/lz4
- 多通道消息机制，协议QoS，字节令牌桶，区块与实体快照分离

### 渲染

- 多线程
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

### 存储

- leveldb存储区块
- 游程编码和区间树

### 工具

- 加入调试命令行和界面
- 多机器人压力测试
- 游戏内更改设置，客户端设置/世界设置
