import Accelerate
import CoreMedia
import Foundation
import VideoToolbox

enum EncoderError: Error {
    case create(OSStatus)
}

/// VideoToolbox H.264 低延迟硬件编码：实时模式、无 B 帧、按需关键帧，输出 Annex-B。
final class VideoEncoder {
    let width: Int
    let height: Int
    private let session: VTCompressionSession
    private let lock = NSLock()
    private var keyframeRequested = true
    typealias Output = (_ annexB: [UInt8], _ keyframe: Bool, _ captureMicros: UInt64, _ refine: Bool) -> Void
    private let output: Output

    init(width: Int, height: Int, fps: Int, bitrate: Int, maxQP: Int, output: @escaping Output) throws {
        self.width = width
        self.height = height
        self.output = output

        // BGRA → NV12 全范围（GPU），显式指定色彩参数，保证与无损层的 RGB 只差压缩误差、没有系统偏差。
        var transfer: VTPixelTransferSession?
        guard VTPixelTransferSessionCreate(allocator: kCFAllocatorDefault, pixelTransferSessionOut: &transfer) == noErr,
              let transfer else { throw EncoderError.create(-1) }
        converter = transfer
        VTSessionSetProperty(transfer, key: kVTPixelTransferPropertyKey_DestinationYCbCrMatrix,
                             value: kCVImageBufferYCbCrMatrix_ITU_R_709_2)
        VTSessionSetProperty(transfer, key: kVTPixelTransferPropertyKey_DestinationColorPrimaries,
                             value: kCVImageBufferColorPrimaries_ITU_R_709_2)
        VTSessionSetProperty(transfer, key: kVTPixelTransferPropertyKey_DestinationTransferFunction,
                             value: kCVImageBufferTransferFunction_sRGB)
        let poolAttributes = [kCVPixelBufferPoolMinimumBufferCountKey as String: 4] as CFDictionary
        let bufferAttributes = [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
            kCVPixelBufferWidthKey as String: width,
            kCVPixelBufferHeightKey as String: height,
            kCVPixelBufferIOSurfacePropertiesKey as String: [String: Any](),
        ] as CFDictionary
        var createdPool: CVPixelBufferPool?
        guard CVPixelBufferPoolCreate(kCFAllocatorDefault, poolAttributes, bufferAttributes, &createdPool) == kCVReturnSuccess,
              let createdPool else { throw EncoderError.create(-2) }
        pool = createdPool

        let spec = [kVTVideoEncoderSpecification_EnableLowLatencyRateControl as String: true] as CFDictionary
        var created: VTCompressionSession?
        let status = VTCompressionSessionCreate(
            allocator: kCFAllocatorDefault, width: Int32(width), height: Int32(height),
            codecType: kCMVideoCodecType_H264, encoderSpecification: spec, imageBufferAttributes: nil,
            compressedDataAllocator: nil, outputCallback: nil, refcon: nil, compressionSessionOut: &created)
        guard status == noErr, let created else {
            VTPixelTransferSessionInvalidate(transfer)
            throw EncoderError.create(status)
        }
        session = created

        set(kVTCompressionPropertyKey_RealTime, kCFBooleanTrue)
        set(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse)
        set(kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_ConstrainedHigh_AutoLevel)
        set(kVTCompressionPropertyKey_AverageBitRate, bitrate as CFNumber)
        set(kVTCompressionPropertyKey_ExpectedFrameRate, fps as CFNumber)
        set(kVTCompressionPropertyKey_MaxKeyFrameInterval, (fps * 300) as CFNumber)
        // 限制最大量化参数：运动画面（滚动、拖动）不会被压得太糊，代价是这些帧的码率更高。
        set(kVTCompressionPropertyKey_MaxAllowedFrameQP, maxQP as CFNumber)
        // 输入由下面的转换器生成：BT.709 矩阵、全范围、sRGB 传输特性（不做伽马转换），客户端按同样参数还原。
        set(kVTCompressionPropertyKey_ColorPrimaries, kCVImageBufferColorPrimaries_ITU_R_709_2)
        set(kVTCompressionPropertyKey_TransferFunction, kCVImageBufferTransferFunction_sRGB)
        set(kVTCompressionPropertyKey_YCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2)
        VTCompressionSessionPrepareToEncodeFrames(session)
        currentBitrate = bitrate
        Log.info("编码器就绪 \(width)×\(height) @ \(fps) fps H.264 低延迟 \(bitrate / 1_000_000) Mbps，最大 QP \(maxQP)，全范围 BT.709")
    }

    private let converter: VTPixelTransferSession
    private let pool: CVPixelBufferPool
    private var currentBitrate = 0

    /// 运行中调整码率与预期帧率（自适应码率）。
    func setBitrate(_ bitrate: Int, fps: Int) {
        lock.lock()
        let changed = bitrate != currentBitrate
        currentBitrate = bitrate
        lock.unlock()
        guard changed else { return }
        set(kVTCompressionPropertyKey_AverageBitRate, bitrate as CFNumber)
        set(kVTCompressionPropertyKey_ExpectedFrameRate, fps as CFNumber)
    }

    deinit {
        VTCompressionSessionInvalidate(session)
        VTPixelTransferSessionInvalidate(converter)
    }

    /// BGRA → NV12 全范围 BT.709。优先用 CPU（vImage/NEON，按行分带多核并行）：
    /// GPU 忙于渲染高分辨率桌面时，GPU 转换要排队（实测 1080p 约 8 ms），CPU 只需 1–2 ms 且不受影响。
    private lazy var cpuConversion: vImage_ARGBToYpCbCr? = {
        var info = vImage_ARGBToYpCbCr()
        var range = vImage_YpCbCrPixelRange(Yp_bias: 0, CbCr_bias: 128, YpRangeMax: 255, CbCrRangeMax: 255,
                                            YpMax: 255, YpMin: 0, CbCrMax: 255, CbCrMin: 0)
        let error = vImageConvert_ARGBToYpCbCr_GenerateConversion(
            kvImage_ARGBToYpCbCrMatrix_ITU_R_709_2, &range, &info, kvImageARGB8888, kvImage420Yp8_CbCr8,
            vImage_Flags(kvImageNoFlags))
        return error == kvImageNoError ? info : nil
    }()

    private var loggedFallback = false

    private func cpuConvert(_ source: CVPixelBuffer, into destination: CVPixelBuffer) -> Bool {
        guard var info = cpuConversion, CVPixelBufferGetPixelFormatType(source) == kCVPixelFormatType_32BGRA else { return false }
        let width = CVPixelBufferGetWidth(source)
        let height = CVPixelBufferGetHeight(source)
        guard width % 2 == 0, height % 2 == 0, CVPixelBufferGetWidth(destination) == width,
              CVPixelBufferGetHeight(destination) == height else { return false }
        CVPixelBufferLockBaseAddress(source, .readOnly)
        CVPixelBufferLockBaseAddress(destination, [])
        defer {
            CVPixelBufferUnlockBaseAddress(destination, [])
            CVPixelBufferUnlockBaseAddress(source, .readOnly)
        }
        guard let src = CVPixelBufferGetBaseAddress(source),
              let lumaBase = CVPixelBufferGetBaseAddressOfPlane(destination, 0),
              let chromaBase = CVPixelBufferGetBaseAddressOfPlane(destination, 1) else { return false }
        let srcStride = CVPixelBufferGetBytesPerRow(source)
        let lumaStride = CVPixelBufferGetBytesPerRowOfPlane(destination, 0)
        let chromaStride = CVPixelBufferGetBytesPerRowOfPlane(destination, 1)

        let bands = 8
        let bandRows = max(2, (height / bands + 1) & ~1)
        var failed = false
        let failLock = NSLock()
        DispatchQueue.concurrentPerform(iterations: bands) { band in
            let y0 = band * bandRows
            guard y0 < height else { return }
            let rows = min(bandRows, height - y0)
            var srcBuffer = vImage_Buffer(data: src + y0 * srcStride, height: vImagePixelCount(rows),
                                          width: vImagePixelCount(width), rowBytes: srcStride)
            var luma = vImage_Buffer(data: lumaBase + y0 * lumaStride, height: vImagePixelCount(rows),
                                     width: vImagePixelCount(width), rowBytes: lumaStride)
            var chroma = vImage_Buffer(data: chromaBase + (y0 / 2) * chromaStride, height: vImagePixelCount(rows / 2),
                                       width: vImagePixelCount(width / 2), rowBytes: chromaStride)
            let permute: [UInt8] = [3, 2, 1, 0]  // BGRA → ARGB
            let error = vImageConvert_ARGB8888To420Yp8_CbCr8(&srcBuffer, &luma, &chroma, &info, permute,
                                                             vImage_Flags(kvImageDoNotTile))
            if error != kvImageNoError {
                failLock.lock()
                failed = true
                failLock.unlock()
            }
        }
        return !failed
    }

    private func convert(_ source: CVPixelBuffer) -> CVPixelBuffer? {
        var destination: CVPixelBuffer?
        guard CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &destination) == kCVReturnSuccess,
              let destination else { return nil }
        CVBufferSetAttachment(destination, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2, .shouldPropagate)
        CVBufferSetAttachment(destination, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2, .shouldPropagate)
        CVBufferSetAttachment(destination, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_sRGB, .shouldPropagate)
        if cpuConvert(source, into: destination) {
            return destination
        }
        if !loggedFallback {
            loggedFallback = true
            Log.info("CPU 色彩转换不可用（\(CVPixelBufferGetWidth(source))×\(CVPixelBufferGetHeight(source))），改用 GPU 转换，可能较慢")
        }
        let status = VTPixelTransferSessionTransferImage(converter, from: source, to: destination)
        if status != noErr {
            Log.info("色彩转换失败 \(status)")
            return nil
        }
        return destination
    }

    func requestKeyframe() {
        lock.lock()
        keyframeRequested = true
        lock.unlock()
    }

    // 分段计时（用于性能日志）：排队等待、色彩转换、硬件编码。
    private var statFrames = 0
    private var statWait: UInt64 = 0
    private var statConvert: UInt64 = 0
    private var statEncode: UInt64 = 0
    private var statEncodeFrames = 0

    /// 取出并清零分段计时：(帧数, 平均排队 ms, 平均转换 ms, 平均编码 ms)。
    func takeStats() -> (frames: Int, wait: Double, convert: Double, encode: Double) {
        lock.lock()
        defer {
            statFrames = 0
            statWait = 0
            statConvert = 0
            statEncode = 0
            statEncodeFrames = 0
            lock.unlock()
        }
        guard statFrames > 0 else { return (0, 0, 0, 0) }
        return (statFrames, Double(statWait) / 1000 / Double(statFrames), Double(statConvert) / 1000 / Double(statFrames),
                statEncodeFrames > 0 ? Double(statEncode) / 1000 / Double(statEncodeFrames) : 0)
    }

    // 两级流水线：转换第 N+1 帧的同时，硬件编码第 N 帧（硬件编码提交是阻塞的，串在一起会把每帧预算用满）。
    private let convertQueue = DispatchQueue(label: "macstream.convert", qos: .userInteractive)
    private let submitQueue = DispatchQueue(label: "macstream.submit", qos: .userInteractive)

    /// 异步编码，可在任意线程调用（按调用顺序输出）。refine 为 true 表示静止画面的补清晰帧（客户端不计入延迟统计）。
    func encode(_ source: CVPixelBuffer, captureMicros: UInt64, refine: Bool) {
        let enqueued = Clock.nowMicros()
        convertQueue.async { [self] in
            let started = Clock.nowMicros()
            guard let pixelBuffer = convert(source) else {
                requestKeyframe()
                return
            }
            let converted = Clock.nowMicros()
            lock.lock()
            statFrames += 1
            statWait += started - enqueued
            statConvert += converted - started
            lock.unlock()
            submitQueue.async { [self] in
                submit(pixelBuffer, captureMicros: captureMicros, refine: refine)
            }
        }
    }

    private func submit(_ pixelBuffer: CVPixelBuffer, captureMicros: UInt64, refine: Bool) {
        let submitted = Clock.nowMicros()
        lock.lock()
        let forceKey = keyframeRequested
        keyframeRequested = false
        lock.unlock()

        let properties = forceKey ? [kVTEncodeFrameOptionKey_ForceKeyFrame as String: true] as CFDictionary : nil
        let pts = CMTime(value: CMTimeValue(captureMicros), timescale: 1_000_000)
        let status = VTCompressionSessionEncodeFrame(
            session, imageBuffer: pixelBuffer, presentationTimeStamp: pts, duration: .invalid,
            frameProperties: properties, infoFlagsOut: nil
        ) { [weak self] status, _, sampleBuffer in
            guard let self else { return }
            guard status == noErr, let sampleBuffer, let (data, isKey) = Self.annexB(sampleBuffer) else {
                if status != noErr {
                    Log.info("编码失败 \(status)")
                    self.requestKeyframe()
                }
                return
            }
            let done = Clock.nowMicros()
            self.lock.lock()
            self.statEncode += done > submitted ? done - submitted : 0
            self.statEncodeFrames += 1
            self.lock.unlock()
            self.output(data, isKey, captureMicros, refine)
        }
        if status != noErr {
            Log.info("提交编码失败 \(status)")
            requestKeyframe()
        }
    }

    private func set(_ key: CFString, _ value: CFTypeRef) {
        let status = VTSessionSetProperty(session, key: key, value: value)
        if status != noErr {
            Log.info("编码器属性 \(key) 设置失败 \(status)")
        }
    }

    /// AVCC（4 字节长度前缀）→ Annex-B；关键帧前附带 SPS/PPS。
    private static func annexB(_ sampleBuffer: CMSampleBuffer) -> ([UInt8], Bool)? {
        guard let block = CMSampleBufferGetDataBuffer(sampleBuffer) else { return nil }
        var isKey = true
        if let attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, createIfNecessary: false) as? [[String: Any]],
           let notSync = attachments.first?[kCMSampleAttachmentKey_NotSync as String] as? Bool {
            isKey = !notSync
        }

        let startCode: [UInt8] = [0, 0, 0, 1]
        var out: [UInt8] = []
        let total = CMBlockBufferGetDataLength(block)
        out.reserveCapacity(total + 128)

        if isKey, let format = CMSampleBufferGetFormatDescription(sampleBuffer) {
            var count = 0
            CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                format, parameterSetIndex: 0, parameterSetPointerOut: nil, parameterSetSizeOut: nil,
                parameterSetCountOut: &count, nalUnitHeaderLengthOut: nil)
            for index in 0..<count {
                var pointer: UnsafePointer<UInt8>?
                var size = 0
                if CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                    format, parameterSetIndex: index, parameterSetPointerOut: &pointer, parameterSetSizeOut: &size,
                    parameterSetCountOut: nil, nalUnitHeaderLengthOut: nil) == noErr, let pointer {
                    out += startCode
                    out += UnsafeBufferPointer(start: pointer, count: size)
                }
            }
        }

        var avcc = [UInt8](repeating: 0, count: total)
        guard CMBlockBufferCopyDataBytes(block, atOffset: 0, dataLength: total, destination: &avcc) == noErr else {
            return nil
        }
        var offset = 0
        while offset + 4 <= total {
            let length = Int(avcc[offset]) << 24 | Int(avcc[offset + 1]) << 16 | Int(avcc[offset + 2]) << 8 | Int(avcc[offset + 3])
            offset += 4
            guard length > 0, offset + length <= total else { break }
            out += startCode
            out += avcc[offset..<offset + length]
            offset += length
        }
        return (out, isKey)
    }
}
