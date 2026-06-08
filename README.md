# Mineworld

## 开始

- 已关闭cpptools提供的IntelliSense功能，因此c_cpp_properties.json已被弃用。
- 使用clangd代码高亮和格式化，需要手动安装。

## 计划

### BUG/优先

- 区块生成/显示应从近到远
- 站着不动，有的区块会长时间不显示
- 优化性能
- 优化同步，减少卡顿

### 架构

- 多线程区块生成
- 事件系统
- 输入系统
- Lua和Sol2
- Lua协程
- Handle资源管理系统

### 游戏

- RayCast选择方块，DDA算法
- 地形生成：Perlin Noise，Simplex Noise，3D Noise，Wave Function Collapse
- 寻路算法：NavMesh，Jump Point Search

### 网络

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

- 加入调试命令行和界面
- 加入配置文件
- GPU分析工具
