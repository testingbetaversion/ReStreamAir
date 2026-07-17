import Foundation

/// Shifts a fragmented-MP4 media segment's presentation time by rewriting
/// its `moof > traf > tfdt` baseMediaDecodeTime — the standards-correct way
/// to adjust A/V sync without re-encoding. Used to compensate for lip-sync
/// drift on sources where the audio and video tracks are already offset
/// relative to each other (a "audio delay" knob per stream, applied only to
/// the audio representation's segments).
enum AudioDelay {
    /// Reads the timescale (units-per-second) from an init segment's
    /// `moov > trak > mdia > mdhd` box. `tfdt` values in that track's media
    /// segments are expressed in this same timescale.
    static func mdhdTimescale(_ data: Data) -> UInt32? {
        let bytes = [UInt8](data)
        let boxes = parseBoxes(bytes, 0, bytes.count)
        guard let moov = findBox("moov", in: boxes),
              let trak = findBox("trak", in: moov.children),
              let mdia = findBox("mdia", in: trak.children),
              let mdhd = findBox("mdhd", in: mdia.children) else { return nil }
        let payloadStart = mdhd.payloadStart
        guard payloadStart < bytes.count else { return nil }
        let version = bytes[payloadStart]
        // version 0: [flags:3][creation:4][modification:4][timescale:4]...
        // version 1: [flags:3][creation:8][modification:8][timescale:4]...
        let timescaleOffset = payloadStart + (version == 1 ? 1 + 3 + 8 + 8 : 1 + 3 + 4 + 4)
        guard timescaleOffset + 4 <= bytes.count else { return nil }
        return readU32(bytes, timescaleOffset)
    }

    /// Reads a media segment's `moof > traf > tfdt` baseMediaDecodeTime — where
    /// this segment actually sits on the media timeline, in the track's mdhd
    /// timescale (see mdhdTimescale). This is ground truth, as opposed to what
    /// the manifest *claims* the segment timeline is: sources with SCTE-35
    /// splices routinely keep advertising a uniform segment duration while the
    /// real media timeline jumps, and only the tfdt reveals that.
    static func baseMediaDecodeTime(_ data: Data) -> UInt64? {
        let bytes = [UInt8](data)
        let boxes = parseBoxes(bytes, 0, bytes.count)
        guard let moof = findBox("moof", in: boxes),
              let traf = findBox("traf", in: moof.children),
              let tfdt = findBox("tfdt", in: traf.children) else { return nil }
        let payloadStart = tfdt.payloadStart
        guard payloadStart < bytes.count else { return nil }
        let version = bytes[payloadStart]
        let offset = payloadStart + 4
        if version == 1 {
            guard offset + 8 <= bytes.count else { return nil }
            return readU64(bytes, offset)
        }
        guard offset + 4 <= bytes.count else { return nil }
        return UInt64(readU32(bytes, offset))
    }

    /// Adds `deltaUnits` (may be negative) to the segment's tfdt
    /// baseMediaDecodeTime, clamped at zero so it can never underflow past
    /// the start of the timeline. No-op (returns the input unchanged) if no
    /// tfdt box is found or delta is zero.
    static func shiftSegment(_ data: Data, byTimescaleUnits deltaUnits: Int64) -> Data {
        guard deltaUnits != 0 else { return data }
        var bytes = [UInt8](data)
        let boxes = parseBoxes(bytes, 0, bytes.count)
        guard let moof = findBox("moof", in: boxes),
              let traf = findBox("traf", in: moof.children),
              let tfdt = findBox("tfdt", in: traf.children) else { return data }
        let payloadStart = tfdt.payloadStart
        guard payloadStart < bytes.count else { return data }
        let version = bytes[payloadStart]
        if version == 1 {
            let offset = payloadStart + 4
            guard offset + 8 <= bytes.count else { return data }
            let current = readU64(bytes, offset)
            let shifted = clampedAdd(current, deltaUnits)
            writeU64(&bytes, offset, shifted)
        } else {
            let offset = payloadStart + 4
            guard offset + 4 <= bytes.count else { return data }
            let current = UInt64(readU32(bytes, offset))
            let shifted = clampedAdd(current, deltaUnits)
            writeU32(&bytes, offset, UInt32(truncatingIfNeeded: shifted))
        }
        return Data(bytes)
    }

    private static func clampedAdd(_ base: UInt64, _ delta: Int64) -> UInt64 {
        if delta >= 0 { return base &+ UInt64(delta) }
        let magnitude = UInt64(-delta)
        return base > magnitude ? base - magnitude : 0
    }

    private static func findBox(_ type: String, in boxes: [MP4Box]) -> MP4Box? {
        boxes.first { $0.type == type }
    }
}
