#!/usr/bin/env python3
"""LoongOS 阶段5⑤ - 从 ELF 直接生成合规的 EFI 应用 PE32+ 映像

为什么不用 objcopy：
  Debian binutils 2.40 的 objcopy 在 aarch64 上产出的是"可选头残缺"的 PE：
  段 RVA 从 0 开始（与 PE 头重叠）、.dynamic 不对齐 4KB、还带着
  RELOCS_STRIPPED 标志 + ImageBase=0 —— EDK2 固件见此直接返回 Unsupported。

本脚本做的事：
  1) 读 ELF 的 PT_LOAD 段（这是一份完整的、vaddr 从 0 起的扁平映像）
  2) 整体平移 0x1000 放到第一个 4KB 对齐的 RVA —— 所有 adrp/adr 是
     PC 相对的，整体平移不影响相对距离，代码一行不用改
  3) 从 ELF 的 SHT_RELA 段收集 R_AARCH64_RELATIVE，生成真正的 PE .reloc
     重定位表（IMAGE_REL_BASED_DIR64），并**清掉 RELOCS_STRIPPED**，
     让固件按"可重定位映像"加载
  4) 把 _start 里的 `bl _relocate` 打成 NOP：gnu-efi 的运行时自搬逻辑
     会再应用一遍重定位，固件已经搬过了，不打断就是双重位移
"""
import struct
import sys

SRC = sys.argv[1] if len(sys.argv) > 1 else 'build/uefi_stub.so'
DST = sys.argv[2] if len(sys.argv) > 2 else 'BOOTAA64.EFI'

SHIFT = 0x1000            # 整幅平移量 = 第一个段的 RVA
FILE_ALIGN = 0x200
SECTION_ALIGN = 0x1000
IMAGE_BASE = 0
SUBSYS_EFI_APP = 10
R_AARCH64_RELATIVE = 0x403

elf = open(SRC, 'rb').read()

# ---------------- 1. 解析 ELF：PT_LOAD 与 SHT_RELA ----------------
assert elf[:4] == b'\x7fELF', '不是 ELF'
assert elf[4] == 2 and elf[5] == 1, '需要 ELF64 小端'
e_phoff, = struct.unpack_from('<Q', elf, 0x20)
e_phentsize, e_phnum = struct.unpack_from('<HH', elf, 0x36)
e_shoff, = struct.unpack_from('<Q', elf, 0x28)
# ⚠️ ELF64 头布局：e_phentsize@0x36 e_phnum@0x38 e_shentsize@0x3A e_shnum@0x3C e_shstrndx@0x3E
e_shentsize, e_shnum = struct.unpack_from('<HH', elf, 0x3A)

loads = []
for i in range(e_phnum):
    o = e_phoff + i * e_phentsize
    p_type, p_flags = struct.unpack_from('<II', elf, o)
    p_offset, p_vaddr, p_paddr, p_filesz = struct.unpack_from('<QQQQ', elf, o + 8)
    if p_type == 1:  # PT_LOAD
        loads.append((p_vaddr, p_offset, p_filesz))
assert loads, '没有 PT_LOAD'
lo_vaddr = min(l[0] for l in loads)
hi_vaddr = max(l[0] + l[2] for l in loads)
img = bytearray(hi_vaddr - lo_vaddr)
for vaddr, off, filesz in loads:
    img[vaddr - lo_vaddr:(vaddr - lo_vaddr) + filesz] = elf[off:off + filesz]

# 收集重定位（.rela.got / .rela.data 等都算）
relocs = []   # (r_offset, type, addend)
for i in range(e_shnum):
    o = e_shoff + i * e_shentsize
    sh_type, = struct.unpack_from('<I', elf, o + 4)
    if sh_type != 4:      # SHT_RELA
        continue
    sh_offset, sh_size, sh_link, sh_info, sh_entsize = struct.unpack_from(
        '<QQIIQ', elf, o + 0x18)
    if sh_entsize == 0:
        continue
    for k in range(sh_size // sh_entsize):
        ro = sh_offset + k * sh_entsize
        r_offset, r_info, r_addend = struct.unpack_from('<QQq', elf, ro)
        r_type = r_info & 0xFFFFFFFF
        if r_type == R_AARCH64_RELATIVE:
            relocs.append((r_offset, r_addend))

print(f"PT_LOAD: vaddr 0x{lo_vaddr:x}..0x{hi_vaddr:x}  "
      f"映像 {len(img)} 字节；R_AARCH64_RELATIVE 重定位 {len(relocs)} 条")

# ---------------- 2. 打断 _relocate（避免与固件双重重定位）----------------
# _start 里 `bl _relocate` 是固定位置：找一个指向 _relocate 的 bl
e_entry, = struct.unpack_from('<Q', elf, 0x18)
# 在 _start 附近找 bl 到 _relocate（地址从符号表读，实测 0x61f0）
_reloc_addr = None
e_shstrndx = None
for i in range(e_shnum):
    o = e_shoff + i * e_shentsize
    sh_type, = struct.unpack_from('<I', elf, o + 4)
    if sh_type == 2:                      # SHT_SYMTAB
        sh_offset, sh_size, sh_link, sh_info, sh_entsize = struct.unpack_from(
            '<QQIIQ', elf, o + 0x18)
        strtab_off, strtab_size = struct.unpack_from('<QQ', elf, e_shoff + sh_link * e_shentsize + 0x18)
        for k in range(sh_size // sh_entsize):
            so = sh_offset + k * sh_entsize
            st_name, = struct.unpack_from('<I', elf, so)
            st_value, = struct.unpack_from('<Q', elf, so + 8)
            nm = elf[strtab_off + st_name: elf.find(b'\0', strtab_off + st_name)].decode('latin1', 'replace')
            if nm == '_relocate':
                _reloc_addr = st_value - lo_vaddr
                break
assert _reloc_addr is not None, '符号表里没有 _relocate'
print(f"_relocate 位于映像偏移 0x{_reloc_addr:x}")

patched = False
for off in range(0, min(len(img) - 4, 0x2000), 4):
    insn, = struct.unpack_from('<I', img, off)
    if (insn >> 26) != 0b100101:      # 不是 BL
        continue
    imm26 = insn & 0x3FFFFFF
    if imm26 & (1 << 25):
        imm26 -= (1 << 26)
    target = off + imm26 * 4
    if target == _reloc_addr:         # 指向 _relocate
        # 换成 mov x0, #0（而不是 NOP）：
        #   _start 在 bl 之后紧跟 `cbnz x0, ret`，x0 必须是 0 才会走到 efi_main。
        #   直接 NOP 的话 x0 还留着前面 adr 算出的装载地址 → cbnz 跳走 → 永不进 main。
        struct.pack_into('<I', img, off, 0xD2800000)   # mov x0, #0
        print(f"已打断自搬：0x{off:x} 的 bl _relocate -> mov x0,#0")
        patched = True
        break
assert patched, '没找到 bl _relocate，ELF 布局可能变了'

# ---------------- 2b. 平移重定位目标 ----------------
# ELF 语义：*(site) = load_base + addend；实测 site 里存的正是 addend。
# 我们把整幅映像平移了 SHIFT，所以目标地址也跟着 +SHIFT：
#   PE 加载器执行 *(site) += load_addr（ImageBase=0）
#   需要最终 = load_addr + (addend + SHIFT)  →  先把 site 预置成 addend + SHIFT
for r_offset, _addend in relocs:
    pos = r_offset - lo_vaddr
    cur, = struct.unpack_from('<Q', img, pos)
    struct.pack_into('<Q', img, pos, cur + SHIFT)
print(f"已平移 {len(relocs)} 个重定位目标 +0x{SHIFT:x}")

# ---------------- 3. 构造 PE 段布局 ----------------
# .text : vaddr [0x0, 0x7600)  -> RVA 0x1000
# .data : vaddr [0x8000, 末尾) -> RVA 0x9000

def align(n, a):
    return (n + a - 1) & ~(a - 1)

TEXT_END = 0x7600
text_bytes = bytes(img[0:TEXT_END])
data_start = 0x8000
data_bytes = bytes(img[data_start:])

text_rva = SHIFT
data_rva = SHIFT + data_start
text_raw = align(len(text_bytes), FILE_ALIGN)
data_raw = align(len(data_bytes), FILE_ALIGN)

# ---------------- 4. 生成 .reloc ----------------
# IMAGE_REL_BASED_DIR64 = 10；每条 2 字节：(type<<12)|页内偏移
pages = {}
for r_offset, _addend in relocs:
    rva = r_offset + SHIFT
    page = rva >> 12
    pages.setdefault(page, []).append(rva & 0xFFF)

reloc_body = bytearray()
for page in sorted(pages):
    offs = sorted(pages[page])
    entry_bytes = len(offs) * 2
    block_size = 8 + entry_bytes
    if block_size % 4:
        block_size += 4 - (block_size % 4)
    reloc_body += struct.pack('<II', page << 12, block_size)
    for ofs in offs:
        reloc_body += struct.pack('<H', (10 << 12) | ofs)
    pad = block_size - 8 - entry_bytes
    reloc_body += b'\0' * pad
reloc_bytes = bytes(reloc_body)
reloc_rva = data_rva + data_raw
reloc_raw = align(len(reloc_bytes), FILE_ALIGN)

size_of_image = align(reloc_rva + len(reloc_bytes), SECTION_ALIGN)

# ---------------- 5. 写 PE 头 ----------------
NUM_SECTIONS = 3
OPT_HDR_SIZE = 240
DOS_HDR_SIZE = 0x40
PE_OFFSET = 0x80
headers_size = PE_OFFSET + 4 + 20 + OPT_HDR_SIZE + NUM_SECTIONS * 40
size_of_headers = align(headers_size, FILE_ALIGN)

entry_rva = SHIFT + 0x1000        # _start 在 vaddr 0x1000

dos = bytearray(DOS_HDR_SIZE)
struct.pack_into('<H', dos, 0x00, 0x5A4D)          # MZ
struct.pack_into('<I', dos, 0x3C, PE_OFFSET)       # e_lfanew

coff = struct.pack(
    '<HHIIIHH',
    0xAA64,          # Machine = ARM64
    NUM_SECTIONS,
    0, 0, 0,         # TimeDateStamp / SymbolTable / NumberOfSymbols
    OPT_HDR_SIZE,
    0x0202,          # EXECUTABLE_IMAGE | DEBUG_STRIPPED（关键：不含 RELOCS_STRIPPED）
)

opt = bytearray(OPT_HDR_SIZE)
struct.pack_into('<H', opt, 0x00, 0x20B)           # PE32+
struct.pack_into('<I', opt, 0x04, text_raw)                    # SizeOfCode
struct.pack_into('<I', opt, 0x08, data_raw + reloc_raw)        # SizeOfInitializedData
struct.pack_into('<I', opt, 0x0C, 0)               # SizeOfUninitializedData
struct.pack_into('<I', opt, 0x10, entry_rva)       # AddressOfEntryPoint
struct.pack_into('<I', opt, 0x14, text_rva)        # BaseOfCode
struct.pack_into('<Q', opt, 0x18, IMAGE_BASE)      # ImageBase
struct.pack_into('<I', opt, 0x20, SECTION_ALIGN)
struct.pack_into('<I', opt, 0x24, FILE_ALIGN)
struct.pack_into('<HH', opt, 0x28, 0, 0)           # OS version
struct.pack_into('<HH', opt, 0x2C, 0, 0)           # Image version
struct.pack_into('<HH', opt, 0x30, 0, 0)           # Subsystem version
struct.pack_into('<I', opt, 0x34, 0)               # Win32VersionValue
struct.pack_into('<I', opt, 0x38, size_of_image)
struct.pack_into('<I', opt, 0x3C, size_of_headers)
struct.pack_into('<I', opt, 0x40, 0)               # CheckSum
struct.pack_into('<H', opt, 0x44, SUBSYS_EFI_APP)
struct.pack_into('<H', opt, 0x46, 0)               # DllCharacteristics
struct.pack_into('<Q', opt, 0x48, 0x100000)        # SizeOfStackReserve
struct.pack_into('<Q', opt, 0x50, 0x1000)          # SizeOfStackCommit
struct.pack_into('<Q', opt, 0x58, 0x100000)        # SizeOfHeapReserve
struct.pack_into('<Q', opt, 0x60, 0x1000)          # SizeOfHeapCommit
struct.pack_into('<I', opt, 0x68, 0)               # LoaderFlags
struct.pack_into('<I', opt, 0x6C, 16)              # NumberOfRvaAndSizes
# DataDirectory[5] = Base Relocation Table
struct.pack_into('<II', opt, 0x70 + 5 * 8, reloc_rva, len(reloc_bytes))


def section_header(name, vsize, vaddr, rawsize, rawptr, chars):
    h = bytearray(40)
    h[0:len(name)] = name.encode()
    struct.pack_into('<IIII', h, 8, vsize, vaddr, rawsize, rawptr)
    struct.pack_into('<I', h, 36, chars)
    return bytes(h)


raw_ptr = size_of_headers
text_hdr = section_header('.text', len(text_bytes), text_rva, text_raw, raw_ptr,
                          0x60000020)          # CODE|EXECUTE|READ
raw_ptr += text_raw
data_hdr = section_header('.data', len(data_bytes), data_rva, data_raw, raw_ptr,
                          0xC0000040)          # INIT_DATA|READ|WRITE
raw_ptr += data_raw
reloc_hdr = section_header('.reloc', len(reloc_bytes), reloc_rva, reloc_raw, raw_ptr,
                           0x42000040)         # INIT_DATA|DISCARDABLE|READ

# ---------------- 6. 拼文件 ----------------
out = bytearray()
out += dos
out += b'\0' * (PE_OFFSET - len(out))
out += b'PE\0\0'
out += coff
out += opt
out += text_hdr + data_hdr + reloc_hdr
out += b'\0' * (size_of_headers - len(out))
out += text_bytes + b'\0' * (text_raw - len(text_bytes))
out += data_bytes + b'\0' * (data_raw - len(data_bytes))
out += reloc_bytes + b'\0' * (reloc_raw - len(reloc_bytes))

open(DST, 'wb').write(bytes(out))
print(f"✅ {DST}: {len(out)} 字节  entry=0x{entry_rva:x}  "
      f".text@0x{text_rva:x}({text_raw})  .data@0x{data_rva:x}({data_raw})  "
      f".reloc@0x{reloc_rva:x}({len(reloc_bytes)})  SizeOfImage=0x{size_of_image:x}")
