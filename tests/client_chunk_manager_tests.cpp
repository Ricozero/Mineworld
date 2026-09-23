#include <bgfx/bgfx.h>
#include <gtest/gtest.h>

#include "client_chunk_manager.h"
#include "test_support.h"
#include "voxel_world.h"

namespace {

using namespace test_support;

TEST(ClientChunkManagerTest, HeadlessKeepsTerrainRevisionsWithoutMeshes) {
    VoxelWorld world;
    ClientChunkManager manager(world, false);
    manager.setCoreChunks({{0, 0, 0}, {1, 0, 0}});
    EXPECT_FALSE(manager.areCoreChunksReady());
    EXPECT_TRUE(manager.upsert({0, 0, 0}, 2, ChunkData{BlockType::Stone}));
    EXPECT_FALSE(manager.areCoreChunksReady());
    EXPECT_TRUE(manager.upsert({1, 0, 0}, 1, ChunkData{}));
    EXPECT_TRUE(manager.areCoreChunksReady());
    EXPECT_FALSE(manager.upsert({0, 0, 0}, 1, ChunkData{}));
    EXPECT_EQ(world.getBlock({0, 0, 0}).type, BlockType::Stone);
    EXPECT_FALSE(manager.unload({0, 0, 0}, 1));
    EXPECT_NE(world.findChunk({0, 0, 0}), nullptr);
    EXPECT_TRUE(manager.unload({0, 0, 0}, 2));
    EXPECT_EQ(world.findChunk({0, 0, 0}), nullptr);
    EXPECT_TRUE(manager.upsert({0, 0, 0}, 3, ChunkData{BlockType::Sand}));
    EXPECT_EQ(world.getBlock({0, 0, 0}).type, BlockType::Sand);
    EXPECT_FALSE(manager.takeNextMeshTask());
    EXPECT_EQ(manager.dirtyMeshCount(), 0u);
    EXPECT_EQ(manager.meshCount(), 0u);
    EXPECT_EQ(manager.meshBytesReserved(), 0u);
    EXPECT_TRUE(manager.renderData().chunks.empty());
    EXPECT_EQ(manager.quadIndexBuffer(), UINT16_MAX);
}

TEST(ClientChunkManagerTest, RenderedCoreChunksStillWaitForMeshes) {
    ensureLogger();
    bgfx::Init init;
    init.type = bgfx::RendererType::Noop;
    init.resolution.width = 1;
    init.resolution.height = 1;
    ASSERT_TRUE(bgfx::init(init));
    struct ShutdownRenderer {
        ~ShutdownRenderer() { bgfx::shutdown(); }
    } shutdownRenderer;
    VoxelWorld world;
    ClientChunkManager manager(world);
    manager.setCoreChunks({{0, 0, 0}});
    ASSERT_TRUE(manager.upsert({0, 0, 0}, 1, ChunkData{BlockType::Stone}));
    EXPECT_FALSE(manager.areCoreChunksReady());
    std::optional<ClientChunkManager::MeshTask> task;
    ASSERT_NO_FATAL_FAILURE(pumpUntil([&] { task = manager.takeNextMeshTask(); }, [&] { return task.has_value(); }));
    ChunkMesh mesh;
    buildChunkMesh(world, task->chunkPos, mesh);
    ASSERT_FALSE(mesh.vertices.empty());
    EXPECT_EQ(manager.completeMeshTask(*task, mesh), ClientChunkManager::MeshTaskResult::Accepted);
    EXPECT_TRUE(manager.areCoreChunksReady());
}

}  // namespace
