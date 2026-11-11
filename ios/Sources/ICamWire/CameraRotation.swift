// Webcam output follows portrait/landscape, rather than continuously reconfiguring video buffers
// for small gravity-sensor changes while the phone is moving or resting on a stand.
public enum CameraRotation {
    public static func snapped(_ degrees: Double) -> Double {
        let turn = (degrees / 90).rounded().truncatingRemainder(dividingBy: 4)
        return (turn < 0 ? turn + 4 : turn) * 90
    }
}
