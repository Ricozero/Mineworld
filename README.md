# Mineworld

Mineworld 是一个使用 C++20 开发的多人在线体素游戏实验项目

## 已实现功能

- 支持本地游戏、远程客户端和专用服务器
- 基于 16×16×16 区块的体素世界、基础地形生成和多人同步
- 基于 EnTT 的玩家、机器人和物理实体
- 基于 bgfx、GLFW 和 ImGui 的渲染及界面
- 基于 Asio、UDP 和 KCP 的可靠网络通信
- 使用 FlatBuffers 序列化握手、输入、实体和区块数据
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

- 网格构建性能优化
- 网络LZ4

### 架构

- 事件系统
- 输入系统
- Lua，Sol2，协程
- Handle资源管理系统热加载（音乐，音效，材质，贴图集，着色器，脚本）
- actor的唯一id
- 服务器区块
  - 区块共享编码缓存
  - 区块生成批量set接口
  - 永久区块
  - 增量修改
  - 热区块和冷区块，冷区块使用RLE+区间树/线段树或者LZ4
  - leveldb，使用LZ4
  - 固定大小的工作线程池
    - 主线程负责计算需求、更新状态机、投递任务和提交完成结果
    - 工作线程只读取 chunkPos、generationId 和不可变世界生成参数，生成 ChunkData 并写入线程安全完成队列
    - 提交前校验区块状态、requesterCount 和 generationId，过期结果直接丢弃
    - 服务器关闭时停止接收任务，清空未开始任务，等待工作线程退出并丢弃无效结果
- 客户端区块
  - 热缓存和冷缓存
  - 多线程网格构建
  - 客户端视野，影响是否构建网格
  - 区块边界用颜色表示不同加载状态
  - 贪婪网格，影响纹理/平滑光照/AO/透明
  - 新区块平滑显示
  - 远距离区块景深

### 玩法

- 24旋转态
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
- 考虑公网：三次握手，NAT
- 增加服务器错误，当前handleHandshake和onClientHello等地方出错会等待超时

### 渲染

- 线框模式
- 贴图集，材质
- 阴影映射
- 环境光遮蔽
- 方块光照传播算法
- 显示实体名称
- LOD
- 光线追踪

### 工具

- 加入调试命令行和界面
- 多机器人压力测试
- 游戏内更改设置，客户端设置/世界设置
