# Mineworld

## 开始

- 已关闭cpptools提供的IntelliSense功能，因此c_cpp_properties.json已被弃用。
- 使用clangd代码高亮和格式化，需要手动安装。

## 计划

### BUG

- 区块同步存在明显延迟
- 即使不动，区块数量还是存在波动
- 看不到区块
- 修改isBlockInBounds范围
- 不使用spectatorName，而是用专门的CameraComponent

### 架构

- 多线程区块生成
- 事件系统
- Lua和Sol2
- Lua协程
- Handle资源管理系统

### 游戏

- 使用控制器控制actor
- Sweep AABB碰撞
- RayCast选择方块，DDA算法
- 地形生成：Perlin Noise，Simplex Noise，3D Noise，Wave Function Collapse
- 寻路算法：NavMesh，Jump Point Search

### 网络

- 定时发送snapshot，线性插值平滑渲染
- 多通道消息机制，协议QoS

### 渲染

- 面剔除
- 视锥体剔除
- 贪婪网格：将相邻的相同纹理面合并
- 阴影映射
- 环境光遮蔽
- 方块光照传播算法

### 存储

- leveldb存储区块
- Palette-based Storage (调色板压缩)

### 工具

- 加入调试命令行
- 加入输入参数或配置文件
- imgui实时修改参数
- 性能分析工具
- GPU分析工具
