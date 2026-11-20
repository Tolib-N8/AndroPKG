import Foundation

/// Minimal ELF64 (little-endian, AArch64) reader — only what the wrapper needs.
public struct ElfFile: Sendable {
    public struct ProgramHeader: Sendable {
        public var type: UInt32
        public var flags: UInt32
        public var offset: UInt64
        public var vaddr: UInt64
        public var filesz: UInt64
        public var memsz: UInt64
        public var align: UInt64

        public var isLoad: Bool { type == ElfFile.PT_LOAD }
        public var readable: Bool { flags & 4 != 0 }
        public var writable: Bool { flags & 2 != 0 }
        public var executable: Bool { flags & 1 != 0 }
    }

    public struct SectionHeader: Sendable {
        public var name: String
        public var type: UInt32
        public var flags: UInt64
        public var addr: UInt64
        public var offset: UInt64
        public var size: UInt64

        public var isExecInstr: Bool { flags & 0x4 != 0 }  // SHF_EXECINSTR
    }

    public static let PT_LOAD: UInt32 = 1
    public static let PT_DYNAMIC: UInt32 = 2
    public static let PT_TLS: UInt32 = 7
    public static let ET_DYN: UInt16 = 3
    public static let EM_AARCH64: UInt16 = 183

    public let bytes: [UInt8]
    public let type: UInt16
    public let programHeaders: [ProgramHeader]
    public let sections: [SectionHeader]

    public var loadSegments: [ProgramHeader] {
        programHeaders.filter(\.isLoad).sorted { $0.vaddr < $1.vaddr }
    }

    public init(bytes: [UInt8]) throws {
        let r = ByteReader(bytes: bytes)
        guard bytes.count >= 64,
              bytes[0] == 0x7F, bytes[1] == 0x45, bytes[2] == 0x4C, bytes[3] == 0x46,
              bytes[4] == 2 /* ELFCLASS64 */, bytes[5] == 1 /* ELFDATA2LSB */
        else { throw ConverterError.notElf }
        let type = try r.u16(16)
        guard try r.u16(18) == ElfFile.EM_AARCH64 else { throw ConverterError.notElf }
        guard type == ElfFile.ET_DYN else {
            throw ConverterError.unsupported("ELF type \(type); only shared objects (ET_DYN) are supported")
        }

        let phoff = Int(try r.u64(32)), shoff = Int(try r.u64(40))
        let phentsize = Int(try r.u16(54)), phnum = Int(try r.u16(56))
        let shentsize = Int(try r.u16(58)), shnum = Int(try r.u16(60)), shstrndx = Int(try r.u16(62))

        var phs: [ProgramHeader] = []
        for i in 0..<phnum {
            let o = phoff + i * phentsize
            phs.append(ProgramHeader(
                type: try r.u32(o), flags: try r.u32(o + 4), offset: try r.u64(o + 8),
                vaddr: try r.u64(o + 16), filesz: try r.u64(o + 32), memsz: try r.u64(o + 40),
                align: try r.u64(o + 48)))
        }

        // Section headers are optional (stripped libs may lack them); tolerate absence.
        var shs: [SectionHeader] = []
        if shoff != 0, shnum > 0, shstrndx < shnum, shoff + shnum * shentsize <= bytes.count {
            let strOff = Int(try r.u64(shoff + shstrndx * shentsize + 24))
            for i in 0..<shnum {
                let o = shoff + i * shentsize
                let nameOff = Int(try r.u32(o))
                shs.append(SectionHeader(
                    name: (try? r.cString(strOff + nameOff)) ?? "",
                    type: try r.u32(o + 4), flags: try r.u64(o + 8), addr: try r.u64(o + 16),
                    offset: try r.u64(o + 24), size: try r.u64(o + 32)))
            }
        }

        self.bytes = bytes
        self.type = type
        self.programHeaders = phs
        self.sections = shs
    }
}
