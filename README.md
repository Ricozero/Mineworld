# Mineworld

## 开始

- 已关闭cpptools提供的IntelliSense功能，因此c_cpp_properties.json已被弃用
- 使用clangd代码高亮和格式化，需要手动安装

## 计划

### 当前

- 区块显示不正常
- 渲染优化
- 线框模式
- 游戏内更改设置
- 日志写入文件

### 架构

- 多线程区块生成
- 事件系统
- 输入系统
- Lua，Sol2，协程
- Handle资源管理系统热加载（音乐，音效，材质，贴图集，着色器，脚本）

### 玩法

- 玩家名显示
- RayCast选择方块
- 破坏和防止方块
- 方块定义数据化
- 地形生成：Perlin Noise，Simplex Noise，3D Noise，Wave Function Collapse
- 寻路算法：NavMesh，Jump Point Search

### 网络

- 多通道消息机制，协议QoS
- KeepAlive

### 渲染

- 贪婪网格
- 贴图集，材质
- 阴影映射
- 环境光遮蔽
- 方块光照传播算法
- 区块更新平滑显示

### 存储

- leveldb存储区块
- 游程编码和区间树

### 工具

- 加入调试命令行和界面
- 多机器人压力测试
