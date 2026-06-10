# Mineworld

## 开始

- 已关闭cpptools提供的IntelliSense功能，因此c_cpp_properties.json已被弃用
- 使用clangd代码高亮和格式化，需要手动安装

## 计划

### 当前

- 区块同步：站着不动，有的区块会长时间不显示；尽快同步近处区块，避免客户端以为是空的掉下去
- 渲染优化
- 线框模式
- 设置：配置窗口大小、图形API、面剔除和视锥体剔除开关；游戏内更改设置
- 显示硬件名称

### 架构

- 多线程区块生成
- 事件系统
- 输入系统
- Lua和Sol2
- Lua协程
- 热加载：Handle资源管理系统（音乐音效材质贴图）

### 玩法

- RayCast选择方块，DDA算法
- 地形生成：Perlin Noise，Simplex Noise，3D Noise，Wave Function Collapse
- 寻路算法：NavMesh，Jump Point Search

### 网络

- 多通道消息机制，协议QoS
- KeepAlive

### 渲染

- 贪婪网格：将相邻的相同纹理面合并
- 阴影映射
- 环境光遮蔽
- 方块光照传播算法

### 存储

- leveldb存储区块
- Palette-based Storage (调色板压缩)

### 工具

- 加入调试命令行和界面
