import Testing
@testable import AndroPkgCore

/// Expected encodings were produced with `llvm-mc -triple=aarch64 -show-encoding`.
@Suite struct InstructionTests {
    @Test func decodesPatchableInstructions() {
        #expect(PatchKind(instruction: 0xD400_0001) == .svc)
        #expect(PatchKind(instruction: 0xD53B_D045) == .tpRead(rt: 5))   // mrs x5, tpidr_el0
        #expect(PatchKind(instruction: 0xD51B_D045) == .tpWrite(rt: 5))  // msr tpidr_el0, x5
        #expect(PatchKind(instruction: 0xD53B_D065) == nil)              // mrs x5, tpidrro_el0
        #expect(PatchKind(instruction: 0xD400_1001) == nil)              // svc #0x80 (Darwin)
    }

    @Test func tpReadTrampolineMatchesAssembler() throws {
        let bytes = try PatchKind.tpRead(rt: 5).trampolineBytes(at: 0x10000, site: 0x4000, dataBase: 0x14000)
        let words = stride(from: 0, to: bytes.count, by: 4).map {
            UInt32(bytes[$0]) | UInt32(bytes[$0 + 1]) << 8 | UInt32(bytes[$0 + 2]) << 16 | UInt32(bytes[$0 + 3]) << 24
        }
        #expect(words[0] == 0xF81F_0FF0)  // str x16, [sp, #-16]!
        #expect(words[1] == 0xD53B_D065)  // mrs x5, tpidrro_el0
        #expect(words[2] == 0x927D_F0A5)  // and x5, x5, #~7
        #expect(words[5] == 0xF870_68A5)  // ldr x5, [x5, x16]
        #expect(words[6] == 0xB500_00E5)  // cbnz x5, +7 (no TCB yet: the slow path, words 7-12)
        #expect(words[13] == 0xF841_07F0) // ldr x16, [sp], #16
        #expect(words[14] == 0x17FF_CFF3) // b 0x4004 (from 0x10038)
    }

    @Test func ctrReadBecomesConstant() throws {
        #expect(PatchKind(instruction: 0xD53B_0023) == .ctrRead(rt: 3))   // mrs x3, ctr_el0
        let bytes = try PatchKind.ctrRead(rt: 3).trampolineBytes(at: 0x10000, site: 0x4000, dataBase: 0x14000)
        let words = stride(from: 0, to: bytes.count, by: 4).map {
            UInt32(bytes[$0]) | UInt32(bytes[$0 + 1]) << 8 | UInt32(bytes[$0 + 2]) << 16 | UInt32(bytes[$0 + 3]) << 24
        }
        #expect(words[0] == 0xD298_0083)  // movz x3, #0xc004
        #expect(words[1] == 0xF2B6_8883)  // movk x3, #0xb444, lsl #16
        #expect(words[2] == 0x17FF_CFFF)  // b 0x4004 (from 0x10008)
    }

    @Test func hookJumpsThroughItsDataSlot() throws {
        let bytes = try PatchKind.hook(index: 1).trampolineBytes(at: 0x10000, site: 0x4000, dataBase: 0x14000)
        let words = stride(from: 0, to: bytes.count, by: 4).map {
            UInt32(bytes[$0]) | UInt32(bytes[$0 + 1]) << 8 | UInt32(bytes[$0 + 2]) << 16 | UInt32(bytes[$0 + 3]) << 24
        }
        #expect(words[0] == 0x9000_0030)  // adrp x16, 0x14000 (4 pages up)
        #expect(words[1] == 0xF940_1210)  // ldr x16, [x16, #32]  (slot 24 + 8)
        #expect(words[2] == 0xD61F_0200)  // br x16
    }

    @Test func tpReadOfX16UsesX17Scratch() throws {
        let bytes = try PatchKind.tpRead(rt: 16).trampolineBytes(at: 0x10000, site: 0x4000, dataBase: 0x14000)
        #expect(Array(bytes[0..<4]) == [0xF1, 0x0F, 0x1F, 0xF8])     // str x17, [sp, #-16]!
        #expect(Array(bytes[20..<24]) == [0x10, 0x6A, 0x71, 0xF8])   // ldr x16, [x16, x17]
    }

    @Test func branchRangeIsChecked() {
        #expect(throws: ConverterError.self) { try AArch64.b(from: 0, to: 1 << 28) }
        #expect(throws: ConverterError.self) { try AArch64.b(from: 0, to: 2) }
    }
}

@Suite struct WrapTests {
    /// Builds a tiny synthetic ELF: one r-x PT_LOAD containing `svc #0; mrs x0, tpidr_el0; ret`.
    static func makeElf(secondSegmentVaddr: UInt64? = nil) -> [UInt8] {
        var w = ByteWriter()
        let phnum: UInt16 = secondSegmentVaddr == nil ? 1 : 2
        w.append([0x7F, 0x45, 0x4C, 0x46, 2, 1, 1, 0]); w.pad(to: 16)
        w.u16(3); w.u16(183); w.u32(1)                 // ET_DYN, EM_AARCH64
        w.u64(0); w.u64(64); w.u64(0)                  // entry, phoff, shoff
        w.u32(0); w.u16(64); w.u16(56); w.u16(phnum)   // flags, ehsize, phentsize, phnum
        w.u16(64); w.u16(0); w.u16(0)
        // PT_LOAD r-x covering the whole file
        w.u32(1); w.u32(5); w.u64(0); w.u64(0); w.u64(0); w.u64(0x200); w.u64(0x200); w.u64(0x4000)
        if let v = secondSegmentVaddr {
            w.u32(1); w.u32(6); w.u64(0x200); w.u64(v); w.u64(v); w.u64(8); w.u64(8); w.u64(0x1000)
        }
        w.pad(to: 0x100)
        w.u32(0xD400_0001); w.u32(0xD53B_D040); w.u32(0xD65F_03C0)
        w.pad(to: 0x208)
        return w.bytes
    }

    @Test func wrapsAndRewrites() throws {
        let elf = try ElfFile(bytes: Self.makeElf())
        let r = try ElfWrapper.wrap(elf, options: .init(platform: .iOS, installName: "@rpath/t.dylib"))
        #expect(r.patches.map(\.kind) == [.svc, .tpRead(rt: 0)])
        let rd = ByteReader(bytes: r.machO)
        #expect(try rd.u32(0) == 0xFEED_FACF)
        #expect(try rd.u32(12) == 6)                                   // MH_DYLIB
        #expect(Array(r.machO[0x2000..<0x2008]) == Array("APKELF01".utf8))
        // Patched site (ELF vaddr 0x100 -> file 0x4000 + 0x100) is now a branch, not svc.
        #expect(try rd.u32(0x4100) & 0xFC00_0000 == 0x1400_0000)
        #expect(r.machO.count % 0x4000 == 16)                          // pages + linkedit stub
    }

    /// ELFs linked for 4 KB pages (code and data on one 16 KB page) are laid out again
    /// (Layout4K) instead of being rejected.
    @Test func relaysOut4KSharedPages() throws {
        let elf = try ElfFile(bytes: Self.makeElf(secondSegmentVaddr: 0x1200))
        let r = try ElfWrapper.wrap(elf, options: .init(platform: .iOS, installName: "t"))
        #expect(r.segmentMap.contains { $0.contains("4KB-page ELF") })
    }

    @Test func rejectsNonElf() {
        #expect(throws: ConverterError.self) { try ElfFile(bytes: [UInt8](repeating: 0, count: 128)) }
    }
}
