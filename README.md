# Mineworld

## 开始

已关闭cpptools提供的IntelliSense功能，因此c_cpp_properties.json已被弃用。
使用clangd代码高亮和格式化，需要手动安装。

## 计划

--integrated-server
--dedicated-server

阶段一：单机引擎核心 (基础建设与渲染)
目标：构建一个“可漫游的静态体素世界”，跑通图形学和ECS基础。
时长：1.5 - 2 个月
工程基建
构建系统：CMake 搭建，集成 bgfx (渲染), glfw (窗口), glm (数学), EnTT (ECS), ImGui (调试)。
ECS 雏形：实现 EnTT Registry，定义基础 Component (Position, Velocity, Rotation)，实现简单的 System (根据输入更新速度，根据速度更新位置)。
体素世界 (World & Data)
Chunk 数据结构：定义 16x16x16 (或 32x32x32) 的 Chunk 类，使用一维数组存储 Block ID。
基础生成：集成 FastNoiseLite 或手写 Perlin Noise，生成简单的地形高度图。
核心渲染 (Rendering)
Hello Voxel：根据 Chunk 数据生成 Mesh（顶点/索引 buffer），编写基础 Shader（顶点/片段）。
网格优化：
V1: 面剔除 (Face Culling) —— 检查相邻方块，只渲染暴露的面。
V2: 贪婪网格 (Greedy Meshing) —— 将相邻的相同纹理面合并，大幅减少 draw calls (进阶，可稍后做)。
基础交互与调试
漫游摄像机：实现 FPS 风格的 Camera。
ImGui 工具：实时修改光照参数、切换方块类型、查看 FPS。

阶段二：网络通信框架 (联机基础)
目标：从单机变为“多人聊天室”，跑通 CS 架构和基础同步。
时长：1 个月
服务器基建 (Skynet & Protocol)
Skynet 环境：搭建 Skynet，理解 Actor 模型，编写基础 Service。
协议栈：集成 KCP (低延迟传输) 和 Protobuf (序列化)。
代码共用：建立 Common 目录，将 ECS 的 Component 定义和 Protobuf 协议文件设为前后端共用。
连接与登录
Echo Server：客户端发包 -> 服务端收包并原样返回 (Ping-Pong)。
登录流程：客户端发送登录包 -> 服务器创建 Agent -> 广播“玩家进入”消息。
基础状态同步 (插值)
简单广播：客户端定时(e.g., 20Hz)发送位置 -> 服务器校验 -> 广播给感兴趣区域(AOI)的其他玩家（初期可全服广播）。
影子渲染：客户端收到其他玩家位置包，不直接设置坐标，而是使用线性插值 (Lerp) 平滑渲染其他玩家的移动。

阶段三：物理、存储与高级同步 (核心玩法)
目标：实现“不仅能看，还能动、能存、能改”，攻克最难的技术壁垒。
时长：2 - 2.5 个月
物理与碰撞 (Physics)
AABB 碰撞：手写简单的 AABB (Axis-Aligned Bounding Box) 碰撞检测。
重点：实现 RayCast (用于选中方块) 和 Sweep AABB (用于玩家移动碰撞)。
物理系统：在 ECS 中添加 Physics System，处理重力、跳跃和阻挡。
高级移动同步 (Prediction & Reconciliation)
这是最难点，需要结合物理系统：
客户端预测 (Prediction)：按下 W 键 -> 本地立即移动(无需等待服务器) -> 保存输入历史。
服务器校正 (Reconciliation)：服务器模拟物理 -> 下发权威位置 -> 客户端对比，若误差过大则回滚并重放输入。
世界交互与动态更新
破坏/放置：鼠标点击 -> RayCast 选中 -> 发送操作包 -> 服务器验证 -> 广播 Chunk Update -> 客户端重建 Mesh。
LevelDB 存储：
Key设计：Chunk_X_Y_Z -> Value: 压缩后的 Chunk 数据。
服务器端实现 Chunk 的按需加载（LRU Cache）和卸载保存。

阶段四：脚本系统与游戏性扩展 (扩展与优化)
目标：让硬编码的引擎变成可扩展的游戏。
时长：1 - 1.5 个月
Lua 脚本集成
Binding：使用 Sol2 将 C++ 的 ECS 接口（如 registry.create(), transform.pos）绑定到 Lua。
逻辑脚本化：将方块属性（如：是否发光、是否透明）和简单玩法（如：每隔 10 秒生成怪）移入 Lua 配置。
画面与性能优化
视觉增强：实现简单的阴影映射 (Shadow Mapping)、环境光遮蔽 (SSAO) 或 简单的方块光照传播算法 (BFS Light Propagation)。
性能分析：接入 Microprofile，定位 CPU/GPU 瓶颈（通常在 Chunk Mesh 生成或大量实体同步上）。
多线程/LOD：将 Chunk Mesh 的生成放入线程池；远处的 Chunk 使用低精度模型。
发布与部署
实现 Docker 部署 Skynet 服务器。
局域网/公网联机测试。

## 发散

一、 核心算法与数据结构（硬核深度）
对于体素游戏，**“多”**是最大的敌人。如何在有限内存和带宽下处理海量数据是核心难点。
1. 区块数据压缩：Palette-based Storage (调色板压缩)
概念：不要直接存 BlockID[4096]。如果一个 Chunk 里全是“空气”，你只需要存一个 ID。如果只有泥土和石头，你可以建立一个局部的 vector<BlockID> palette，然后用少量的 bit（比如 4bit 或 5bit）作为索引来存储具体方块。
发散点：这涉及到位操作 (Bit manipulation) 和 RLE (Run-Length Encoding) 压缩算法。
面试价值：展示你对内存布局极致优化的能力，这在高性能服务器开发中极受重视。
2. 射线检测：DDA 算法 (Digital Differential Analyzer)
概念：在 CS:GO 里检测击中人是用物理引擎的 Raycast，但在 MC 里检测玩家视线看的是哪个方块，绝对不能遍历路径上的每个点。
发散点：学习 3D DDA 算法。这是一种基于网格的高效遍历算法，复杂度仅与穿过的方块数有关。
面试价值：图形学和几何算法的基础应用，比直接调物理引擎接口更有含金量。
3. 地形生成：不仅是 Perlin Noise
概念：Perlin Noise 比较老旧且有晶格感。
发散点：
Simplex Noise：性能更好，维数更高时开销更低。
3D Noise (3D 噪声)：可以生成“浮空岛”和“洞穴”（Perlin 通常是生成高度图，没法做重叠地形）。
WFC (Wave Function Collapse, 波函数坍缩)：如果你想尝试生成复杂的建筑结构（如地牢），这是目前最火的算法。
4. 寻路系统：Recast & Detour
概念：体素世界的寻路不能只用 A* 跑格子，否则怪物走得像走方阵。
发散点：虽然是体素，但可以抽象出 NavMesh (导航网格)。了解如何在动态破坏地形后，局部更新 NavMesh。或者研究 JPS (Jump Point Search) 这种专门针对网格地图优化的寻路算法。

二、 架构与系统设计（扩展性）
5. 所有的逻辑都是“事件” (Event-Driven Architecture)
发散点：不要在代码里直接调用 Player::TakeDamage()。
尝试建立一套全局事件总线 (Event Bus)。
EventBus.Publish(DamageEvent{target, amount})
这样做的好处是：成就系统监听伤害事件（造成1万点伤害跳成就）、UI系统监听（飘血字）、音效系统监听（播放受击音效），它们完全解耦。
结合 EnTT：EnTT 自带 Signal/Dispatcher 功能，非常好用。
6. Lua 的高级用法：协程 (Coroutines) 管理任务
发散点：服务器逻辑中充斥着“等待3秒后刷怪”、“玩家采集动作持续2秒”。不要用 Update() 里的 timer += dt 这种原始方式。
方案：封装 Lua 协程。
code
Lua
-- 脚本写法
function OnPlayerInteract(player)
    PlayAnimation(player, "mining")
    Yield(Seconds(2.0)) -- 挂起协程，C++层接管计时
    AddItem(player, "stone", 1)
end
面试价值：展示你对异步逻辑流控制的理解，这是现代游戏脚本系统的标配。
7. 资源管理：Handle 系统
概念：在 C++ 游戏引擎中，尽量不要直接传递 Texture* 或 Entity* 指针（容易悬空、难以序列化）。
发散点：实现一套 Handle 系统。
用一个 uint32_t 或 uint64_t 代表资源或实体。
Handle 包含：Index (数组下标) + Generation (代数，用于检测是否已销毁重建) + Type。
这解决了 C++ 指针最大的痛点，也是 ECS 里的 Entity ID 的核心逻辑。

三、 工程化与网络（专业度）
8. 网络协议的“优先级” (QoS)
发散点：KCP 是底层的传输。在应用层，你需要区分消息的重要性。
高优先级：玩家输入、方块破坏（必须必达，顺序不能乱）。
低优先级：远处怪物的移动、聊天信息、天空颜色的变化（丢了就丢了，或者晚点到也没事）。
实践：设计一个多通道（Channel）的消息队列，确保关键数据不被垃圾数据阻塞。
9. 自动化构建与 CI/CD
发散点：既然从零开始，不如把环境弄得专业点。
Vcpkg / Conan：用来管理 bgfx, protobuf 等第三方库，别手动拷贝 .lib 文件。
GitHub Actions：提交代码后自动编译 Windows 和 Linux 版本。
面试价值：这表明你有带团队或在大厂规范下工作的潜质，而不只是个写代码的“独狼”。
10. 崩溃捕获与分析 (Crash Reporting)
发散点：C++ 最怕 Crash。服务器如果在半夜挂了怎么办？
Google Breakpad 或 Crashpad。
集成这些库，当游戏崩溃时生成 .dmp (minidump) 文件。你可以事后用 VS 打开这个文件，直接定位到崩溃的那一行代码。
面试价值：这是服务器开发极其核心的维稳能力。

四、 图形学的“性价比”优化（视觉效果）
虽然你不是图形程序员，但加一两个“低成本高产出”的特效会让Demo档次起飞。
11. Ambient Occlusion (AO) 环境光遮蔽
发散点：不需要复杂的屏幕空间 AO (SSAO)。
Voxel AO：在构建 Mesh 时，计算四个顶点的遮挡情况（看周围有没有方块），直接把亮度值写进顶点的 Color 属性里。
效果：墙角会变暗，方块层次感瞬间增强，且运行时0性能消耗。
12. 视锥体剔除 (Frustum Culling)
发散点：世界很大，不要把背后的、很远的 Chunk 发给 GPU。
简单实现：计算 Chunk 的包围盒 (AABB)，判断是否在摄像机的视锥体内。如果不在，直接不提交 bgfx 的 draw call。
