import Foundation

/// Emits a minimal MH_DYLIB that dyld will map and codesign will sign.
/// No symbols, no fixups: all linking happens in the andropkg ELF loader.
enum MachOWriter {
    static let LC_SEGMENT_64: UInt32 = 0x19
    static let LC_ID_DYLIB: UInt32 = 0xD
    static let LC_LOAD_DYLIB: UInt32 = 0xC
    static let LC_SYMTAB: UInt32 = 0x2
    static let LC_DYSYMTAB: UInt32 = 0xB
    static let LC_UUID: UInt32 = 0x1B
    static let LC_BUILD_VERSION: UInt32 = 0x32

    static let MH_MAGIC_64: UInt32 = 0xFEED_FACF
    static let CPU_TYPE_ARM64: UInt32 = 0x0100_000C
    static let MH_DYLIB: UInt32 = 6
    // MH_NOUNDEFS | MH_DYLDLINK | MH_TWOLEVEL | MH_NO_REEXPORTED_DYLIBS
    static let headerFlags: UInt32 = 0x1 | 0x4 | 0x80 | 0x10_0000

    static func write(segments: [ElfWrapper.Segment], info: WrapInfo,
                      options: ElfWrapper.Options, uuid: [UInt8]) throws -> [UInt8] {
        let page = Int(ElfWrapper.pageSize)
        let base = ElfWrapper.elfBaseVM

        // File layout: header page, then each segment's file-backed bytes on page boundaries.
        struct Placed { var seg: ElfWrapper.Segment; var fileOff: Int; var fileSize: Int }
        var placed: [Placed] = []
        var cursor = page
        for s in segments {
            let fsz = alignUp(s.content.count, page)
            guard UInt64(fsz) <= s.vmEnd - s.vmStart else {
                throw ConverterError.layout("\(s.name): file size exceeds vm size")
            }
            placed.append(Placed(seg: s, fileOff: fsz == 0 ? 0 : cursor, fileSize: fsz))
            cursor += fsz
        }
        let linkeditOff = cursor
        let linkeditContent = [UInt8](repeating: 0, count: 16)   // empty string table
        let linkeditVM = base + segments.last!.vmEnd

        // Load commands.
        var cmds = ByteWriter()
        var ncmds: UInt32 = 0

        func segment(_ name: String, vm: UInt64, vmsize: UInt64, fileoff: Int, filesize: Int,
                     maxprot: UInt32, initprot: UInt32, sections: [(String, UInt64, UInt64, Int)] = []) {
            cmds.u32(LC_SEGMENT_64)
            cmds.u32(UInt32(72 + 80 * sections.count))
            cmds.fixedString(name, width: 16)
            cmds.u64(vm); cmds.u64(vmsize)
            cmds.u64(UInt64(fileoff)); cmds.u64(UInt64(filesize))
            cmds.u32(maxprot); cmds.u32(initprot)
            cmds.u32(UInt32(sections.count)); cmds.u32(0)
            for (sect, addr, size, off) in sections {
                cmds.fixedString(sect, width: 16)
                cmds.fixedString(name, width: 16)
                cmds.u64(addr); cmds.u64(size)
                cmds.u32(UInt32(off)); cmds.u32(3)      // offset, align 2^3
                cmds.u32(0); cmds.u32(0)                // reloff, nreloc
                cmds.u32(0)                             // S_REGULAR
                cmds.u32(0); cmds.u32(0); cmds.u32(0)   // reserved1..3
            }
            ncmds += 1
        }

        func dylib(_ cmd: UInt32, _ path: String, current: UInt32, compat: UInt32) {
            let size = alignUp(24 + path.utf8.count + 1, 8)
            cmds.u32(cmd); cmds.u32(UInt32(size))
            cmds.u32(24); cmds.u32(1)                   // name offset, timestamp
            cmds.u32(current); cmds.u32(compat)
            cmds.fixedString(path, width: size - 24)
            ncmds += 1
        }

        let rx = ElfWrapper.protR | ElfWrapper.protX
        segment("__TEXT", vm: 0, vmsize: UInt64(page), fileoff: 0, filesize: page, maxprot: rx, initprot: rx,
                sections: [("__elfwrap", UInt64(ElfWrapper.infoFileOffset), UInt64(WrapInfo.size),
                            ElfWrapper.infoFileOffset)])
        for p in placed {
            segment(p.seg.name, vm: base + p.seg.vmStart, vmsize: p.seg.vmEnd - p.seg.vmStart,
                    fileoff: p.fileOff, filesize: p.fileSize, maxprot: p.seg.prot, initprot: p.seg.prot)
        }
        segment("__LINKEDIT", vm: linkeditVM, vmsize: UInt64(page), fileoff: linkeditOff,
                filesize: linkeditContent.count, maxprot: ElfWrapper.protR, initprot: ElfWrapper.protR)

        dylib(LC_ID_DYLIB, options.installName, current: version(1, 0), compat: version(1, 0))
        dylib(LC_LOAD_DYLIB, "/usr/lib/libSystem.B.dylib", current: version(1351, 0), compat: version(1, 0))

        cmds.u32(LC_SYMTAB); cmds.u32(24)
        cmds.u32(UInt32(linkeditOff)); cmds.u32(0)                          // symoff, nsyms
        cmds.u32(UInt32(linkeditOff)); cmds.u32(UInt32(linkeditContent.count)) // stroff, strsize
        ncmds += 1

        cmds.u32(LC_DYSYMTAB); cmds.u32(80)
        for _ in 0..<18 { cmds.u32(0) }
        ncmds += 1

        cmds.u32(LC_UUID); cmds.u32(24); cmds.append(uuid)
        ncmds += 1

        cmds.u32(LC_BUILD_VERSION); cmds.u32(24)
        cmds.u32(options.platform.machOPlatform)
        cmds.u32(options.platform.minOS)
        cmds.u32(version(26, 0))   // sdk
        cmds.u32(0)                // ntools
        ncmds += 1

        // codesign must be able to insert LC_CODE_SIGNATURE (16 bytes) after the commands.
        guard 32 + cmds.count + 16 <= ElfWrapper.infoFileOffset else {
            throw ConverterError.layout("load commands overflow header page")
        }

        var out = ByteWriter()
        out.u32(MH_MAGIC_64); out.u32(CPU_TYPE_ARM64); out.u32(0)   // cpusubtype ALL
        out.u32(MH_DYLIB); out.u32(ncmds); out.u32(UInt32(cmds.count))
        out.u32(headerFlags); out.u32(0)
        out.append(cmds.bytes)
        out.pad(to: ElfWrapper.infoFileOffset)
        out.append(info.bytes)
        out.pad(to: page)
        for p in placed where p.fileSize > 0 {
            out.pad(to: p.fileOff)
            out.append(p.seg.content)
            out.pad(to: p.fileOff + p.fileSize)
        }
        out.pad(to: linkeditOff)
        out.append(linkeditContent)
        return out.bytes
    }
}
