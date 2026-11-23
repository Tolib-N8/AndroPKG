import Foundation
import CryptoKit

/// Wraps an Android AArch64 ELF shared object into a Mach-O dylib that iOS dyld can map
/// and that can be code-signed, *without* relinking the machine code.
///
/// Layout (Mach-O vmaddr space):
///
///     0x0000  __TEXT      r-x  Mach-O header + load commands + __elfwrap info section
///     0x4000  __ELF_0..N  ...  ELF PT_LOAD segments at  0x4000 + p_vaddr  (relative layout
///                              preserved, so ADRP/GOT/PC-relative code keeps working)
///     ....    __APK_TRAMP r-x  one trampoline per rewritten instruction (svc #0, mrs/msr tpidr_el0)
///     ....    __APK_DATA  rw-  [0] svc dispatcher, [1] TSD offset of the bionic TP,
///                              [2] TP slow path (all set by the loader)
///     ....    __LINKEDIT  r--  (codesign appends the signature here)
///
/// dyld only maps the segments and validates the signature; there are no Mach-O fixups or
/// exports. The andropkg ELF loader (loader/) then applies ELF relocations, resolves symbols
/// against the shim libraries, and runs init_array.
public enum ElfWrapper {
    public enum Platform: String, Sendable, CaseIterable {
        case iOS = "ios", macOS = "macos", iOSSimulator = "ios-sim"

        var machOPlatform: UInt32 {
            switch self { case .macOS: 1; case .iOS: 2; case .iOSSimulator: 7 }
        }
        var minOS: UInt32 {
            switch self { case .macOS: version(14, 0); case .iOS, .iOSSimulator: version(17, 0) }
        }
    }

    public struct Options: Sendable {
        public var platform: Platform
        public var installName: String
        public var patchSyscalls: Bool

        public init(platform: Platform, installName: String, patchSyscalls: Bool = true) {
            self.platform = platform
            self.installName = installName
            self.patchSyscalls = patchSyscalls
        }
    }

    public struct Result: Sendable {
        public let machO: [UInt8]
        /// Rewritten instructions (svc #0, mrs/msr tpidr_el0).
        public let patches: [PatchSite]
        /// Human-readable segment map, for diagnostics.
        public let segmentMap: [String]
    }

    static let pageSize: UInt64 = 0x4000          // iOS/arm64 page size
    static let elfBaseVM: UInt64 = 0x4000          // ELF vaddr 0 lives here
    static let infoFileOffset = 0x2000             // __TEXT,__elfwrap

    // VM_PROT_*
    static let protR: UInt32 = 1, protW: UInt32 = 2, protX: UInt32 = 4

    struct Segment {
        var name: String
        var vmStart: UInt64   // ELF vaddr space, page aligned
        var vmEnd: UInt64     // page aligned
        var content: [UInt8]  // file-backed bytes starting at vmStart
        var prot: UInt32
    }

    public static func wrap(_ elf: ElfFile, options: Options) throws -> Result {
        let loads = elf.loadSegments
        guard !loads.isEmpty else { throw ConverterError.layout("no PT_LOAD segments") }

        // 1. Rebuild each PT_LOAD exactly as the Linux kernel / bionic linker would map it
        //    (an ELF linked for 4 KB pages: code and data apart, see Layout4K).
        var segments: [Segment] = []
        var split: UInt64 = 0, shift: UInt64 = 0, moved = 0
        if Layout4K.needed(loads) {
            let l = try Layout4K.layout(elf)
            segments = l.segments
            split = l.split
            shift = l.shift
            moved = l.rewritten
        }
        for (i, ph) in loads.enumerated() where shift == 0 && split == 0 {
            if ph.writable && ph.executable {
                throw ConverterError.unsupported("segment \(i) is W+X; iOS forbids writable code")
            }
            let vmStart = alignDown(ph.vaddr, pageSize)
            let vmEnd = alignUp(ph.vaddr + ph.memsz, pageSize)
            if let prev = segments.last, vmStart < prev.vmEnd {
                throw ConverterError.unsupported(
                    "PT_LOAD \(i) shares a 16KB page with the previous segment " +
                    "(ELF built with 4KB max-page-size); relink with -z max-page-size=16384")
            }
            let delta = Int(ph.vaddr - vmStart)
            let fileBacked = delta + Int(ph.filesz)
            var content = [UInt8](repeating: 0, count: fileBacked)
            let src = Int(ph.offset) - delta
            for j in 0..<fileBacked where src + j >= 0 && src + j < elf.bytes.count {
                content[j] = elf.bytes[src + j]
            }
            var prot: UInt32 = 0
            if ph.readable { prot |= protR }
            if ph.writable { prot |= protW }
            if ph.executable { prot |= protX }
            segments.append(Segment(name: "__ELF_\(i)", vmStart: vmStart, vmEnd: vmEnd,
                                    content: content, prot: prot))
        }
        let elfVMEnd = segments.last!.vmEnd

        // 2. Find and rewrite instructions iOS cannot execute as-is (before signing — code is
        //    immutable afterwards). Each site becomes `b <trampoline>`.
        let sites = options.patchSyscalls ? findPatchSites(elf: elf, segments: segments) : []
        let trampBase = elfVMEnd
        var tramp = ByteWriter()
        for site in sites {
            let t = trampBase + UInt64(tramp.count)
            try patchWord(&segments, at: site.vaddr, AArch64.b(from: site.vaddr, to: t))
            tramp.append([UInt8](repeating: 0, count: site.kind.trampolineSize))  // sizing pass
        }
        // Data page goes right after the (page-rounded) trampolines; re-emit now that its address is fixed.
        let trampSize = alignUp(UInt64(tramp.count), pageSize)
        let dataBase = trampBase + trampSize
        if !sites.isEmpty {
            tramp = ByteWriter()
            for site in sites {
                let t = trampBase + UInt64(tramp.count)
                tramp.append(try site.kind.trampolineBytes(at: t, site: site.vaddr, dataBase: dataBase))
            }
            precondition(alignUp(UInt64(tramp.count), pageSize) == trampSize)
            segments.append(Segment(name: "__APK_TRAMP", vmStart: trampBase, vmEnd: dataBase,
                                    content: tramp.bytes, prot: protR | protX))
            segments.append(Segment(name: "__APK_DATA", vmStart: dataBase, vmEnd: dataBase + pageSize,
                                    content: [UInt8](repeating: 0, count: 16), prot: protR | protW))
        }

        // 3. Emit the Mach-O.
        func count(_ k: (PatchKind) -> Bool) -> UInt32 { UInt32(sites.filter { k($0.kind) }.count) }
        let info = WrapInfo(elfVMOffset: elfBaseVM, elfVMSize: elfVMEnd,
                            trampVMOffset: sites.isEmpty ? 0 : elfBaseVM + trampBase,
                            dataVMOffset: sites.isEmpty ? 0 : elfBaseVM + dataBase,
                            svcCount: count { if case .svc = $0 { true } else { false } },
                            tpReadCount: count { if case .tpRead = $0 { true } else { false } },
                            tpWriteCount: count { if case .tpWrite = $0 { true } else { false } },
                            splitVaddr: split, shift: shift)
        let uuid = Array(Insecure.MD5.hash(data: elf.bytes))
        let machO = try MachOWriter.write(segments: segments, info: info, options: options, uuid: uuid)

        var map = segments.map { s in
            String(format: "%-12@ vm 0x%08llx-0x%08llx %@%@%@ file 0x%llx",
                   s.name as NSString, elfBaseVM + s.vmStart, elfBaseVM + s.vmEnd,
                   (s.prot & protR != 0 ? "r" : "-") as NSString,
                   (s.prot & protW != 0 ? "w" : "-") as NSString,
                   (s.prot & protX != 0 ? "x" : "-") as NSString, UInt64(s.content.count))
        }
        if shift > 0 {
            map.append(String(format: "4KB-page ELF: data from 0x%llx moved by 0x%llx, %d references rewritten",
                              split, shift, moved))
        }
        return Result(machO: machO, patches: sites, segmentMap: map)
    }

    /// Scans executable code for instructions that need rewriting. Uses SHF_EXECINSTR sections
    /// when present (avoids matching data embedded in executable segments), else whole
    /// executable segments.
    static func findPatchSites(elf: ElfFile, segments: [Segment]) -> [PatchSite] {
        var ranges: [Range<UInt64>] = elf.sections
            .filter { $0.isExecInstr && $0.type != 8 /* SHT_NOBITS */ && $0.size > 0 }
            .map { $0.addr..<($0.addr + $0.size) }
        if ranges.isEmpty {
            ranges = elf.loadSegments.filter(\.executable).map { $0.vaddr..<($0.vaddr + $0.filesz) }
        }
        var sites: [PatchSite] = []
        for r in ranges {
            var v = alignUp(r.lowerBound, 4)
            while v + 4 <= r.upperBound {
                if let w = readWord(segments, at: v), let k = PatchKind(instruction: w) {
                    sites.append(PatchSite(vaddr: v, kind: k))
                }
                v += 4
            }
        }
        // Functions andropkg replaces (PatchKind.hookedFunctions), where the image defines them.
        for (i, name) in PatchKind.hookedFunctions.enumerated() {
            guard let v = definedFunction(elf: elf, name: name),
                  ranges.contains(where: { $0.contains(v) }) else { continue }
            sites.removeAll { $0.vaddr == v }
            sites.append(PatchSite(vaddr: v, kind: .hook(index: UInt32(i))))
        }
        return sites.sorted { $0.vaddr < $1.vaddr }
    }

    /// The address of a function the image defines and exports (.dynsym), if any.
    static func definedFunction(elf: ElfFile, name: String) -> UInt64? {
        guard let symtab = elf.sections.first(where: { $0.name == ".dynsym" }),
              let strtab = elf.sections.first(where: { $0.name == ".dynstr" }) else { return nil }
        let b = elf.bytes
        func u16(_ o: Int) -> UInt16 { UInt16(b[o]) | UInt16(b[o + 1]) << 8 }
        func u32(_ o: Int) -> UInt32 { (0..<4).reduce(0) { $0 | UInt32(b[o + $1]) << (8 * UInt32($1)) } }
        func u64(_ o: Int) -> UInt64 { (0..<8).reduce(0) { $0 | UInt64(b[o + $1]) << (8 * UInt64($1)) } }
        let target = Array(name.utf8)
        var o = Int(symtab.offset)
        let end = Int(symtab.offset + symtab.size)
        while o + 24 <= end, o + 24 <= b.count {
            let nameOff = Int(strtab.offset) + Int(u32(o))
            let info = b[o + 4], shndx = u16(o + 6), value = u64(o + 8)
            if info & 0xF == 2 /* STT_FUNC */, shndx != 0, value != 0, nameOff + target.count < b.count,
               Array(b[nameOff..<(nameOff + target.count)]) == target, b[nameOff + target.count] == 0 {
                return value
            }
            o += 24
        }
        return nil
    }

    static func readWord(_ segments: [Segment], at v: UInt64) -> UInt32? {
        guard let s = segments.first(where: { v >= $0.vmStart && v + 4 <= $0.vmStart + UInt64($0.content.count) })
        else { return nil }
        let o = Int(v - s.vmStart)
        return UInt32(s.content[o]) | UInt32(s.content[o + 1]) << 8 |
               UInt32(s.content[o + 2]) << 16 | UInt32(s.content[o + 3]) << 24
    }

    static func patchWord(_ segments: inout [Segment], at v: UInt64, _ word: UInt32) throws {
        guard let i = segments.firstIndex(where: { v >= $0.vmStart && v + 4 <= $0.vmStart + UInt64($0.content.count) })
        else { throw ConverterError.layout("patch address 0x\(String(v, radix: 16)) not file-backed") }
        let o = Int(v - segments[i].vmStart)
        for k in 0..<4 { segments[i].content[o + k] = UInt8(truncatingIfNeeded: word >> (8 * UInt32(k))) }
    }
}

/// Mirrors `apk_wrap_info` in loader/include/andropkg/loader.h.
struct WrapInfo {
    static let magic = Array("APKELF01".utf8)

    var elfVMOffset: UInt64
    var elfVMSize: UInt64
    var trampVMOffset: UInt64
    var dataVMOffset: UInt64      // [0] svc dispatcher, [1] TSD byte offset of the bionic TP
    var svcCount: UInt32
    var tpReadCount: UInt32
    var tpWriteCount: UInt32
    var splitVaddr: UInt64 = 0    // version 3: ELF vaddrs >= splitVaddr were moved up by shift
    var shift: UInt64 = 0

    var version: UInt32 { shift > 0 ? 3 : 2 }
    var size: Int { shift > 0 ? 80 : 64 }

    var bytes: [UInt8] {
        var w = ByteWriter()
        w.append(WrapInfo.magic)
        w.u32(version)
        w.u32(0)            // flags
        w.u64(elfVMOffset)
        w.u64(elfVMSize)
        w.u64(trampVMOffset)
        w.u64(dataVMOffset)
        w.u32(svcCount)
        w.u32(tpReadCount)
        w.u32(tpWriteCount)
        w.u32(0)
        if shift > 0 {
            w.u64(splitVaddr)
            w.u64(shift)
        }
        assert(w.count == size)
        return w.bytes
    }
}

enum AArch64 {
    static func b(from pc: UInt64, to target: UInt64) throws -> UInt32 {
        let delta = Int64(bitPattern: target &- pc)
        guard delta % 4 == 0, delta >= -(1 << 27), delta < (1 << 27) else {
            throw ConverterError.layout("branch out of range: 0x\(String(pc, radix: 16)) -> 0x\(String(target, radix: 16))")
        }
        return 0x1400_0000 | (UInt32(truncatingIfNeeded: delta >> 2) & 0x03FF_FFFF)
    }

    static func adrp(rd: UInt32, from pc: UInt64, to target: UInt64) throws -> UInt32 {
        let pages = Int64(bitPattern: (target >> 12) &- (pc >> 12))
        guard pages >= -(1 << 20), pages < (1 << 20) else { throw ConverterError.layout("adrp out of range") }
        let imm = UInt32(truncatingIfNeeded: pages) & 0x1F_FFFF
        return 0x9000_0000 | (imm & 3) << 29 | (imm >> 2) << 5 | rd
    }

    /// ldr Xt, [Xn, #offset] (unsigned, 8-byte scaled)
    static func ldrX(rt: UInt32, rn: UInt32, offset: UInt32) -> UInt32 {
        precondition(offset % 8 == 0 && offset < 32768)
        return 0xF940_0000 | (offset / 8) << 10 | rn << 5 | rt
    }
}

func version(_ major: UInt32, _ minor: UInt32, _ patch: UInt32 = 0) -> UInt32 {
    major << 16 | minor << 8 | patch
}
