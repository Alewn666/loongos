/* LoongOS 阶段5⑤ - UEFI 引导 stub
 * BOOTAA64.EFI：固件把它加载到任意地址 → 它从同一 FAT 卷读 kernel.elf
 *   → 按 ELF program header 把 PT_LOAD 段拷到 paddr（0x40080000 起）
 *   → ExitBootServices → 关 MMU/缓存（复现 -kernel 直载的裸机现场）→ 跳 _start。
 * 内核主体零改动：_start 自带 EL2→EL1 降级 + 设栈 + 清 bss，stub 只补上
 * "从磁盘文件装载"这一段 -kernel 由 QEMU 代劳的活。
 *
 * ⚠️ 不要用 gnu-efi 的 Print()：它的 %lx 在 aarch64 上会打出几万个空格把串口刷爆
 *    （实测踩过）。这里全部自己拼 CHAR16 再 OutputString。
 */
#include <efi.h>
#include <efilib.h>

/* ---- 最小 ELF64 定义（不依赖系统 elf.h，它带 libc 类型） ---- */
#define PT_LOAD 1
typedef struct {
    UINT8  e_ident[16];
    UINT16 e_type, e_machine;
    UINT32 e_version;
    UINT64 e_entry, e_phoff, e_shoff;
    UINT32 e_flags;
    UINT16 e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;
typedef struct {
    UINT32 p_type, p_flags;
    UINT64 p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} Elf64_Phdr;

/* 传给内核的启动信息。放在 stub 的 .data 里：stub 镜像在 ExitBootServices 之后
 * 依然留在内存（引导服务不会回收已加载的映像），所以这个地址是安全可读的。 */
typedef struct {
    UINT32 magic;       /* 'LOOS' = 0x4C4F4F53 */
    UINT32 width;
    UINT32 height;
    UINT32 stride;      /* 每行像素数 */
    UINT32 pixfmt;      /* 0=RGBX, 1=BGRX */
    UINT32 bpp;
    UINT64 base;        /* 帧缓冲物理地址 */
    UINT64 size;
    UINT64 img_addr;    /* 阶段6③：esp.img 被读到的物理地址（安装器用；0 = 没有） */
    UINT64 img_size;
} LOONGOS_BOOTINFO;

#define BOOTINFO_MAGIC 0x4C4F4F53u
static LOONGOS_BOOTINFO g_bi;

#define KERNEL_BASE 0x40080000UL
#define PL011_DR    0x09000000UL

/* ---------------- 极简输出助手（不用 Print） ---------------- */
static CHAR16 obuf[192];
static UINTN  olen;

static void o_reset(void) { olen = 0; obuf[0] = 0; }
static void o_str(const CHAR16 *s)
{
    while (*s && olen < (sizeof(obuf) / sizeof(CHAR16)) - 1) obuf[olen++] = *s++;
    obuf[olen] = 0;
}
static void o_hex(UINT64 v)
{
    static const CHAR16 hx[] = L"0123456789ABCDEF";
    CHAR16 tmp[19];
    UINTN n = 0;
    tmp[n++] = '0'; tmp[n++] = 'x';
    for (INT32 i = 15; i >= 0; i--) tmp[n++] = hx[(v >> (i * 4)) & 0xF];
    tmp[n] = 0;
    o_str(tmp);
}
static void o_dec(UINT64 v)
{
    CHAR16 tmp[21];
    UINTN n = 0, i;
    if (v == 0) { o_str(L"0"); return; }
    while (v && n < 20) { tmp[n++] = (CHAR16)('0' + (v % 10)); v /= 10; }
    for (i = 0; i < n / 2; i++) { CHAR16 t = tmp[i]; tmp[i] = tmp[n - 1 - i]; tmp[n - 1 - i] = t; }
    tmp[n] = 0;
    o_str(tmp);
}
static void o_flush(EFI_SYSTEM_TABLE *st)
{
    obuf[olen] = 0;
    st->ConOut->OutputString(st->ConOut, obuf);
    olen = 0;
    obuf[0] = 0;
}
/* 一行：标签 + 若干十六进制值 */
static void o_line(EFI_SYSTEM_TABLE *st, const CHAR16 *label,
                   UINT64 a, UINT64 b, UINT64 c)
{
    o_reset(); o_str(label);
    if (a) { o_str(L" "); o_hex(a); }
    if (b) { o_str(L" "); o_hex(b); }
    if (c) { o_str(L" "); o_hex(c); }
    o_str(L"\r\n");
    o_flush(st);
}

/* 直接往 PL011 串口写（绕过 UEFI ConOut）。
 * 用途：判断虚拟机的显示界面接的是"串口"还是"图形控制台(GOP)"。
 *   屏幕上能看到 → 串口可见，内核不打字就是真崩了
 *   看不到       → 显示的是 GOP，内核写串口自然看不见（内核可能还活着） */
static void pl011_write(const char *m)
{
    volatile UINT32 *dr = (volatile UINT32 *)PL011_DR;
    while (*m) *dr = (UINT8)*m++;     /* ⚠️ 必须 m++，否则死循环 */
}

/* 把一段内存从 D-cache 清到 PoC。
 * ⚠️ KVM（真实 CPU）下缓存是开的：stub 写进内存的内核段可能还躺在 cache 里，
 *    而内核是关缓存运行的，读到的就是旧数据 → 跳飞/卡死。QEMU TCG 下不易复现。 */
static void clean_dcache(UINT64 start, UINT64 end)
{
    UINT64 a;
    for (a = start & ~(UINT64)63; a < end; a += 64)
        __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

/* 若当前在 EL2（KVM 常见），跳内核前必须关掉 EL2 的 MMU，
 * 否则 eret 到 EL1 后 EL2 侧状态残留，行为不可预期。 */
static void drop_el2_mmu(void)
{
    UINT64 el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    if (((el >> 2) & 3) != 2) return;
    __asm__ volatile(
        "mrs x9, sctlr_el2\n"
        "bic x9, x9, #(1<<0)\n"     /* 关 EL2 MMU */
        "bic x9, x9, #(1<<2)\n"     /* 关 EL2 dcache */
        "bic x9, x9, #(1<<12)\n"    /* 关 EL2 icache */
        "msr sctlr_el2, x9\n"
        "isb\n"
        "tlbi alle2\n"
        "dsb sy\n"
        "isb\n" ::: "x9", "memory");
}

/* 关 MMU + 缓存 + TLB，然后跳内核入口（不返回）。
 * x0 传 0（=DTB 指针，内核 kernel_main 显式 (void)dtb，不用）。 */
static void __attribute__((noreturn)) enter_kernel(UINT64 entry)
{
    __asm__ volatile("msr daifset, #0xf");            /* 关中断：内核自己开 */
    __asm__ volatile(
        "mrs x9, sctlr_el1\n"
        "bic x9, x9, #(1<<0)\n"                         /* 清 M：关 MMU */
        "bic x9, x9, #(1<<2)\n"                         /* 清 C：关 dcache */
        "bic x9, x9, #(1<<12)\n"                        /* 清 I：关 icache */
        "msr sctlr_el1, x9\n"
        "isb\n");
    __asm__ volatile(
        "tlbi vmalle1\n"                               /* 无效化 TLB */
        "ic iallu\n"                                   /* 无效化 icache */
        "dsb sy\n"
        "isb\n");
    __asm__ volatile(
        "mov x0, %1\n"          /* x0 = bootinfo 指针（内核第一个参数） */
        "br  %0\n" ::"r"(entry), "r"((UINT64)(UINTN)&g_bi)
        : "x0", "memory");
    __builtin_unreachable();
}

EFI_STATUS
EFIAPI
efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_STATUS st;
    EFI_LOADED_IMAGE *loaded = NULL;
    EFI_FILE_IO_INTERFACE *fs = NULL;
    EFI_FILE_HANDLE root = NULL, f = NULL;
    EFI_FILE_INFO *info = NULL;
    VOID *buf = NULL;
    VOID *mapbuf = NULL;
    UINTN info_size, fsize, read_len, map_size = 0, map_key = 0, desc_size = 0;
    UINT32 desc_ver = 0;
    UINT64 entry = 0, klo = 0, khi = 0, stub_lo = 0, stub_hi = 0;
    Elf64_Ehdr *eh;
    Elf64_Phdr *ph;

    /* ⚠️ 必须最先调 InitializeLib：gnu-efi 的 gBS / gST / gImageHandle 全在这里赋值，
     *    不调的话后面 gBS->HandleProtocol 就是空指针 → 跳到数据段崩（踩过）。 */
    InitializeLib(ImageHandle, SystemTable);

    o_reset(); o_str(L"[stub] LoongOS UEFI loader\r\n"); o_flush(SystemTable);

    /* 1. 先拿到本镜像信息（后面判"会不会覆盖自己"要用） */
    st = gBS->HandleProtocol(ImageHandle, &LoadedImageProtocol, (VOID **)&loaded);
    if (EFI_ERROR(st)) {
        o_reset(); o_str(L"[stub] loaded-image fail\r\n"); o_flush(SystemTable);
        return st;
    }
    stub_lo = (UINT64)(UINTN)loaded->ImageBase;
    stub_hi = stub_lo + (UINT64)loaded->ImageSize;
    o_line(SystemTable, L"[stub] stub image lo/hi/size", stub_lo, stub_hi,
           (UINT64)loaded->ImageSize);

    /* 2. 打开本镜像所在 FAT 卷 */
    st = gBS->HandleProtocol(loaded->DeviceHandle, &FileSystemProtocol, (VOID **)&fs);
    if (EFI_ERROR(st)) {
        o_reset(); o_str(L"[stub] sfs fail\r\n"); o_flush(SystemTable);
        return st;
    }
    st = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(st)) {
        o_reset(); o_str(L"[stub] openvolume fail\r\n"); o_flush(SystemTable);
        return st;
    }

    /* 3. 读 kernel.elf（FAT 用反斜杠路径） */
    st = root->Open(root, &f, L"\\kernel.elf", EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st)) {
        o_reset(); o_str(L"[stub] open kernel.elf fail\r\n"); o_flush(SystemTable);
        return st;
    }
    info_size = 0;
    f->GetInfo(f, &GenericFileInfo, &info_size, NULL);      /* 首调只求尺寸 */
    st = gBS->AllocatePool(EfiLoaderData, info_size, (VOID **)&info);
    if (EFI_ERROR(st)) return st;
    st = f->GetInfo(f, &GenericFileInfo, &info_size, info);
    if (EFI_ERROR(st)) return st;
    fsize = (UINTN)info->FileSize;
    st = gBS->AllocatePool(EfiLoaderData, fsize, &buf);
    if (EFI_ERROR(st)) return st;
    read_len = fsize;
    st = f->Read(f, &read_len, buf);
    if (EFI_ERROR(st) || read_len != fsize) {
        o_reset(); o_str(L"[stub] read kernel.elf fail\r\n"); o_flush(SystemTable);
        return EFI_LOAD_ERROR;
    }
    o_line(SystemTable, L"[stub] kernel.elf bytes", (UINT64)fsize, 0, 0);

    /* 3b. 顺便把 esp.img（4MB FAT，含 BOOTAA64.EFI + kernel.elf）读进内存，
     *     交给内核的安装器写盘 —— 这样内核里不需要任何 FAT / 分区写入代码。
     *     装好的系统从硬盘启动时，硬盘上不会有 esp.img，这里跳过即可（不算错）。 */
    {
        EFI_FILE_PROTOCOL *f2 = NULL;
        UINT8 infobuf[256];
        UINTN isz = sizeof(infobuf);
        st = root->Open(root, &f2, L"\\payload.img", EFI_FILE_MODE_READ, 0);
        if (!EFI_ERROR(st)) {
            EFI_FILE_INFO *fi = (EFI_FILE_INFO *)infobuf;
            st = f2->GetInfo(f2, &GenericFileInfo, &isz, infobuf);
            if (!EFI_ERROR(st)) {
                UINT64 isize = (UINT64)fi->FileSize;
                EFI_PHYSICAL_ADDRESS addr = 0;
                st = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData,
                                        EFI_SIZE_TO_PAGES(isize), &addr);
                if (!EFI_ERROR(st)) {
                    UINTN rl = (UINTN)isize;
                    st = f2->Read(f2, &rl, (VOID *)(UINTN)addr);
                    if (!EFI_ERROR(st) && rl == (UINTN)isize) {
                        g_bi.img_addr = (UINT64)addr;
                        g_bi.img_size = isize;
                        o_line(SystemTable, L"[stub] payload.img bytes", isize, 0, 0);
                    } else {
                        o_str(L"[stub] payload.img read fail\r"); o_flush(SystemTable);
                    }
                } else {
                    o_str(L"[stub] payload.img alloc fail\r"); o_flush(SystemTable);
                }
            }
            f2->Close(f2);
        } else {
            o_str(L"[stub] no payload.img (installed disk?)\r"); o_flush(SystemTable);
        }
    }

    /* 4. 校验 ELF 头（aarch64） */
    eh = (Elf64_Ehdr *)buf;
    if (read_len < sizeof(Elf64_Ehdr) || eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F' || eh->e_machine != 0xB7 /* EM_AARCH64 */) {
        o_reset(); o_str(L"[stub] bad ELF\r\n"); o_flush(SystemTable);
        return EFI_LOAD_ERROR;
    }
    entry = eh->e_entry;

    /* 5. 算内核要占的物理范围，检查是否与自己重叠 */
    ph = (Elf64_Phdr *)((UINT8 *)buf + eh->e_phoff);
    klo = ~(UINT64)0; khi = 0;
    for (UINT16 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_paddr < klo) klo = ph[i].p_paddr;
        if (ph[i].p_paddr + ph[i].p_memsz > khi) khi = ph[i].p_paddr + ph[i].p_memsz;
    }
    o_line(SystemTable, L"[stub] entry/target lo/hi", entry, klo, khi);

    if (klo < stub_hi && khi > stub_lo) {
        /* 固件把 stub 自己加载到了内核要占的地址上 —— 再拷就是自毁（真机卡死过） */
        o_reset();
        o_str(L"[stub] !! OVERLAP stub/kernel -> ABORT (no copy, safe)\r\n");
        o_flush(SystemTable);
        o_line(SystemTable, L"[stub] stub lo/hi", stub_lo, stub_hi, 0);
        o_line(SystemTable, L"[stub] kern lo/hi", klo, khi, 0);
        return EFI_LOAD_ERROR;
    }

    /* 6. 按 program header 把 PT_LOAD 段拷到 paddr，bss 尾清零 */
    for (UINT16 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        o_line(SystemTable, L"[stub]  seg paddr/filesz/memsz",
               (UINT64)ph[i].p_paddr, (UINT64)ph[i].p_filesz, (UINT64)ph[i].p_memsz);
        CopyMem((VOID *)ph[i].p_paddr, (UINT8 *)buf + ph[i].p_offset, (UINTN)ph[i].p_filesz);
        if (ph[i].p_memsz > ph[i].p_filesz)
            SetMem((UINT8 *)ph[i].p_paddr + ph[i].p_filesz,
                   (UINTN)(ph[i].p_memsz - ph[i].p_filesz), 0);
    }
    /* 关键：把刚写进去的内核段清出 cache（KVM 下缓存是开的） */
    clean_dcache(klo & ~(UINT64)63, khi);
    /* ⚠️ 也必须清 bootinfo：payload.img 的 img_addr/img_size 是刚写进 g_bi 的，
       不清就关缓存 → 内核从 DRAM 读到旧值 0 → 安装器报「缺少 ESP 载荷」（踩过）。 */
    clean_dcache((UINT64)&g_bi, (UINT64)&g_bi + sizeof(g_bi));
    o_reset(); o_str(L"[stub] segments copied + dcache cleaned\r\n"); o_flush(SystemTable);

    /* 7. 取内存映射（诊断 + 退出引导服务要用） */
    gBS->GetMemoryMap(&map_size, NULL, &map_key, &desc_size, &desc_ver);
    map_size += desc_size * 4;                              /* 留余量避免 EFI_INVALID_PARAMETER */
    st = gBS->AllocatePool(EfiLoaderData, map_size, &mapbuf);
    if (EFI_ERROR(st)) return st;
    st = gBS->GetMemoryMap(&map_size, mapbuf, &map_key, &desc_size, &desc_ver);
    if (EFI_ERROR(st)) return st;

    /* 诊断：内核装载地址是否落在一段 RAM 里 */
    {
        UINTN n = map_size / desc_size, found = 0;
        for (UINTN i = 0; i < n; i++) {
            EFI_MEMORY_DESCRIPTOR *e =
                (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)mapbuf + i * desc_size);
            UINT64 s0 = e->PhysicalStart;
            UINT64 s1 = s0 + (e->NumberOfPages << 12);
            if (s0 <= KERNEL_BASE && KERNEL_BASE < s1) {
                o_reset();
                o_str(L"[stub] 0x40080000 in mem type=");
                o_dec((UINT64)e->Type);
                o_str(L" range");
                o_str(L" "); o_hex(s0);
                o_str(L" .. "); o_hex(s1);
                o_str(L"\r\n");
                o_flush(SystemTable);
                found = 1;
            }
        }
        if (!found) {
            o_reset();
            o_str(L"[stub] !! 0x40080000 NOT in any RAM descriptor, entries=");
            o_dec((UINT64)n);
            o_str(L"\r\n");
            o_flush(SystemTable);
        }
    }

    /* 7b. 取 GOP 帧缓冲信息，供内核做图形输出
     *     （用户真机实测：虚拟机窗口显示的是图形控制台，内核写串口看不见） */
    {
        EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
        st = gBS->LocateProtocol(&GraphicsOutputProtocol, NULL, (VOID **)&gop);
        if (!EFI_ERROR(st) && gop && gop->Mode && gop->Mode->Info) {
            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi = gop->Mode->Info;
            g_bi.magic  = BOOTINFO_MAGIC;
            g_bi.width  = mi->HorizontalResolution;
            g_bi.height = mi->VerticalResolution;
            g_bi.stride = mi->PixelsPerScanLine;
            g_bi.bpp    = 32;
            g_bi.pixfmt = (mi->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) ? 1 : 0;
            g_bi.base   = gop->Mode->FrameBufferBase;
            g_bi.size   = gop->Mode->FrameBufferSize;
            o_reset();
            o_str(L"[stub] GOP fb ");  o_hex(g_bi.base);
            o_str(L" "); o_dec(g_bi.width); o_str(L"x"); o_dec(g_bi.height);
            o_str(L" fmt="); o_dec(g_bi.pixfmt);
            o_str(L"\r");
            o_flush(SystemTable);
        } else {
            /* ⚠️ 没有 GOP 也要给 magic！安装器要从 bootinfo 拿 payload.img 的地址，
               原来这里写 0 会让 `install` 误判"缺少 ESP 载荷"（2026-10-25 踩）。 */
            g_bi.magic = BOOTINFO_MAGIC;
            g_bi.width = g_bi.height = g_bi.stride = g_bi.bpp = 0;
            g_bi.base = 0;
            g_bi.size = 0;
            o_reset(); o_str(L"[stub] no GOP (serial only)\r"); o_flush(SystemTable);
        }
    }

    /* 8. 判定显示通道（关键顺序：ConOut 那行先写，PL011 标记最后写）
     *    · 看到 PL011-222 开头  -> 窗口显示的就是串口（内核输出本该可见，看不见=真崩）
     *    · 看到 CONOUT-111 开头 -> 窗口显示的是图形控制台（内核写串口不可见，其实还活着） */
    o_reset(); o_str(L"\rCONOUT-111 target "); o_hex(entry); o_str(L"\r");
    o_flush(SystemTable);
    pl011_write("\rPL011-222 \r");
    st = gBS->ExitBootServices(ImageHandle, map_key);
    if (EFI_ERROR(st)) {                                    /* 映射已变 → 重取再退一次 */
        gBS->GetMemoryMap(&map_size, mapbuf, &map_key, &desc_size, &desc_ver);
        st = gBS->ExitBootServices(ImageHandle, map_key);
        if (EFI_ERROR(st)) return st;
    }

    /* 10. 引导服务已停，此后只能裸跳（不再调用任何 gBS / ConOut） */
    drop_el2_mmu();      /* KVM 下我们可能还在 EL2，先关掉 EL2 的 MMU */
    enter_kernel(entry);
    return EFI_LOAD_ERROR;   /* 不可达 */
}
