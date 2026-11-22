import Foundation

/// ELF shared objects linked for 4 KB pages (`-z max-page-size=4096`, older NDKs) put the end of
/// the code and the start of the data in one 16 KB page, which iOS can map only one way (r-x or
/// rw-). They are laid out differently:
///
///   * every non-writable PT_LOAD (rodata, eh_frame, text) becomes one r-x segment, unmoved;
///   * every writable PT_LOAD (relro, data, bss) becomes one rw- segment, moved up by `shift`
///     (a multiple of 4 KB) onto 16 KB pages of its own.
///
/// Everything that reaches the moved data relative to the code is then rewritten:
///   * ADRP / ADR / LDR (literal) instructions in executable sections;
///   * PC-relative pointers in .eh_frame (CIE personality) and in the LSDA type tables of
///     .gcc_except_table (C++ exception `catch` types), which point into the data.
/// Absolute pointers are relocations; the loader adds `shift` to any ELF vaddr >= `split`
/// (apk_wrap_info version 3).
enum Layout4K {
    struct Result {
        var segments: [ElfWrapper.Segment]
        var split: UInt64
        var shift: UInt64
        var rewritten: Int
    }

    static let smallPage: UInt64 = 0x1000

    static func needed(_ loads: [ElfFile.ProgramHeader]) -> Bool {
        var prevEnd: UInt64 = 0
        for (i, ph) in loads.enumerated() {
            let start = alignDown(ph.vaddr, ElfWrapper.pageSize)
            if i > 0 && start < prevEnd { return true }
            prevEnd = alignUp(ph.vaddr + ph.memsz, ElfWrapper.pageSize)
        }
        return false
    }

    static func layout(_ elf: ElfFile) throws -> Result {
        let loads = elf.loadSegments
        guard let firstW = loads.firstIndex(where: \.writable), firstW > 0 else {
            throw ConverterError.unsupported("4KB-page ELF without a code segment before its data")
        }
        let ro = Array(loads[..<firstW]), rw = Array(loads[firstW...])
        guard rw.allSatisfy(\.writable) else {
            throw ConverterError.unsupported("4KB-page ELF with code after its data")
        }
        if let i = loads.firstIndex(where: { $0.writable && $0.executable }) {
            throw ConverterError.unsupported("segment \(i) is W+X; iOS forbids writable code")
        }
        let roEnd = ro.map { $0.vaddr + $0.memsz }.max()!
        let split = alignDown(rw[0].vaddr, smallPage)
        guard roEnd <= split else {
            throw ConverterError.unsupported("code and data share a 4 KB page (no separation possible)")
        }
        let roVM = alignDown(ro[0].vaddr, ElfWrapper.pageSize)..<alignUp(roEnd, ElfWrapper.pageSize)
        // The smallest move (a multiple of 4 KB: ADRP pages, 4-byte LDR offsets) that puts the
        // data on the next 16 KB page: PC-relative reaches (ADR: +-1 MB) stay as short as possible.
        let shift = roVM.upperBound > split ? roVM.upperBound - split : 0
        let rwStart = alignDown(split + shift, ElfWrapper.pageSize)
        let rwEnd = alignUp(rw.map { $0.vaddr + $0.memsz }.max()! + shift, ElfWrapper.pageSize)

        var code = segment(elf, ro, name: "__ELF_0", vmStart: roVM.lowerBound, vmEnd: roVM.upperBound,
                           shift: 0, prot: ElfWrapper.protR | (ro.contains(where: \.executable) ? ElfWrapper.protX : 0))
        let data = segment(elf, rw, name: "__ELF_1", vmStart: rwStart, vmEnd: rwEnd,
                           shift: shift, prot: ElfWrapper.protR | ElfWrapper.protW)

        var rewritten = 0
        if shift > 0 {
            var patcher = Patcher(segment: code, split: split, shift: shift)
            // Instructions.
            var ranges: [Range<UInt64>] = elf.sections
                .filter { $0.isExecInstr && $0.type != 8 && $0.size > 0 }
                .map { $0.addr..<($0.addr + $0.size) }
            if ranges.isEmpty {
                ranges = ro.filter(\.executable).map { $0.vaddr..<($0.vaddr + $0.filesz) }
            }
            for r in ranges {
                var pc = alignUp(r.lowerBound, 4)
                while pc + 4 <= r.upperBound {
                    try patcher.instruction(at: pc)
                    pc += 4
                }
            }
            // Exception tables.
            if let eh = elf.sections.first(where: { $0.name == ".eh_frame" && $0.type != 8 }) {
                try patcher.ehFrame(eh.addr..<(eh.addr + eh.size))
            }
            code = patcher.segment
            rewritten = patcher.count
        }
        return Result(segments: [code, data], split: split, shift: shift, rewritten: rewritten)
    }

    /// One segment holding several PT_LOADs (each at vaddr + shift), zero between them.
    static func segment(_ elf: ElfFile, _ phs: [ElfFile.ProgramHeader], name: String, vmStart: UInt64,
                        vmEnd: UInt64, shift: UInt64, prot: UInt32) -> ElfWrapper.Segment {
        let fileEnd = phs.map { $0.vaddr + shift + $0.filesz }.max()! - vmStart
        var content = [UInt8](repeating: 0, count: Int(fileEnd))
        for ph in phs where ph.filesz > 0 {
            let dst = Int(ph.vaddr + shift - vmStart), src = Int(ph.offset)
            let n = min(Int(ph.filesz), elf.bytes.count - src)
            if n > 0 { content.replaceSubrange(dst..<(dst + n), with: elf.bytes[src..<(src + n)]) }
        }
        return ElfWrapper.Segment(name: name, vmStart: vmStart, vmEnd: vmEnd, content: content, prot: prot)
    }

    struct Patcher {
        var segment: ElfWrapper.Segment
        let split: UInt64
        let shift: UInt64
        var count = 0
        var patched = Set<UInt64>()

        // MARK: memory

        func has(_ v: UInt64, _ n: Int) -> Bool {
            v >= segment.vmStart && v + UInt64(n) <= segment.vmStart + UInt64(segment.content.count)
        }
        func u8(_ v: UInt64) throws -> UInt8 {
            guard has(v, 1) else { throw ConverterError.layout("read outside code at 0x\(String(v, radix: 16))") }
            return segment.content[Int(v - segment.vmStart)]
        }
        func read(_ v: UInt64, _ n: Int) throws -> UInt64 {
            var r: UInt64 = 0
            for i in 0..<n { r |= UInt64(try u8(v + UInt64(i))) << (8 * UInt64(i)) }
            return r
        }
        mutating func write(_ v: UInt64, _ n: Int, _ value: UInt64) {
            for i in 0..<n {
                segment.content[Int(v - segment.vmStart) + i] = UInt8(truncatingIfNeeded: value >> (8 * UInt64(i)))
            }
        }
        func uleb(_ p: inout UInt64) throws -> UInt64 {
            var r: UInt64 = 0, s: UInt64 = 0
            while true {
                let b = try u8(p); p += 1
                if s < 64 { r |= UInt64(b & 0x7f) << s }
                s += 7
                if b & 0x80 == 0 { return r }
            }
        }
        func sleb(_ p: inout UInt64) throws -> Int64 {
            var r: Int64 = 0, s: Int64 = 0, b: UInt8 = 0
            repeat {
                b = try u8(p); p += 1
                if s < 64 { r |= Int64(b & 0x7f) << s }
                s += 7
            } while b & 0x80 != 0
            if s < 64 && b & 0x40 != 0 { r |= -(Int64(1) << s) }
            return r
        }

        // MARK: instructions

        static func sext(_ v: UInt64, _ bits: Int) -> Int64 {
            let s = 64 - bits
            return Int64(bitPattern: v << UInt64(s)) >> Int64(s)
        }

        mutating func instruction(at pc: UInt64) throws {
            let w = UInt32(try read(pc, 4))
            var out: UInt32? = nil
            if w & 0x9F00_0000 == 0x9000_0000 || w & 0x9F00_0000 == 0x1000_0000 {  // ADRP / ADR
                let isPage = w & 0x8000_0000 != 0
                let imm = Self.sext(UInt64((w >> 5) & 0x7FFFF) << 2 | UInt64((w >> 29) & 3), 21)
                let target = isPage ? (pc & ~0xFFF) &+ UInt64(bitPattern: imm << 12) : pc &+ UInt64(bitPattern: imm)
                if target >= (isPage ? alignDown(split, Layout4K.smallPage) : split) {
                    let n = imm + Int64(isPage ? shift >> 12 : shift)
                    guard n >= -(1 << 20) && n < (1 << 20) else {
                        throw ConverterError.layout("ADR(P) at 0x\(String(pc, radix: 16)) out of range after moving the data")
                    }
                    let u = UInt32(truncatingIfNeeded: n) & 0x1F_FFFF
                    out = (w & 0x9F00_001F) | (u & 3) << 29 | (u >> 2) << 5
                }
            } else if w & 0x3B00_0000 == 0x1800_0000 {  // LDR/LDRSW/PRFM (literal)
                let off = Self.sext(UInt64((w >> 5) & 0x7FFFF), 19) * 4
                if pc &+ UInt64(bitPattern: off) >= split {
                    let n = off + Int64(shift)
                    guard n >= -(1 << 20) && n < (1 << 20) else {
                        throw ConverterError.layout("LDR literal at 0x\(String(pc, radix: 16)) out of range after moving the data")
                    }
                    out = (w & 0xFF00_001F) | (UInt32(truncatingIfNeeded: n / 4) & 0x7FFFF) << 5
                }
            }
            if let o = out {
                write(pc, 4, UInt64(o))
                count += 1
            }
        }

        // MARK: DWARF pointers

        /// Size of a fixed-size DW_EH_PE value; nil for LEB128 forms.
        static func size(_ enc: UInt8) -> Int? {
            switch enc & 0x0F {
            case 0x00: 8
            case 0x02, 0x0A: 2
            case 0x03, 0x0B: 4
            case 0x04, 0x0C: 8
            default: nil
            }
        }

        /// Reads an encoded pointer at p (advancing it); returns its target if it's PC-relative.
        func pointer(_ p: inout UInt64, _ enc: UInt8) throws -> (field: UInt64, value: Int64, target: UInt64?) {
            let field = p
            var value: Int64
            if let n = Self.size(enc) {
                let raw = try read(p, n)
                p += UInt64(n)
                value = enc & 0x08 != 0 ? Self.sext(raw, n * 8) : Int64(bitPattern: raw)
            } else if enc & 0x0F == 0x01 {
                value = Int64(bitPattern: try uleb(&p))
            } else {
                value = try sleb(&p)
            }
            let target = enc & 0x70 == 0x10 ? field &+ UInt64(bitPattern: value) : nil
            return (field, value, target)
        }

        /// Moves a PC-relative pointer that reaches the data.
        mutating func fix(_ field: UInt64, _ value: Int64, _ target: UInt64?, _ enc: UInt8) throws {
            guard let t = target, t >= split, field < split, !patched.contains(field),
                  let n = Self.size(enc) else { return }
            let v = value + Int64(shift)
            if n < 8 {
                let lim = Int64(1) << (n * 8 - 1)
                guard (enc & 0x08 != 0 && v >= -lim && v < lim) || (enc & 0x08 == 0 && v >= 0 && v < 2 * lim) else {
                    throw ConverterError.layout("unwind pointer at 0x\(String(field, radix: 16)) out of range")
                }
            }
            write(field, n, UInt64(bitPattern: v))
            patched.insert(field)
            count += 1
        }

        mutating func ehFrame(_ range: Range<UInt64>) throws {
            struct CIE { var aug: String; var fdeEnc: UInt8; var lsdaEnc: UInt8 }
            var cies: [UInt64: CIE] = [:]
            var lsdas = Set<UInt64>()
            var p = range.lowerBound
            while p + 4 <= range.upperBound {
                let recPos = p
                var len = try read(p, 4); p += 4
                if len == 0 { continue }  // (zero terminator; keep scanning if more follows)
                if len == 0xFFFF_FFFF { len = try read(p, 8); p += 8 }
                let body = p, end = body + len
                guard end <= range.upperBound else { break }
                let id = UInt32(try read(body, 4))
                var q = body + 4
                if id == 0 {  // CIE
                    let version = try u8(q); q += 1
                    var aug = ""
                    while true { let c = try u8(q); q += 1; if c == 0 { break }; aug.append(Character(UnicodeScalar(c))) }
                    if aug.contains("eh") { q += 8 }
                    _ = try uleb(&q); _ = try sleb(&q)
                    if version == 1 { q += 1 } else { _ = try uleb(&q) }
                    var cie = CIE(aug: aug, fdeEnc: 0, lsdaEnc: 0xFF)
                    if aug.hasPrefix("z") {
                        _ = try uleb(&q)
                        for c in aug.dropFirst() {
                            switch c {
                            case "P":
                                let enc = try u8(q); q += 1
                                let ptr = try pointer(&q, enc & 0x7F)
                                try fix(ptr.field, ptr.value, ptr.target, enc & 0x7F)
                            case "L": cie.lsdaEnc = try u8(q); q += 1
                            case "R": cie.fdeEnc = try u8(q); q += 1
                            default: break
                            }
                        }
                    }
                    cies[recPos] = cie
                } else if let cie = cies[body - UInt64(id)] {  // FDE
                    _ = try pointer(&q, cie.fdeEnc)            // pc_begin
                    _ = try pointer(&q, cie.fdeEnc & 0x0F)     // pc_range
                    if cie.aug.hasPrefix("z") {
                        _ = try uleb(&q)
                        if cie.aug.contains("L") && cie.lsdaEnc != 0xFF {
                            let l = try pointer(&q, cie.lsdaEnc & 0x7F)
                            if let t = l.target, t < split, l.value != 0 { lsdas.insert(t) }
                        }
                    }
                }
                p = end
            }
            for l in lsdas.sorted() { try lsda(l) }
        }

        /// The C++ LSDA: its type table entries (`catch` types) point at typeinfo, in the data.
        mutating func lsda(_ start: UInt64) throws {
            var p = start
            let lpEnc = try u8(p); p += 1
            if lpEnc != 0xFF { _ = try pointer(&p, lpEnc & 0x7F) }
            let ttEnc = try u8(p); p += 1
            var ttBase: UInt64? = nil
            if ttEnc != 0xFF { let off = try uleb(&p); ttBase = p + off }
            let csEnc = try u8(p); p += 1
            let csLen = try uleb(&p)
            let csEnd = p + csLen
            var actions = Set<UInt64>()
            while p < csEnd {
                _ = try pointer(&p, csEnc); _ = try pointer(&p, csEnc); _ = try pointer(&p, csEnc)
                let a = try uleb(&p)
                if a > 0 { actions.insert(a - 1) }
            }
            guard let base = ttBase, let size = Self.size(ttEnc & 0x7F) else { return }
            var maxFilter: Int64 = 0
            for a in actions {
                var q = csEnd + a
                for _ in 0..<1000 {  // (chains are short; bound malformed input)
                    let filter = try sleb(&q)
                    let nextPos = q
                    let next = try sleb(&q)
                    if filter > maxFilter { maxFilter = filter }
                    if next == 0 { break }
                    q = nextPos &+ UInt64(bitPattern: next)
                }
            }
            guard maxFilter > 0 && maxFilter < 100_000 else { return }
            for i in 1...maxFilter {
                var e = base - UInt64(i) * UInt64(size)
                let ptr = try pointer(&e, ttEnc & 0x7F)
                try fix(ptr.field, ptr.value, ptr.target, ttEnc & 0x7F)
            }
        }
    }
}
