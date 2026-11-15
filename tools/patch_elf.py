from pathlib import Path

p = Path('native-elf-loader-next/elf_loader.c')
s = p.read_text(encoding='utf-8')
start = s.index('static int map_segments(struct elf_loader *l, const struct file_image *fi,')
end = s.index('/* ------------------------------------------------------------------ */\n/* 动态段解析', start)

new = r'''static int map_segments(struct elf_loader *l, const struct file_image *fi,
                        const Elf64_Ehdr *eh)
{
    const Elf64_Phdr *ph;
    uint64_t page = (uint64_t)page_size();
    uint64_t min_vaddr = UINT64_MAX;
    uint64_t max_vaddr = 0;
    uint64_t map_size;
    uint64_t i;
    void *map;

    if (!l || !fi || !eh || page == 0) return 0;
    ph = (const Elf64_Phdr *)((const unsigned char *)fi->map + eh->e_phoff);

    for (i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_memsz == 0) continue;
        if (ph[i].p_filesz > ph[i].p_memsz) {
            loader_fail(l, "map", "PT_LOAD[%llu] p_filesz > p_memsz",
                        (unsigned long long)i);
            return 0;
        }
        if (ph[i].p_offset > fi->size ||
            ph[i].p_filesz > fi->size - ph[i].p_offset) {
            loader_fail(l, "map", "PT_LOAD[%llu] 文件范围越界",
                        (unsigned long long)i);
            return 0;
        }
        if (ph[i].p_vaddr > UINT64_MAX - ph[i].p_memsz) {
            loader_fail(l, "map", "PT_LOAD[%llu] vaddr+memsz 溢出",
                        (unsigned long long)i);
            return 0;
        }
        if (ph[i].p_vaddr < min_vaddr) min_vaddr = ph[i].p_vaddr;
        if (ph[i].p_vaddr + ph[i].p_memsz > max_vaddr)
            max_vaddr = ph[i].p_vaddr + ph[i].p_memsz;
    }
    if (min_vaddr == UINT64_MAX || max_vaddr <= min_vaddr) {
        loader_fail(l, "map", "没有有效 PT_LOAD");
        return 0;
    }
    if (!align_down_u64(min_vaddr, page, &min_vaddr) ||
        !align_up_u64(max_vaddr, page, &max_vaddr)) {
        loader_fail(l, "map", "PT_LOAD 页对齐溢出");
        return 0;
    }
    if (max_vaddr <= min_vaddr || max_vaddr - min_vaddr > ELF_LOADER_MAX_MAP_SIZE) {
        loader_fail(l, "map", "PT_LOAD 映射跨度非法");
        return 0;
    }
    map_size = max_vaddr - min_vaddr;
    map = mmap(NULL, (size_t)map_size, PROT_NONE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED) {
        loader_fail(l, "map", "保留 PROT_NONE 映射失败（size=%llu errno=%d）",
                    (unsigned long long)map_size, errno);
        return 0;
    }
    l->map_base = map;
    l->map_size = (size_t)map_size;
    l->load_bias = (uintptr_t)map - (uintptr_t)min_vaddr;
    l->segment_count = 0;

    /* OHOS rejects adding PROT_EXEC with mprotect on an anonymous reservation
     * (observed EINVAL at the first executable page).  Build each PT_LOAD as
     * a file-backed mapping with its final permissions instead; the file is
     * still the original .so, no bytes are patched.  BSS pages are anonymous
     * RW and are never executable. */
    for (i = 0; i < eh->e_phnum; ++i) {
        uint64_t seg_start_v, seg_end_v, seg_span;
        uint64_t file_off, file_end_v, file_span;
        uint64_t bss_begin, bss_end;
        uintptr_t seg_addr;
        int prot = 0;

        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) continue;
        if (ph[i].p_flags & PF_R) prot |= PROT_READ;
        if (ph[i].p_flags & PF_W) prot |= PROT_WRITE;
        if (ph[i].p_flags & PF_X) prot |= PROT_EXEC;
        if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) {
            loader_fail(l, "map", "PT_LOAD[%llu] 同时要求 W/X（W^X）",
                        (unsigned long long)i);
            return 0;
        }
        if (!align_down_u64(ph[i].p_vaddr, page, &seg_start_v) ||
            !align_up_u64(ph[i].p_vaddr + ph[i].p_memsz, page, &seg_end_v) ||
            seg_end_v <= seg_start_v) {
            loader_fail(l, "map", "PT_LOAD[%llu] 段页对齐失败",
                        (unsigned long long)i);
            return 0;
        }
        seg_span = seg_end_v - seg_start_v;
        seg_addr = (uintptr_t)map + (uintptr_t)(seg_start_v - min_vaddr);

        file_off = ph[i].p_offset & ~(page - 1);
        if (!align_up_u64(ph[i].p_offset + ph[i].p_filesz, page, &file_end_v) ||
            file_end_v < file_off) {
            loader_fail(l, "map", "PT_LOAD[%llu] 文件页对齐失败",
                        (unsigned long long)i);
            return 0;
        }
        file_span = file_end_v - file_off;
        if (file_span > seg_span) file_span = seg_span;
        if (file_span) {
            void *got = mmap((void *)seg_addr, (size_t)file_span, prot,
                             MAP_FIXED | MAP_PRIVATE, fi->fd, (off_t)file_off);
            if (got == MAP_FAILED) {
                loader_fail(l, "map", "文件段映射失败（seg=%llu off=%llu len=%llu prot=%d errno=%d）",
                            (unsigned long long)i, (unsigned long long)file_off,
                            (unsigned long long)file_span, prot, errno);
                return 0;
            }
        }

        /* BSS begins at the first page after the file-backed bytes.  A pure
         * BSS PT_LOAD starts at its (possibly unaligned) virtual address. */
        bss_begin = ph[i].p_filesz
                        ? ((ph[i].p_vaddr + ph[i].p_filesz + page - 1) & ~(page - 1))
                        : (ph[i].p_vaddr & ~(page - 1));
        if (!align_up_u64(ph[i].p_vaddr + ph[i].p_memsz, page, &bss_end)) {
            loader_fail(l, "map", "PT_LOAD[%llu] BSS 页对齐失败",
                        (unsigned long long)i);
            return 0;
        }
        if (bss_end > bss_begin) {
            void *got = mmap((void *)((uintptr_t)map + (uintptr_t)(bss_begin - min_vaddr)),
                             (size_t)(bss_end - bss_begin), PROT_READ | PROT_WRITE,
                             MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (got == MAP_FAILED) {
                loader_fail(l, "map", "BSS 映射失败（seg=%llu len=%llu errno=%d）",
                            (unsigned long long)i,
                            (unsigned long long)(bss_end - bss_begin), errno);
                return 0;
            }
        }

        /* Zero the tail of the last file page when it contains BSS bytes.  A
         * writable data segment can be cleared in place; a read-only/exec
         * page with a non-page-aligned BSS tail is refused rather than
         * executed with stale bytes. */
        if (ph[i].p_memsz > ph[i].p_filesz) {
            uint64_t tail = (ph[i].p_vaddr + ph[i].p_filesz) & (page - 1);
            if (tail) {
                uint64_t n = page - tail;
                if (n > ph[i].p_memsz - ph[i].p_filesz)
                    n = ph[i].p_memsz - ph[i].p_filesz;
                if (prot & PROT_WRITE) {
                    memset((unsigned char *)map +
                               (size_t)(ph[i].p_vaddr - min_vaddr + ph[i].p_filesz),
                           0, (size_t)n);
                } else {
                    loader_fail(l, "map", "PT_LOAD[%llu] 只读段含未对齐 BSS 尾部",
                                (unsigned long long)i);
                    return 0;
                }
            }
        }

        if (l->segment_count >= ELF_LOADER_MAX_SEGMENTS) {
            loader_fail(l, "map", "PT_LOAD 数量超过内部上限");
            return 0;
        }
        l->segments[l->segment_count].start = (uintptr_t)(l->load_bias + ph[i].p_vaddr);
        l->segments[l->segment_count].end =
            l->segments[l->segment_count].start + (uintptr_t)ph[i].p_memsz;
        l->segments[l->segment_count].prot = prot;
        ++l->segment_count;
    }

    for (i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type == PT_DYNAMIC) {
            l->dynamic_vaddr = (uintptr_t)ph[i].p_vaddr;
            l->dynamic_size = (size_t)ph[i].p_filesz;
            break;
        }
    }
    if (l->dynamic_vaddr == 0 || l->dynamic_size < sizeof(Elf64_Dyn)) {
        loader_fail(l, "map", "缺少有效 PT_DYNAMIC");
        return 0;
    }
    return 1;
}

'''

s = s[:start] + new + s[end:]
p.write_text(s, encoding='utf-8')
print('map_segments replaced')
