#if os(iOS)
import Foundation
import ICamWire
import Network

/// TCP listener on 127.0.0.1:47800. The PC reaches it through usbmuxd over the USB cable; binding to
/// loopback means nothing on Wi-Fi or cellular can connect. One host at a time: a new connection
/// is refused while the current host is connected.
///
/// Backpressure: we count bytes handed to Network.framework that it hasn't finished sending. Video
/// frames are refused once that exceeds the budget, so a slow link drops frames instead of building
/// latency; control messages always go through.
final class Server: @unchecked Sendable {
    let queue = DispatchQueue(label: "icam.net", qos: .userInteractive)
    var onConnect: (() -> Void)?
    var onDisconnect: (() -> Void)?
    var onMessage: ((Message) -> Void)?
    var onListenerState: ((String) -> Void)?
    /// Called on `queue` whenever the congestion state flips (send queue above/below half the budget).
    var onCongestion: ((Bool) -> Void)?

    private var listener: NWListener?
    private var connection: NWConnection?
    private var framer = Framer(maxPayload: Wire.maxControlPayload)
    private var hostSession = HostSession()
    private var rateWindow = DispatchTime.now().uptimeNanoseconds
    private var receivedMessages = 0
    private var nextAcceptTime: UInt64 = 0
    private var inFlight = 0
    private var connectionId = 0       // send completions from an old connection must not touch inFlight
    private var lastCongested = false
    private(set) var budgetBytes = 1_500_000
    private var restartScheduled = false

    // Counters for the Status message (read on `queue`).
    private(set) var sentBytes = 0
    private(set) var droppedFrames = 0

    func start() { queue.async { self.startListener() } }

    func stop() {
        queue.async {
            self.listener?.cancel()
            self.listener = nil
            self.drop()
        }
    }

    /// Called when returning to the foreground: iOS may have torn the listener down.
    func ensureListening() {
        queue.async {
            if self.listener == nil || self.listener?.state != .ready { self.startListener() }
        }
    }

    var isConnected: Bool { connection != nil }
    var currentConnectionId: Int { connectionId }  // read only on queue

    func setBudget(bitrate: Int) {
        // About 250 ms of video, at least 1 MB (a 4K keyframe can be several hundred KB).
        budgetBytes = max(1_000_000, bitrate / 8 / 4)
    }

    private func startListener() {
        listener?.cancel()
        listener = nil
        let params = NWParameters.tcp
        params.allowLocalEndpointReuse = true
        if let tcp = params.defaultProtocolStack.transportProtocol as? NWProtocolTCP.Options { tcp.noDelay = true }
        params.requiredLocalEndpoint = .hostPort(host: "127.0.0.1", port: NWEndpoint.Port(rawValue: Wire.devicePort)!)
        do {
            let l = try NWListener(using: params)
            l.stateUpdateHandler = { [weak self, weak l] state in
                guard let self else { return }
                switch state {
                case .ready: self.onListenerState?("ready")
                case .failed(let e):
                    self.onListenerState?("failed: \(e)")
                    l?.cancel()
                    if self.listener === l { self.listener = nil }
                    self.scheduleRestart()
                default: break
                }
            }
            l.newConnectionHandler = { [weak self] c in self?.accept(c) }
            l.start(queue: queue)
            listener = l
        } catch {
            onListenerState?("error: \(error)")
            scheduleRestart()
        }
    }

    private func scheduleRestart() {
        guard !restartScheduled else { return }
        restartScheduled = true
        queue.asyncAfter(deadline: .now() + 1) {
            self.restartScheduled = false
            if self.listener == nil { self.startListener() }
        }
    }

    private func accept(_ c: NWConnection) {
        guard connection == nil, DispatchTime.now().uptimeNanoseconds >= nextAcceptTime else { c.cancel(); return }
        connection = c
        connectionId += 1
        framer = Framer(maxPayload: Wire.maxControlPayload)
        hostSession = HostSession()
        rateWindow = DispatchTime.now().uptimeNanoseconds
        receivedMessages = 0
        inFlight = 0
        c.stateUpdateHandler = { [weak self, weak c] state in
            guard let self, let c, self.connection === c else { return }
            switch state {
            case .ready:
                self.onConnect?()
                self.receive(c)
            case .failed, .cancelled:
                self.drop()
            default: break
            }
        }
        c.start(queue: queue)
        queue.asyncAfter(deadline: .now() + 3) { [weak self, weak c] in
            guard let self, let c, self.connection === c, !self.hostSession.ready else { return }
            self.drop()
        }
    }

    private func receive(_ c: NWConnection) {
        c.receive(minimumIncompleteLength: 1, maximumLength: 64 * 1024) { [weak self, weak c] data, _, complete, error in
            guard let self, let c, self.connection === c else { return }
            if let data, !data.isEmpty {
                guard self.framer.push(data) else { self.drop(); return }  // not our protocol
                while let (type, payload) = self.framer.pop() {
                    let now = DispatchTime.now().uptimeNanoseconds
                    if now - self.rateWindow >= 1_000_000_000 { self.rateWindow = now; self.receivedMessages = 0 }
                    self.receivedMessages += 1
                    guard self.receivedMessages <= 120, let m = Codec.decode(type, payload),
                          self.hostSession.accept(m) else { self.drop(); return }
                    self.onMessage?(m)
                    guard self.connection === c else { return }
                }
                if self.framer.corrupt { self.drop(); return }
            }
            if complete || error != nil { self.drop(); return }
            self.receive(c)
        }
    }

    /// Must be called on `queue`. Returns false if a video frame was dropped for backpressure.
    @discardableResult
    func send(_ bytes: [UInt8], video: Bool = false) -> Bool {
        guard let c = connection else { return false }
        if video && inFlight > budgetBytes {
            droppedFrames += 1
            return false
        }
        // A peer that writes commands but never reads replies cannot grow the control queue forever.
        if !video && inFlight + bytes.count > budgetBytes + Wire.maxControlPayload { drop(); return false }
        inFlight += bytes.count
        sentBytes += bytes.count
        let id = connectionId
        c.send(content: Data(bytes), completion: .contentProcessed { [weak self] _ in
            guard let self, self.connectionId == id else { return }
            self.inFlight -= bytes.count
            self.reportCongestion()
        })
        reportCongestion()
        return true
    }

    private func reportCongestion() {
        let now = congested
        if now != lastCongested {
            lastCongested = now
            onCongestion?(now)
        }
    }

    /// Drops the current host connection (e.g. it went silent) and keeps listening.
    func disconnect() {
        dispatchPrecondition(condition: .onQueue(queue))
        drop()
    }

    var congested: Bool { inFlight > budgetBytes / 2 }

    private func drop() {
        guard let c = connection else { return }
        connection = nil
        nextAcceptTime = DispatchTime.now().uptimeNanoseconds + 2_000_000_000
        connectionId += 1
        c.stateUpdateHandler = nil
        c.cancel()
        inFlight = 0
        reportCongestion()
        onDisconnect?()
    }
}
#endif
