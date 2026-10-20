import CoreVideo
import Foundation
import Metal

/// 用 GPU 比较两帧 BGRA，找出内容变化的 64×64 图块。逐像素精确比较，耗时远低于 CPU 逐行比对。
/// Metal 不可用时返回 nil，由调用方退回 CPU 比对。
final class TileDiffer {
    private let device: MTLDevice
    private let queue: MTLCommandQueue
    private let pipeline: MTLComputePipelineState
    private var textureCache: CVMetalTextureCache?

    private static let source = """
    #include <metal_stdlib>
    using namespace metal;
    kernel void tileDiff(texture2d<float, access::read> a [[texture(0)]],
                         texture2d<float, access::read> b [[texture(1)]],
                         device atomic_uint* flags [[buffer(0)]],
                         constant uint& tilesX [[buffer(1)]],
                         constant uint& tileSize [[buffer(2)]],
                         uint2 gid [[thread_position_in_grid]]) {
        if (gid.x >= a.get_width() || gid.y >= a.get_height()) return;
        if (any(a.read(gid) != b.read(gid))) {
            atomic_store_explicit(&flags[(gid.y / tileSize) * tilesX + gid.x / tileSize], 1u, memory_order_relaxed);
        }
    }
    """

    init?() {
        guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else { return nil }
        do {
            let library = try device.makeLibrary(source: Self.source, options: nil)
            guard let function = library.makeFunction(name: "tileDiff") else { return nil }
            pipeline = try device.makeComputePipelineState(function: function)
        } catch {
            Log.info("GPU 图块比对初始化失败：\(error)")
            return nil
        }
        self.device = device
        self.queue = queue
        CVMetalTextureCacheCreate(kCFAllocatorDefault, nil, device, nil, &textureCache)
        if textureCache == nil { return nil }
    }

    /// 异步比较：提交后立即返回 true，GPU 完成后在任意线程回调每个图块是否变化（1/0，失败为 nil）。
    /// 返回 false 表示无法提交（调用方应改用 CPU 比对）。
    func diff(_ previous: CVPixelBuffer, _ current: CVPixelBuffer, tileSize: Int, tilesX: Int, tilesY: Int,
              completion: @escaping ([UInt8]?) -> Void) -> Bool {
        guard let a = texture(previous), let b = texture(current) else { return false }
        let count = tilesX * tilesY
        guard let flags = device.makeBuffer(length: count * 4, options: .storageModeShared),
              let commands = queue.makeCommandBuffer(), let encoder = commands.makeComputeCommandEncoder() else {
            return false
        }
        memset(flags.contents(), 0, count * 4)
        var tilesXValue = UInt32(tilesX)
        var tileSizeValue = UInt32(tileSize)
        encoder.setComputePipelineState(pipeline)
        encoder.setTexture(CVMetalTextureGetTexture(a), index: 0)
        encoder.setTexture(CVMetalTextureGetTexture(b), index: 1)
        encoder.setBuffer(flags, offset: 0, index: 0)
        encoder.setBytes(&tilesXValue, length: 4, index: 1)
        encoder.setBytes(&tileSizeValue, length: 4, index: 2)
        let width = CVPixelBufferGetWidth(current)
        let height = CVPixelBufferGetHeight(current)
        let group = MTLSize(width: 16, height: 16, depth: 1)
        encoder.dispatchThreads(MTLSize(width: width, height: height, depth: 1), threadsPerThreadgroup: group)
        encoder.endEncoding()
        commands.addCompletedHandler { buffer in
            // 纹理与像素缓冲必须存活到 GPU 完成。
            withExtendedLifetime((a, b, previous, current)) {}
            guard buffer.status == .completed else {
                completion(nil)
                return
            }
            let words = flags.contents().bindMemory(to: UInt32.self, capacity: count)
            var result = [UInt8](repeating: 0, count: count)
            for i in 0..<count where words[i] != 0 {
                result[i] = 1
            }
            completion(result)
        }
        commands.commit()
        return true
    }

    private func texture(_ buffer: CVPixelBuffer) -> CVMetalTexture? {
        guard let textureCache else { return nil }
        var texture: CVMetalTexture?
        let status = CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault, textureCache, buffer, nil, .bgra8Unorm,
            CVPixelBufferGetWidth(buffer), CVPixelBufferGetHeight(buffer), 0, &texture)
        return status == kCVReturnSuccess ? texture : nil
    }
}
