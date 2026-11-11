#if os(iOS)
import ICamWire
import SwiftUI

/// Camera-app style screen: big preview, lens pills (0,5 · 1× · 2), a round "shutter" that shows
/// whether the PC is receiving video, and the flip button. Landscape puts the controls on the right,
/// like the iPhone Camera app.
struct ContentView: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        GeometryReader { geo in
            ZStack {
                Color.black.ignoresSafeArea()
                if model.permission == .denied || model.permission == .restricted {
                    PermissionHelp()
                } else {
                    let landscape = geo.size.width > geo.size.height
                    let layout = landscape ? AnyLayout(HStackLayout(spacing: 14)) : AnyLayout(VStackLayout(spacing: 12))
                    VStack(spacing: 12) {
                        TopBar()
                        // AnyLayout preserves the preview's identity when changing direction. Using
                        // two conditional screen trees tore down and reattached the running camera.
                        layout {
                            VStack(spacing: 8) {
                                PreviewCard()
                                StatsLine()
                            }
                            .frame(maxWidth: .infinity, maxHeight: .infinity)
                            if landscape {
                                ControlsColumn().frame(width: 104)
                            } else {
                                VStack(spacing: 12) {
                                    LensPills(vertical: false)
                                    BottomBar()
                                }
                            }
                        }
                        if !landscape { HelpLine() }
                    }
                    .padding(.horizontal, 16)
                    .padding(.vertical, 8)
                }
            }
        }
        .animation(.easeInOut(duration: 0.25), value: model.state.cameraId)
    }
}

// MARK: - top bar

struct TopBar: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        HStack(spacing: 10) {
            StatusPill()
            Spacer(minLength: 4)
            QualityMenu()
            Menu {
                Button { model.toggleFraming() } label: {
                    Label(model.state.fillFrame ? "Mostrar imagen completa" : "Llenar encuadre 16:9", systemImage: "aspectratio")
                }
                Button { model.toggleMirror() } label: {
                    Label(model.mirror ? "Desactivar espejo" : "Activar espejo", systemImage: "arrow.left.and.right")
                }
            } label: {
                Image(systemName: "aspectratio").font(.system(size: 20, weight: .semibold))
                    .foregroundStyle(model.state.fillFrame ? Color.yellow : Color.white)
                    .frame(width: 44, height: 44).background(.white.opacity(0.14), in: Circle())
            }
            .accessibilityLabel("Encuadre y espejo")
            RoundIcon(system: model.preview ? "eye" : "eye.slash", active: !model.preview, help: "Vista previa") {
                model.togglePreview()
            }
            RoundIcon(system: model.dim ? "moon.fill" : "moon", active: model.dim, help: "Atenuar") { model.toggleDim() }
        }
    }
}

struct StatusPill: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        let s = model.state
        let (icon, text, color): (String, String, Color) = {
            if s.userPaused || s.paused != nil { return ("pause.fill", "EN PAUSA", .orange) }
            if s.streaming { return ("circle.fill", "EN DIRECTO", .red) }
            if let h = s.hostName { return ("laptopcomputer", h, .green) }
            return ("cable.connector", s.listening ? "Conecta al PC por USB" : "Iniciando…", .gray)
        }()
        return HStack(spacing: 6) {
            Image(systemName: icon).font(.caption2.weight(.bold))
            Text(text).font(.caption.weight(.semibold)).lineLimit(1)
        }
        .foregroundStyle(s.streaming ? Color.white : color)
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
        .background(s.streaming ? AnyShapeStyle(Color.red) : AnyShapeStyle(color.opacity(0.18)), in: Capsule())
    }
}

struct QualityMenu: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        let s = model.state
        Menu {
            Section("Calidad del vídeo") {
                ForEach(Quality.allCases) { q in
                    Button {
                        model.setQuality(q)
                    } label: {
                        if q == s.quality {
                            Label("\(q.label) — \(q.detail)", systemImage: "checkmark")
                        } else {
                            Text("\(q.label) — \(q.detail)")
                        }
                    }
                    .disabled(!(q.supported(by: model.currentCamera?.formats ?? [])))
                }
            }
            Section {
                Text("Ajusta el vídeo que sale del iPhone. Teams decide la resolución de la llamada.")
            }
        } label: {
            VStack(spacing: 0) {
                Text(s.streaming ? (s.height == 2160 ? "4K · \(s.fps)" : "\(s.height)p · \(s.fps)") : s.quality.label)
                    .font(.caption.weight(.bold).monospacedDigit())
                Text("CALIDAD").font(.system(size: 8, weight: .semibold))
            }
            .foregroundStyle(.white)
            .padding(.horizontal, 9)
            .frame(minHeight: 44)
            .background(.white.opacity(0.14), in: RoundedRectangle(cornerRadius: 8))
        }
        .accessibilityLabel("Calidad de captura")
        .accessibilityValue(s.quality.label)
    }
}

struct RoundIcon: View {
    let system: String
    var active = false
    var size: CGFloat = 44
    let help: String
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Image(systemName: system)
                .font(.system(size: size * 0.45, weight: .semibold))
                .foregroundStyle(active ? Color.yellow : Color.white)
                .frame(width: size, height: size)
                .background(.white.opacity(0.14), in: Circle())
        }
        .accessibilityLabel(help)
    }
}

// MARK: - preview

struct PreviewCard: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        let s = model.state
        ZStack {
            if model.preview {
                CameraPreview(session: model.pipeline.camera.session, cameraId: s.cameraId,
                              fillFrame: s.fillFrame, mirrored: model.mirror)
            } else {
                Color(white: 0.06)
                VStack(spacing: 8) {
                    Image(systemName: "eye.slash").font(.title2)
                    Text(s.streaming ? "Vista previa oculta · el PC sigue recibiendo vídeo" : "Vista previa oculta")
                        .font(.footnote)
                }
                .foregroundStyle(.secondary)
            }
            if s.userPaused || s.paused != nil {
                VStack(spacing: 8) {
                    Image(systemName: "pause.circle.fill").font(.system(size: 40))
                    Text(s.userPaused ? "Vídeo en pausa" : (s.paused ?? "Cámara en pausa"))
                        .font(.callout.weight(.medium)).multilineTextAlignment(.center)
                }
                .padding(16)
                .background(.black.opacity(0.7), in: RoundedRectangle(cornerRadius: 14))
                .padding()
            }
            VStack {
                HStack {
                    if s.streaming {
                        HStack(spacing: 5) {
                            Circle().fill(.red).frame(width: 8, height: 8)
                            Text("EN DIRECTO").font(.caption2.weight(.bold))
                        }
                        .padding(.horizontal, 8)
                        .padding(.vertical, 4)
                        .background(.black.opacity(0.55), in: Capsule())
                    }
                    Spacer()
                    if s.thermal >= 2 {
                        Label(s.fpsCap > 0 ? "Caliente · \(s.fpsCap) fps" : "Caliente", systemImage: "thermometer.high")
                            .font(.caption2.weight(.semibold))
                            .foregroundStyle(.orange)
                            .padding(.horizontal, 8)
                            .padding(.vertical, 4)
                            .background(.black.opacity(0.55), in: Capsule())
                    }
                }
                Spacer()
                if s.portrait {
                    Label(s.fillFrame ? "16:9 · recorte centrado" : "Imagen completa · bandas laterales", systemImage: "aspectratio")
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.yellow)
                        .padding(.horizontal, 10)
                        .padding(.vertical, 5)
                        .background(.black.opacity(0.6), in: Capsule())
                }
            }
            .padding(8)
        }
        .aspectRatio(16.0 / 9.0, contentMode: .fit)
        .clipShape(RoundedRectangle(cornerRadius: 14))
        .overlay(RoundedRectangle(cornerRadius: 14).stroke(s.streaming ? Color.red.opacity(0.8) : Color.clear, lineWidth: 2))
    }
}

struct StatsLine: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        let s = model.state
        HStack(spacing: 12) {
            if s.streaming {
                Text("\(s.width)×\(s.height) · \(Int(s.measuredFps.rounded())) fps · " + String(format: "%.1f Mbps", s.mbps))
            } else if s.hostName != nil {
                Text("Listo. Elige «iPhone Camera» en Teams.")
            } else {
                Text(" ")
            }
            if s.droppedFrames + s.encoderDrops > 0 {
                Text("descartados \(s.droppedFrames + s.encoderDrops)").foregroundStyle(.secondary)
            }
        }
        .font(.caption.monospacedDigit())
        .lineLimit(1)
        .minimumScaleFactor(0.75)
        .foregroundStyle(.white.opacity(0.75))
        .frame(maxWidth: .infinity)
    }
}

// MARK: - lens pills, shutter, flip

struct LensPills: View {
    @EnvironmentObject var model: AppModel
    let vertical: Bool

    var body: some View {
        let lenses = model.isFront ? [] : model.backLenses
        let layout = vertical ? AnyLayout(VStackLayout(spacing: 10)) : AnyLayout(HStackLayout(spacing: 10))
        layout {
            ForEach(lenses) { lens in
                let on = model.isSelected(lens)
                Button {
                    model.select(lens)
                } label: {
                    Text(on ? "\(lens.label)×" : lens.label)
                        .font(.system(size: on ? 13 : 11, weight: .bold).monospacedDigit())
                        .foregroundStyle(on ? Color.yellow : Color.white)
                        .frame(width: on ? 40 : 32, height: on ? 40 : 32)
                        .background(.black.opacity(0.55), in: Circle())
                        .overlay(Circle().stroke(.white.opacity(0.15), lineWidth: 1))
                }
            }
        }
        .padding(6)
        .background(.white.opacity(lenses.isEmpty ? 0 : 0.08), in: Capsule())
        .frame(height: vertical ? nil : 52)
    }
}

struct ShutterStatus: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        let s = model.state
        let fill: Color = s.userPaused ? .orange : s.streaming ? .red : .white
        Button { model.togglePause() } label: {
          VStack(spacing: 6) {
            ZStack {
                Circle().stroke(.white, lineWidth: 4).frame(width: 74, height: 74)
                if s.streaming {
                    RoundedRectangle(cornerRadius: 8).fill(fill).frame(width: 32, height: 32)
                } else {
                    Circle().fill(fill).frame(width: 60, height: 60)
                    Image(systemName: s.userPaused ? "play.fill" : "pause.fill")
                        .font(.system(size: 22, weight: .bold)).foregroundStyle(.black)
                }
            }
            Text(s.userPaused ? "REANUDAR" : s.streaming ? "PAUSAR" : "LISTO")
                .font(.caption2.weight(.bold))
                .foregroundStyle(s.streaming ? Color.red : Color.white.opacity(0.8))
          }
        }
        .buttonStyle(.plain)
        .accessibilityLabel(s.userPaused ? "Reanudar vídeo" : "Pausar vídeo")
    }
}

struct FlipButton: View {
    @EnvironmentObject var model: AppModel
    @State private var turns = 0.0

    var body: some View {
        Button {
            withAnimation(.easeInOut(duration: 0.35)) { turns += 180 }
            model.flipCamera()
        } label: {
            Image(systemName: "arrow.triangle.2.circlepath")
                .font(.system(size: 22, weight: .semibold))
                .foregroundStyle(.white)
                .rotationEffect(.degrees(turns))
                .frame(width: 54, height: 54)
                .background(.white.opacity(0.14), in: Circle())
        }
        .accessibilityLabel(model.isFront ? "Usar cámara trasera" : "Usar cámara frontal")
    }
}

struct BottomBar: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        HStack {
            BatteryBadge().frame(width: 54)
            Spacer()
            ShutterStatus()
            Spacer()
            FlipButton()
        }
        .padding(.horizontal, 12)
    }
}

struct ControlsColumn: View {
    var body: some View {
        VStack(spacing: 18) {
            FlipButton()
            Spacer(minLength: 0)
            ShutterStatus()
            Spacer(minLength: 0)
            LensPills(vertical: true)
            BatteryBadge()
        }
        .padding(.vertical, 8)
    }
}

struct BatteryBadge: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        let b = model.pipeline.battery
        let level = b.level == 255 ? nil : Int(b.level)
        let symbol: String = {
            if b.charging { return "battery.100.bolt" }
            guard let l = level else { return "battery.50" }
            return l > 75 ? "battery.100" : l > 50 ? "battery.75" : l > 25 ? "battery.50" : "battery.25"
        }()
        return VStack(spacing: 2) {
            Image(systemName: symbol).font(.system(size: 16))
            if let level { Text("\(level)%").font(.caption2.monospacedDigit()) }
        }
        .foregroundStyle(.white.opacity(0.7))
    }
}

struct HelpLine: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        Text(model.state.streaming
             ? "Deja la app abierta y el iPhone desbloqueado."
             : "Conecta el iPhone al PC con cable y elige «iPhone Camera» en Teams.")
            .font(.caption2)
            .foregroundStyle(.white.opacity(0.5))
            .multilineTextAlignment(.center)
    }
}

struct PermissionHelp: View {
    var body: some View {
        VStack(spacing: 12) {
            Image(systemName: "camera.fill").font(.system(size: 44))
            Text("iCam necesita acceso a la cámara").font(.headline)
            Text("Actívalo en Ajustes › Privacidad y seguridad › Cámara › iCam.").multilineTextAlignment(.center)
            Button("Abrir Ajustes") {
                if let url = URL(string: UIApplication.openSettingsURLString) { UIApplication.shared.open(url) }
            }
            .buttonStyle(.borderedProminent)
        }
        .foregroundStyle(.white)
        .padding(32)
    }
}
#endif
