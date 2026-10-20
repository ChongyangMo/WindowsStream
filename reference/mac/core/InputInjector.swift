import AppKit
import CoreGraphics

/// 把客户端的键鼠事件注入为系统级 CGEvent（需要“辅助功能”权限）。所有方法在同一串行队列上调用。
final class InputInjector {
    private let source = CGEventSource(stateID: .hidSystemState)
    private let displayID: () -> CGDirectDisplayID
    private var position = CGPoint.zero
    private var pressedButtons = Set<Int>()
    private var pressedKeys = Set<UInt16>()
    private var capsLockOn = false
    private var flags: CGEventFlags = []
    private var clickCount = 0
    private var lastClickTime: TimeInterval = 0
    private var lastClickButton = -1
    private var lastClickPosition = CGPoint.zero
    private var scrollRemainderX = 0.0
    private var scrollRemainderY = 0.0

    /// Windows 滚轮一格（120）对应的像素数；用像素单位滚动，触控板也能平滑。
    private let pixelsPerNotch = 48.0

    private static let modifierFlags: [UInt16: CGEventFlags] = [
        0x37: .maskCommand, 0x36: .maskCommand,
        0x38: .maskShift, 0x3C: .maskShift,
        0x3A: .maskAlternate, 0x3D: .maskAlternate,
        0x3B: .maskControl, 0x3E: .maskControl,
        0x3F: .maskSecondaryFn,
    ]
    private static let capsLock: UInt16 = 0x39
    private static let arrowKeys: Set<UInt16> = [0x7B, 0x7C, 0x7D, 0x7E]
    private static let functionKeys: Set<UInt16> = [
        0x72, 0x73, 0x74, 0x75, 0x77, 0x79, // Help Home PageUp ForwardDelete End PageDown
        0x7A, 0x78, 0x63, 0x76, 0x60, 0x61, 0x62, 0x64, 0x65, 0x6D, 0x67, 0x6F, // F1–F12
        0x69, 0x6B, 0x71, 0x6A, 0x40, 0x4F, 0x50, 0x5A, // F13–F20
    ]
    private static let keypadKeys: Set<UInt16> = [
        0x41, 0x43, 0x45, 0x47, 0x4B, 0x4C, 0x4E, 0x51,
        0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5B, 0x5C,
    ]

    init(displayID: @escaping () -> CGDirectDisplayID) {
        self.displayID = displayID
    }

    func move(x: UInt16, y: UInt16) {
        let point = map(x, y)
        let type: CGEventType
        let button: CGMouseButton
        if pressedButtons.contains(0) {
            (type, button) = (.leftMouseDragged, .left)
        } else if pressedButtons.contains(1) {
            (type, button) = (.rightMouseDragged, .right)
        } else if let other = pressedButtons.first {
            (type, button) = (.otherMouseDragged, CGMouseButton(rawValue: UInt32(other)) ?? .center)
        } else {
            (type, button) = (.mouseMoved, .left)
        }
        guard let event = CGEvent(mouseEventSource: source, mouseType: type, mouseCursorPosition: point, mouseButton: button) else { return }
        event.setIntegerValueField(.mouseEventDeltaX, value: Int64((point.x - position.x).rounded()))
        event.setIntegerValueField(.mouseEventDeltaY, value: Int64((point.y - position.y).rounded()))
        event.flags = flags
        event.post(tap: .cghidEventTap)
        position = point
    }

    func button(_ button: Int, down: Bool, x: UInt16, y: UInt16) {
        sendButton(button, down: down, at: map(x, y))
    }

    func scroll(dx: Int16, dy: Int16) {
        scrollRemainderY += Double(dy) / 120 * pixelsPerNotch
        scrollRemainderX += -Double(dx) / 120 * pixelsPerNotch
        let pixelsY = scrollRemainderY.rounded(.towardZero)
        let pixelsX = scrollRemainderX.rounded(.towardZero)
        scrollRemainderY -= pixelsY
        scrollRemainderX -= pixelsX
        guard pixelsY != 0 || pixelsX != 0,
              let event = CGEvent(scrollWheelEvent2Source: source, units: .pixel, wheelCount: 2,
                                  wheel1: Int32(pixelsY), wheel2: Int32(pixelsX), wheel3: 0)
        else { return }
        event.flags = flags
        event.post(tap: .cghidEventTap)
    }

    /// 已按下的键再次收到 down 视为自动重复（客户端转发 Windows 的按键重复）。
    func key(_ code: UInt16, down: Bool) {
        let isModifier = Self.modifierFlags[code] != nil || code == Self.capsLock
        var isRepeat = false
        if down {
            isRepeat = !pressedKeys.insert(code).inserted
            if isRepeat && isModifier {
                return
            }
        } else if pressedKeys.remove(code) == nil {
            return
        }
        if code == Self.capsLock && down {
            capsLockOn.toggle()
        }
        recomputeFlags()

        guard let event = CGEvent(keyboardEventSource: source, virtualKey: code, keyDown: down) else { return }
        if isRepeat {
            event.setIntegerValueField(.keyboardEventAutorepeat, value: 1)
        }
        var eventFlags = flags
        if isModifier {
            event.type = .flagsChanged
        } else {
            if Self.arrowKeys.contains(code) {
                eventFlags.formUnion([.maskNumericPad, .maskSecondaryFn])
            } else if Self.functionKeys.contains(code) {
                eventFlags.insert(.maskSecondaryFn)
            } else if Self.keypadKeys.contains(code) {
                eventFlags.insert(.maskNumericPad)
            }
        }
        event.flags = eventFlags
        event.post(tap: .cghidEventTap)
    }

    /// 断开或失焦时松开所有按键与鼠标键，避免卡键。
    func releaseAll() {
        for code in pressedKeys where code != Self.capsLock {
            key(code, down: false)
        }
        pressedKeys.removeAll()
        for button in pressedButtons {
            sendButton(button, down: false, at: position)
        }
        recomputeFlags()
    }

    private func sendButton(_ button: Int, down: Bool, at point: CGPoint) {
        if down {
            let now = ProcessInfo.processInfo.systemUptime
            let near = hypot(point.x - lastClickPosition.x, point.y - lastClickPosition.y) <= 4
            if button == lastClickButton && near && now - lastClickTime <= NSEvent.doubleClickInterval {
                clickCount += 1
            } else {
                clickCount = 1
            }
            lastClickTime = now
            lastClickButton = button
            lastClickPosition = point
            pressedButtons.insert(button)
        } else if pressedButtons.remove(button) == nil {
            return
        }

        let type: CGEventType
        let mouseButton: CGMouseButton
        switch button {
        case 0:
            (type, mouseButton) = (down ? .leftMouseDown : .leftMouseUp, .left)
        case 1:
            (type, mouseButton) = (down ? .rightMouseDown : .rightMouseUp, .right)
        default:
            (type, mouseButton) = (down ? .otherMouseDown : .otherMouseUp, CGMouseButton(rawValue: UInt32(button)) ?? .center)
        }
        guard let event = CGEvent(mouseEventSource: source, mouseType: type, mouseCursorPosition: point, mouseButton: mouseButton) else { return }
        event.setIntegerValueField(.mouseEventClickState, value: Int64(button == lastClickButton ? clickCount : 1))
        if button >= 2 {
            event.setIntegerValueField(.mouseEventButtonNumber, value: Int64(button))
        }
        event.flags = flags
        event.post(tap: .cghidEventTap)
        position = point
    }

    private func recomputeFlags() {
        var result: CGEventFlags = []
        for code in pressedKeys {
            if let flag = Self.modifierFlags[code] {
                result.insert(flag)
            }
        }
        if capsLockOn {
            result.insert(.maskAlphaShift)
        }
        flags = result
    }

    private func map(_ x: UInt16, _ y: UInt16) -> CGPoint {
        let bounds = CGDisplayBounds(displayID())
        return CGPoint(x: bounds.minX + Double(x) / 65535 * (bounds.width - 1),
                       y: bounds.minY + Double(y) / 65535 * (bounds.height - 1))
    }
}
