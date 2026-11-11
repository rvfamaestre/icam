#if os(iOS)
import CoreMedia
import Foundation
import VideoToolbox

/// One encoded access unit in Annex-B, ready for the wire.
struct EncodedFrame {
    var annexB: [UInt8]
    var key: Bool
    var ptsUs: UInt64
    var sps: [UInt8]?
    var pps: [UInt8]?
}

/// Hardware H.264 encoder (VTCompressionSession) tuned for live video: real-time, no frame
/// reordering (no B-frames), keyframe on demand, BT.709 SDR. The output size is fixed per session;
/// VideoToolbox scales with a centred crop or letterbox, preserving the camera's aspect ratio.
final class H264Encoder {
    let width: Int32, height: Int32, fps: Int32
    private(set) var bitrate: Int
    private var session: VTCompressionSession?
    private let onOutput: (EncodedFrame) -> Void
    private let lock = NSLock()
    private var forceKey = true
    private var _pending = 0
    private var _failures = 0
    private var _dropped = 0
    /// Frames VideoToolbox chose not to encode (rate control). Not an error.
    var droppedByEncoder: Int { lock.lock(); defer { lock.unlock() }; return _dropped }
    var pending: Int { lock.lock(); defer { lock.unlock() }; return _pending }
    /// Consecutive frames the encoder failed on; the pipeline rebuilds the encoder past a threshold.
    var consecutiveFailures: Int { lock.lock(); defer { lock.unlock() }; return _failures }

    static func defaultBitrate(width: Int, height: Int, fps: Int) -> Int {
        // Bits per pixel per frame: generous at 30 fps (webcam scenes are mostly static), a bit less at 60.
        let bpp = fps > 30 ? 0.11 : 0.16
        return min(80_000_000, max(1_500_000, Int(Double(width * height * fps) * bpp)))
    }

    init?(width: Int, height: Int, fps: Int, bitrate: Int, fillFrame: Bool = false,
          onOutput: @escaping (EncodedFrame) -> Void) {
        self.width = Int32(width)
        self.height = Int32(height)
        self.fps = Int32(fps)
        self.bitrate = bitrate
        self.onOutput = onOutput
        // Low-latency rate control keeps one frame in flight; at 4K it caps throughput, so use the
        // normal real-time path (still no reordering) above 1440p.
        let lowLatency = width * height <= 2560 * 1440
        let spec: [CFString: Any] = lowLatency ? [kVTVideoEncoderSpecification_EnableLowLatencyRateControl: true] : [:]
        var s: VTCompressionSession?
        let status = VTCompressionSessionCreate(
            allocator: kCFAllocatorDefault, width: self.width, height: self.height, codecType: kCMVideoCodecType_H264,
            encoderSpecification: spec as CFDictionary, imageBufferAttributes: nil, compressedDataAllocator: nil,
            outputCallback: nil, refcon: nil, compressionSessionOut: &s)
        guard status == noErr, let s else { return nil }
        session = s
        func set(_ key: CFString, _ value: Any) { VTSessionSetProperty(s, key: key, value: value as CFTypeRef) }
        set(kVTCompressionPropertyKey_RealTime, kCFBooleanTrue!)
        set(kVTCompressionPropertyKey_ProfileLevel,
            lowLatency ? kVTProfileLevel_H264_ConstrainedHigh_AutoLevel : kVTProfileLevel_H264_High_AutoLevel)
        set(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse!)
        if !lowLatency { set(kVTCompressionPropertyKey_MaxFrameDelayCount, NSNumber(value: 1)) }
        set(kVTCompressionPropertyKey_ExpectedFrameRate, NSNumber(value: fps))
        set(kVTCompressionPropertyKey_MaxKeyFrameInterval, NSNumber(value: fps * 2))
        set(kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, NSNumber(value: 2))
        set(kVTCompressionPropertyKey_AverageBitRate, NSNumber(value: bitrate))
        // Cap bursts at 2× the average over one second so a scene change can't flood USB. Tighter caps
        // make low-latency rate control drop whole frames when the phone moves (measured: 24–26 fps).
        set(kVTCompressionPropertyKey_DataRateLimits, [NSNumber(value: bitrate / 4), NSNumber(value: 1)] as CFArray)
        set(kVTCompressionPropertyKey_ColorPrimaries, kCMFormatDescriptionColorPrimaries_ITU_R_709_2)
        set(kVTCompressionPropertyKey_TransferFunction, kCMFormatDescriptionTransferFunction_ITU_R_709_2)
        set(kVTCompressionPropertyKey_YCbCrMatrix, kCMFormatDescriptionYCbCrMatrix_ITU_R_709_2)
        let scaling = fillFrame ? kVTScalingMode_Trim : kVTScalingMode_Letterbox
        guard VTSessionSetProperty(s, key: kVTCompressionPropertyKey_PixelTransferProperties,
            value: [kVTPixelTransferPropertyKey_ScalingMode: scaling] as CFDictionary) == noErr else {
            VTCompressionSessionInvalidate(s)
            session = nil
            return nil
        }
        guard VTCompressionSessionPrepareToEncodeFrames(s) == noErr else {
            VTCompressionSessionInvalidate(s)
            session = nil
            return nil
        }
    }

    deinit { invalidate() }

    func invalidate() {
        lock.lock()
        let s = session
        session = nil
        lock.unlock()
        if let s {
            VTCompressionSessionCompleteFrames(s, untilPresentationTimeStamp: .invalid)
            VTCompressionSessionInvalidate(s)
        }
    }

    func forceKeyframe() {
        lock.lock(); forceKey = true; lock.unlock()
    }

    func setBitrate(_ bps: Int) {
        lock.lock(); let s = session; bitrate = bps; lock.unlock()
        guard let s else { return }
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_AverageBitRate, value: NSNumber(value: bps))
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_DataRateLimits,
                             value: [NSNumber(value: bps / 4), NSNumber(value: 1)] as CFArray)
    }

    /// `ptsUs` is the capture time on the host-time clock (same clock as Ping/Pong).
    func encode(_ pixelBuffer: CVPixelBuffer, pts: CMTime, ptsUs: UInt64) {
        lock.lock()
        guard let s = session else { lock.unlock(); return }
        let key = forceKey
        forceKey = false
        _pending += 1
        lock.unlock()
        let props: CFDictionary? = key ? [kVTEncodeFrameOptionKey_ForceKeyFrame: kCFBooleanTrue!] as CFDictionary : nil
        let status = VTCompressionSessionEncodeFrame(s, imageBuffer: pixelBuffer, presentationTimeStamp: pts,
                                                     duration: .invalid, frameProperties: props, infoFlagsOut: nil) {
            [weak self] status, info, sample in
            self?.handleOutput(status: status, info: info, sample: sample, ptsUs: ptsUs)
        }
        if status != noErr {
            lock.lock(); _pending -= 1; _failures += 1; forceKey = true; lock.unlock()
        }
    }

    private func handleOutput(status: OSStatus, info: VTEncodeInfoFlags, sample: CMSampleBuffer?, ptsUs: UInt64) {
        lock.lock(); _pending -= 1; lock.unlock()
        if status == noErr && info.contains(.frameDropped) {
            lock.lock(); _dropped += 1; lock.unlock()
            return
        }
        guard status == noErr, let sample, CMSampleBufferDataIsReady(sample),
              let block = CMSampleBufferGetDataBuffer(sample) else {
            lock.lock(); _failures += 1; lock.unlock()
            forceKeyframe()
            return
        }
        lock.lock(); _failures = 0; lock.unlock()
        var key = true
        if let atts = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: false) as? [[CFString: Any]],
           let first = atts.first, let notSync = first[kCMSampleAttachmentKey_NotSync] as? Bool {
            key = !notSync
        }
        var sps: [UInt8]?, pps: [UInt8]?
        var prefix = 4  // AVCC NAL length size; VideoToolbox uses 4, but honour what the format says
        if let fmt = CMSampleBufferGetFormatDescription(sample) {
            if key {
                sps = Self.parameterSet(fmt, 0)
                pps = Self.parameterSet(fmt, 1)
            }
            var n: Int32 = 0
            if CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, parameterSetIndex: 0, parameterSetPointerOut: nil,
                                                                  parameterSetSizeOut: nil, parameterSetCountOut: nil,
                                                                  nalUnitHeaderLengthOut: &n) == noErr, [1, 2, 4].contains(n) {
                prefix = Int(n)
            }
        }
        // AVCC (4-byte big-endian lengths) -> Annex-B start codes.
        let total = CMBlockBufferGetDataLength(block)
        var avcc = [UInt8](repeating: 0, count: total)
        guard CMBlockBufferCopyDataBytes(block, atOffset: 0, dataLength: total, destination: &avcc) == noErr else { return }
        var out = [UInt8]()
        out.reserveCapacity(total + 16)
        var i = 0
        while i + prefix <= total {
            var len = 0
            for k in 0..<prefix { len = len << 8 | Int(avcc[i + k]) }
            i += prefix
            guard len > 0, len <= total - i else { forceKeyframe(); return }
            out.append(contentsOf: [0, 0, 0, 1])
            out.append(contentsOf: avcc[i..<i + len])
            i += len
        }
        guard i == total else { forceKeyframe(); return }  // trailing garbage: don't send a damaged unit
        onOutput(EncodedFrame(annexB: out, key: key, ptsUs: ptsUs, sps: sps, pps: pps))
    }

    private static func parameterSet(_ fmt: CMFormatDescription, _ index: Int) -> [UInt8]? {
        var ptr: UnsafePointer<UInt8>?
        var size = 0
        var count = 0
        var nalHeader: Int32 = 0
        let st = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
            fmt, parameterSetIndex: index, parameterSetPointerOut: &ptr, parameterSetSizeOut: &size,
            parameterSetCountOut: &count, nalUnitHeaderLengthOut: &nalHeader)
        guard st == noErr, let ptr, size > 0 else { return nil }
        return Array(UnsafeBufferPointer(start: ptr, count: size))
    }
}
#endif
