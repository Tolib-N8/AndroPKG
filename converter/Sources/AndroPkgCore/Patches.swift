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
    /// The first instruction of an exported function andropkg replaces (`hookedFunctions`):
    /// a jump to the replacement the loader puts in data slot `dataSlotHooks + 8 * index`.
    /// Calls inside the image are direct `bl`s, which no relocation can redirect.
    case hook(index: UInt32)
    /// `mrs Xt, ctr_el0` — the cache type register, which Darwin does not let user code read
    /// (SIGILL). compiler-rt's __clear_cache reads it. Xt gets a constant instead: 64-byte
    /// lines, and DIC/IDC set (no cache maintenance needed): nothing writes code at runtime here.
    case ctrRead(rt: UInt32)

    /// Functions replaced in every image that defines them, in data slot order (the loader,
    /// loader/src/elf_loader.c, fills the slots in the same order).
    /// - libffi's closure allocator (JNA's libjnidispatch, ...): Linux libffi maps writable
    ///   memory executable for its trampolines, which iOS forbids; andropkg hands out closures
    ///   backed by pre-signed trampolines instead (ndk-libs/ffi_closures.c).
    public static let hookedFunctions = ["ffi_closure_alloc", "ffi_closure_free"]

    static let svcWord: UInt32 = 0xD400_0001
    static let mrsTpidrEL0: UInt32 = 0xD53B_D040
    static let msrTpidrEL0: UInt32 = 0xD51B_D040
    static let mrsCtrEL0: UInt32 = 0xD53B_0020
    /// RES1, CWG = ERG = DminLine = IminLine = 64 bytes, PIPT icache, IDC, DIC.
    static let ctrValue: UInt32 = 0xB444_C004

    init?(instruction w: UInt32) {
        if w == PatchKind.svcWord { self = .svc }
        else if w & ~0x1F == PatchKind.mrsTpidrEL0 { self = .tpRead(rt: w & 0x1F) }
        else if w & ~0x1F == PatchKind.msrTpidrEL0 { self = .tpWrite(rt: w & 0x1F) }
        else if w & ~0x1F == PatchKind.mrsCtrEL0 { self = .ctrRead(rt: w & 0x1F) }
        else { return nil }
    }

    var trampolineSize: Int {
        switch self {
        case .svc: 6 * 4
        case .tpRead(let rt): rt == 31 ? 4 : 15 * 4
        case .tpWrite: 4
        case .hook: 3 * 4
        case .ctrRead: 3 * 4
        }
    }

    /// Data page layout (shared by all trampolines of one image).
    static let dataSlotSvcEntry: UInt64 = 0
    static let dataSlotTSDOffset: UInt64 = 8
    /// andropkg_tp_slowpath: attaches a bionic TCB to a thread that has none yet (any thread
    /// can call into Android code: Java threads, Core Audio, GCD) and returns it in x16.
    static let dataSlotTPSlowPath: UInt64 = 16
    /// Replacements of `hookedFunctions`, one slot each.
    static let dataSlotHooks: UInt64 = 24

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
            // A thread without a TCB yet (NULL) takes the slow path, which creates one.
            let s: UInt32 = rt == 16 ? 17 : 16                             // scratch
            let slot = dataBase + PatchKind.dataSlotTSDOffset
            let slow = dataBase + PatchKind.dataSlotTPSlowPath
            w.append(0xF81F_0FE0 | s)                                     // 0  str xS, [sp, #-16]!
            w.append(0xD53B_D060 | rt)                                    // 1  mrs xT, tpidrro_el0
            w.append(0x927D_F000 | rt << 5 | rt)                          // 2  and xT, xT, #~7
            w.append(try AArch64.adrp(rd: s, from: t + 12, to: slot))     // 3  adrp xS, data
            w.append(AArch64.ldrX(rt: s, rn: s, offset: UInt32(slot & 0xFFF)))  // 4  xS = key * 8
            w.append(0xF860_6800 | s << 16 | rt << 5 | rt)                // 5  ldr xT, [xT, xS]
            w.append(0xB500_0000 | (7 << 5) | rt)                         // 6  cbnz xT, 13
            w.append(0xA9BF_7BF0)                                         // 7  stp x16, x30, [sp, #-16]!
            w.append(try AArch64.adrp(rd: 16, from: t + 32, to: slow))    // 8  adrp x16, data
            w.append(AArch64.ldrX(rt: 16, rn: 16, offset: UInt32(slow & 0xFFF)))  // 9
            w.append(0xD63F_0200)                                         // 10 blr x16  (TP -> x16)
            switch rt {                                                   // 11-12 TP into xT
            case 16:
                w.append(0xF94007FE)                                      //    ldr x30, [sp, #8]
                w.append(0x910043FF)                                      //    add sp, sp, #16
            case 30:
                w.append(0xAA1003FE)                                      //    mov x30, x16
                w.append(0xF84107F0)                                      //    ldr x16, [sp], #16
            default:
                w.append(0xAA1003E0 | rt)                                 //    mov xT, x16
                w.append(0xA8C17BF0)                                      //    ldp x16, x30, [sp], #16
            }
            w.append(0xF841_07E0 | s)                                     // 13 ldr xS, [sp], #16
            w.append(try AArch64.b(from: t + 56, to: site + 4))           // 14

        case .tpWrite:
            w.append(0xD420_F4A0)                                         // brk #0x7a5 ("APK")

        case .hook(let index):
            // At a function's entry x16 (IP0) is free: jump to the replacement.
            let slot = dataBase + PatchKind.dataSlotHooks + 8 * UInt64(index)
            w.append(try AArch64.adrp(rd: 16, from: t, to: slot))         // adrp x16, data
            w.append(AArch64.ldrX(rt: 16, rn: 16, offset: UInt32(slot & 0xFFF)))
            w.append(0xD61F_0200)                                         // br x16

        case .ctrRead(let rt):
            if rt == 31 {
                w.append(0xD503_201F)                                     // nop (mrs xzr)
                w.append(0xD503_201F)
            } else {
                w.append(0xD280_0000 | (PatchKind.ctrValue & 0xFFFF) << 5 | rt)   // movz xT, #lo
                w.append(0xF2A0_0000 | (PatchKind.ctrValue >> 16) << 5 | rt)      // movk xT, #hi, lsl 16
            }
            w.append(try AArch64.b(from: t + 8, to: site + 4))
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
        case .hook(let i): return "hook(\(PatchKind.hookedFunctions[Int(i)]))@\(a)"
        case .ctrRead(let rt): return "mrs-ctr(x\(rt))@\(a)"
        }
    }
}
