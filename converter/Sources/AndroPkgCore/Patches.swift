import Foundation

/// An instruction in Android code that cannot run as-is on Darwin and is rewritten into a
/// `b <trampoline>` at conversion time (signed code cannot be patched at runtime).
public enum PatchKind: Sendable, Equatable {
    /// `svc #0` — Linux syscall trap → andropkg_svc_entry (shim/src/svc_entry.S).
    case svc
    /// `mrs Xt, tpidr_el0` — bionic thread pointer read. XNU owns TPIDR_EL0 (it stores the CPU
    /// number there on every context switch), so the TP lives in a Darwin TSD slot instead.
    case tpRead(rt: UInt32)
    /// `msr tpidr_el0, Xt` — only bionic itself does this; trap loudly.
    case tpWrite(rt: UInt32)

    static let svcWord: UInt32 = 0xD400_0001
    static let mrsTpidrEL0: UInt32 = 0xD53B_D040
    static let msrTpidrEL0: UInt32 = 0xD51B_D040

    init?(instruction w: UInt32) {
        if w == PatchKind.svcWord { self = .svc }
        else if w & ~0x1F == PatchKind.mrsTpidrEL0 { self = .tpRead(rt: w & 0x1F) }
        else if w & ~0x1F == PatchKind.msrTpidrEL0 { self = .tpWrite(rt: w & 0x1F) }
        else { return nil }
    }

    var trampolineSize: Int {
        switch self {
        case .svc: 6 * 4
        case .tpRead(let rt): rt == 31 ? 4 : 8 * 4
        case .tpWrite: 4
        }
    }

    /// Data page layout (shared by all trampolines of one image).
    static let dataSlotSvcEntry: UInt64 = 0
    static let dataSlotTSDOffset: UInt64 = 8

    /// - Parameters: t = trampoline vaddr, site = patched instruction vaddr, dataBase = __APK_DATA vaddr.
    func trampolineBytes(at t: UInt64, site: UInt64, dataBase: UInt64) throws -> [UInt8] {
        var w: [UInt32] = []
        switch self {
        case .svc:
            let slot = dataBase + PatchKind.dataSlotSvcEntry
            w.append(0xA9BF_7BF0)                                         // stp x16, x30, [sp, #-16]!
            w.append(try AArch64.adrp(rd: 16, from: t + 4, to: slot))     // adrp x16, data
            w.append(AArch64.ldrX(rt: 16, rn: 16, offset: UInt32(slot & 0xFFF)))
            w.append(0xD63F_0200)                                         // blr x16
            w.append(0xA8C1_7BF0)                                         // ldp x16, x30, [sp], #16
            w.append(try AArch64.b(from: t + 20, to: site + 4))

        case .tpRead(let rt) where rt == 31:
            w.append(try AArch64.b(from: t, to: site + 4))                // mrs xzr: no-op

        case .tpRead(let rt):
            // Xt = ((void **)(tpidrro_el0 & ~7))[tsd_key]   — clobbers nothing but Xt.
            let s: UInt32 = rt == 16 ? 17 : 16                             // scratch
            let slot = dataBase + PatchKind.dataSlotTSDOffset
            w.append(0xF81F_0FE0 | s)                                     // str xS, [sp, #-16]!
            w.append(0xD53B_D060 | rt)                                    // mrs xT, tpidrro_el0
            w.append(0x927D_F000 | rt << 5 | rt)                          // and xT, xT, #~7
            w.append(try AArch64.adrp(rd: s, from: t + 12, to: slot))     // adrp xS, data
            w.append(AArch64.ldrX(rt: s, rn: s, offset: UInt32(slot & 0xFFF)))  // xS = key * 8
            w.append(0xF860_6800 | s << 16 | rt << 5 | rt)                // ldr xT, [xT, xS]
            w.append(0xF841_07E0 | s)                                     // ldr xS, [sp], #16
            w.append(try AArch64.b(from: t + 28, to: site + 4))

        case .tpWrite:
            w.append(0xD420_F4A0)                                         // brk #0x7a5 ("APK")
        }
        precondition(w.count * 4 == trampolineSize)
        var out = ByteWriter()
        w.forEach { out.u32($0) }
        return out.bytes
    }
}

public struct PatchSite: Sendable, Equatable, CustomStringConvertible {
    public let vaddr: UInt64
    public let kind: PatchKind

    public var description: String {
        let a = "0x" + String(vaddr, radix: 16)
        switch kind {
        case .svc: return "svc@\(a)"
        case .tpRead(let rt): return "mrs-tp(x\(rt))@\(a)"
        case .tpWrite(let rt): return "msr-tp(x\(rt))@\(a)"
        }
    }
}
