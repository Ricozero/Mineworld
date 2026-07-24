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
  - 多线程生成
  - 多线程渲染
  - 视野4时，机器人会掉下去，先移动卡卡的，然后移动很快
  - 视野2时，简单场景静止状态帧率120，需要极致优化
- 游戏内更改设置

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

- 多通道消息机制，协议QoS
- KeepAlive

### 渲染

- 线框模式
- 贪婪网格
- 贴图集，材质
- 阴影映射
- 环境光遮蔽
- 方块光照传播算法
- 区块更新平滑显示
- 区块渲染视野
- 显示实体名称

### 存储

- leveldb存储区块
- 游程编码和区间树

### 工具

- 加入调试命令行和界面
- 多机器人压力测试
