# Mineworld

## 开始

- 已关闭cpptools提供的IntelliSense功能，因此c_cpp_properties.json已被弃用。
- 使用clangd代码高亮和格式化，需要手动安装。

## 计划

### BUG/优先

- 区块生成/显示应从近到远
- 站着不动，有的区块会长时间不显示
- F5切换视角，区分不同模式的视角，现在视角是在actor的底部，能看到自己，Actor的方向应当和朝向相关
- 入口改成Server/Client，从Client选择连接或者内建Server

### 架构

- 多线程区块生成
- 事件系统
- 输入系统
- 同步优化：输入序列号，其他actor插值缓冲
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
- 加入配置文件
- imgui实时修改参数
- 性能分析工具
- GPU分析工具
