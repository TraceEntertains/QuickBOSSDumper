#include "disadiff.h"
#include "sha.h"
#include "ff.h"
#include "sddata.h"

#define GET_DPFS_BIT(b, lvl) (((((u32*) (void*) lvl)[b >> 5]) >> (31 - (b % 32))) & 1)

typedef struct {
    u8  magic[8]; // "DISA" 0x00040000
    u32 n_partitions;
    u8  padding0[4];
    u64 offset_table1;
    u64 offset_table0;
    u64 size_table;
    u64 offset_descA;
    u64 size_descA;
    u64 offset_descB;
    u64 size_descB;
    u64 offset_partitionA;
    u64 size_partitionA;
    u64 offset_partitionB;
    u64 size_partitionB;
    u8  active_table; // 0 or 1
    u8  padding1[3];
    u8  hash_table[0x20]; // for the active table
    u8  unused[0x74];
} __attribute__((packed, aligned(4))) DisaHeader;

typedef struct {
    u8  magic[8]; // "DIFF" 0x00030000
    u64 offset_table1; // also desc offset
    u64 offset_table0; // also desc offset
    u64 size_table; // includes desc size
    u64 offset_partition;
    u64 size_partition;
    u32 active_table; // 0 or 1
    u8  hash_table[0x20]; // for the active table
    u64 unique_id; // see: http://3dbrew.org/wiki/Extdata
    u8  unused[0xA4];
} __attribute__((packed, aligned(4))) DiffHeader;

typedef struct {
    u8  magic[8]; // "DIFI" 0x00010000
    u64 offset_ivfc; // always 0x44
    u64 size_ivfc; // always 0x78
    u64 offset_dpfs; // always 0xBC
    u64 size_dpfs; // always 0x50
    u64 offset_hash; // always 0x10C
    u64 size_hash; // may include padding
    u8  ivfc_use_extlvl4;
    u8  dpfs_lvl1_selector;
    u8  padding[2];
    u64 ivfc_offset_extlvl4;
} __attribute__((packed, aligned(4))) DifiHeader;

typedef struct {
    u8  magic[8]; // "IVFC" 0x00020000
    u64 size_hash; // same as the one in DIFI, may include padding
    u64 offset_lvl1;
    u64 size_lvl1;
    u32 log_lvl1;
    u8  padding0[4];
    u64 offset_lvl2;
    u64 size_lvl2;
    u32 log_lvl2;
    u8  padding1[4];
    u64 offset_lvl3;
    u64 size_lvl3;
    u32 log_lvl3;
    u8  padding2[4];
    u64 offset_lvl4;
    u64 size_lvl4;
    u64 log_lvl4;
    u64 size_ivfc; // 0x78
} __attribute__((packed, aligned(4))) IvfcDescriptor;

typedef struct {
    u8  magic[8]; // "DPFS" 0x00010000
    u64 offset_lvl1;
    u64 size_lvl1;
    u32 log_lvl1;
    u8  padding0[4];
    u64 offset_lvl2;
    u64 size_lvl2;
    u32 log_lvl2;
    u8  padding1[4];
    u64 offset_lvl3;
    u64 size_lvl3;
    u32 log_lvl3;
    u8  padding2[4];
} __attribute__((packed, aligned(4))) DpfsDescriptor;

typedef struct {
    DifiHeader difi;
    IvfcDescriptor ivfc;
    DpfsDescriptor dpfs;
    u8 hash[0x20];
    u8 padding[4]; // all zeroes when encrypted
} __attribute__((packed, aligned(4))) DifiStruct;

static FIL ddfile;
static FIL* ddfp = NULL;

inline static u32 DisaDiffSize(const TCHAR* path) {
    FILINFO fno;
    return (fa_stat(path, &fno) == FR_OK) ? fno.fsize : 0;
}

inline static FRESULT DisaDiffOpen(const TCHAR* path) {
    FRESULT res = FR_OK;

    ddfp = NULL;
    if (path) {
        res = fx_open(&ddfile, path, FA_READ | FA_WRITE | FA_OPEN_EXISTING);
        if (res == FR_OK) ddfp = &ddfile;
    }

    return res;
}

inline static FRESULT DisaDiffRead(void* buf, UINT btr, UINT ofs) {
    if (ddfp) {
        FRESULT res;
        UINT br;
        if ((f_tell(ddfp) != ofs) &&
            (f_lseek(ddfp, ofs) != FR_OK)) return FR_DENIED;
        res = fx_read(ddfp, buf, btr, &br);
        if ((res == FR_OK) && (br != btr)) res = FR_DENIED;
        return res;
    } return FR_DENIED;
}

inline static FRESULT DisaDiffWrite(const void* buf, UINT btw, UINT ofs) {
    if (ddfp) {
        FRESULT res;
        UINT bw;
        if ((ddfp->fptr != ofs) &&
            (f_lseek(ddfp, ofs) != FR_OK)) return FR_DENIED;
        res = fx_write(ddfp, buf, btw, &bw);
        if ((res == FR_OK) && (bw != btw)) res = FR_DENIED;
        return res;
    } return FR_DENIED;
}

inline static FRESULT DisaDiffClose() {
    if (ddfp) {
        ddfp = NULL;
        return fx_close(&ddfile);
    } else return FR_OK;
}

inline static FRESULT DisaDiffQRead(const TCHAR* path, void* buf, UINT ofs, UINT btr) {
    return fx_qread(path, buf, ofs, btr, NULL);
}

inline static FRESULT DisaDiffQWrite(const TCHAR* path, const void* buf, UINT ofs, UINT btw) {
    return fx_qwrite(path, buf, ofs, btw, NULL);
}

u32 GetDisaDiffRWInfo(const char* path, DisaDiffRWInfo* info, bool partitionB) {
    static const u8 disa_magic[] = { DISA_MAGIC };
    static const u8 diff_magic[] = { DIFF_MAGIC };
    static const u8 ivfc_magic[] = { IVFC_MAGIC };
    static const u8 dpfs_magic[] = { DPFS_MAGIC };
    static const u8 difi_magic[] = { DIFI_MAGIC };

    // reset reader info
    memset(info, 0x00, sizeof(DisaDiffRWInfo));

    // get file size, header at header offset
    u32 file_size = DisaDiffSize(path);
    u8 header[0x100];
    if (DisaDiffQRead(path, header, 0x100, 0x100) != FR_OK)
        return 1;

    // DISA/DIFF header: find partition offset & size and DIFI descriptor
    u32 offset_partition = 0;
    u32 size_partition = 0;
    u32 offset_difi = 0;
    if (memcmp(header, disa_magic, 8) == 0) { // DISA file
        DisaHeader* disa = (DisaHeader*) (void*) header;
        info->offset_table = (disa->active_table) ? disa->offset_table1 : disa->offset_table0;
        info->size_table = disa->size_table;
        offset_difi = info->offset_table;
        info->offset_partition_hash = 0x16C;
        if (!partitionB) {
            offset_partition = (u32) disa->offset_partitionA;
            size_partition = (u32) disa->size_partitionA;
            offset_difi += (u32) disa->offset_descA;
        } else {
            if (disa->n_partitions != 2) return 1;
            offset_partition = (u32) disa->offset_partitionB;
            size_partition = (u32) disa->size_partitionB;
            offset_difi += (u32) disa->offset_descB;
        }
    } else if (memcmp(header, diff_magic, 8) == 0) { // DIFF file
        if (partitionB)
            return 1;
        DiffHeader* diff = (DiffHeader*) (void*) header;
        offset_partition = (u32) diff->offset_partition;
        size_partition = (u32) diff->size_partition;
        info->offset_table = (diff->active_table) ? diff->offset_table1 : diff->offset_table0;
        info->size_table = diff->size_table;
        offset_difi = info->offset_table;
        info->offset_partition_hash = 0x134;
    } else {
        return 1;
    }

    // check the output so far
    if (!offset_difi || (offset_difi + sizeof(DifiStruct) > file_size) || (offset_partition + size_partition > file_size))
        return 1;

    info->offset_difi = offset_difi;
    // read DIFI struct from file
    const DifiStruct difis;
    if (DisaDiffQRead(path, (DifiStruct*) &difis, offset_difi, sizeof(DifiStruct)) != FR_OK)
        return 1;

    if ((memcmp(difis.difi.magic, difi_magic, 8) != 0) ||
        (memcmp(difis.ivfc.magic, ivfc_magic, 8) != 0) ||
        (memcmp(difis.dpfs.magic, dpfs_magic, 8) != 0))
        return 1;

    // check & get data from DIFI header
    const DifiHeader* difi = &(difis.difi);
    if ((difi->offset_ivfc != sizeof(DifiHeader)) ||
        (difi->size_ivfc != sizeof(IvfcDescriptor)) ||
        (difi->offset_dpfs != difi->offset_ivfc + difi->size_ivfc) ||
        (difi->size_dpfs != sizeof(DpfsDescriptor)) ||
        (difi->offset_hash != difi->offset_dpfs + difi->size_dpfs) ||
        (difi->size_hash < 0x20))
        return 1;

    info->dpfs_lvl1_selector = difi->dpfs_lvl1_selector;
    info->ivfc_use_extlvl4 = difi->ivfc_use_extlvl4;
    info->offset_ivfc_lvl4 = (u32) (offset_partition + difi->ivfc_offset_extlvl4);
    info->offset_master_hash = (u32) difi->offset_hash;

    // check & get data from DPFS descriptor
    const DpfsDescriptor* dpfs = &(difis.dpfs);
    if ((dpfs->offset_lvl1 + dpfs->size_lvl1 > dpfs->offset_lvl2) ||
        (dpfs->offset_lvl2 + dpfs->size_lvl2 > dpfs->offset_lvl3) ||
        (dpfs->offset_lvl3 + dpfs->size_lvl3 > size_partition) ||
        (2 > dpfs->log_lvl2) || (dpfs->log_lvl2 > dpfs->log_lvl3) ||
        !dpfs->size_lvl1 || !dpfs->size_lvl2 || !dpfs->size_lvl3)
        return 1;

    info->offset_dpfs_lvl1 = (u32) (offset_partition + dpfs->offset_lvl1);
    info->offset_dpfs_lvl2 = (u32) (offset_partition + dpfs->offset_lvl2);
    info->offset_dpfs_lvl3 = (u32) (offset_partition + dpfs->offset_lvl3);
    info->size_dpfs_lvl1 = (u32) dpfs->size_lvl1;
    info->size_dpfs_lvl2 = (u32) dpfs->size_lvl2;
    info->size_dpfs_lvl3 = (u32) dpfs->size_lvl3;
    info->log_dpfs_lvl2 = (u32) dpfs->log_lvl2;
    info->log_dpfs_lvl3 = (u32) dpfs->log_lvl3;

    // check & get data from IVFC descriptor
    const IvfcDescriptor* ivfc = &(difis.ivfc);
    if ((ivfc->size_hash != difi->size_hash) ||
        (ivfc->size_ivfc != sizeof(IvfcDescriptor)) ||
        (ivfc->offset_lvl1 + ivfc->size_lvl1 > ivfc->offset_lvl2) ||
        (ivfc->offset_lvl2 + ivfc->size_lvl2 > ivfc->offset_lvl3) ||
        (ivfc->offset_lvl3 + ivfc->size_lvl3 > dpfs->size_lvl3))
        return 1;

    if (!info->ivfc_use_extlvl4) {
        if ((ivfc->offset_lvl3 + ivfc->size_lvl3 > ivfc->offset_lvl4) ||
            (ivfc->offset_lvl4 + ivfc->size_lvl4 > dpfs->size_lvl3))
            return 1;

        info->offset_ivfc_lvl4 = (u32) ivfc->offset_lvl4;
    } else if (info->offset_ivfc_lvl4 + ivfc->size_lvl4 > offset_partition + size_partition)
        return 1;

    info->log_ivfc_lvl1 = (u32) ivfc->log_lvl1;
    info->log_ivfc_lvl2 = (u32) ivfc->log_lvl2;
    info->log_ivfc_lvl3 = (u32) ivfc->log_lvl3;
    info->log_ivfc_lvl4 = (u32) ivfc->log_lvl4;
    info->offset_ivfc_lvl1 = (u32) ivfc->offset_lvl1;
    info->offset_ivfc_lvl2 = (u32) ivfc->offset_lvl2;
    info->offset_ivfc_lvl3 = (u32) ivfc->offset_lvl3;
    info->size_ivfc_lvl1 = (u32) ivfc->size_lvl1;
    info->size_ivfc_lvl2 = (u32) ivfc->size_lvl2;
    info->size_ivfc_lvl3 = (u32) ivfc->size_lvl3;
    info->size_ivfc_lvl4 = (u32) ivfc->size_lvl4;

    return 0;
}

u32 BuildDisaDiffDpfsLvl2Cache(const char* path, const DisaDiffRWInfo* info, u8* cache, u32 cache_size) {
    const u32 min_cache_bits = (info->size_dpfs_lvl3 + (1 << info->log_dpfs_lvl3) - 1) >> info->log_dpfs_lvl3;
    const u32 min_cache_size = ((min_cache_bits + 31) >> (3 + 2)) << 2;
    const u32 offset_lvl1 = info->offset_dpfs_lvl1 + ((info->dpfs_lvl1_selector) ? info->size_dpfs_lvl1 : 0);

    // safety (this still assumes all the checks from GetDisaDiffRWInfo())
    if ((cache_size < min_cache_size) ||
        (min_cache_size > info->size_dpfs_lvl2) ||
        (min_cache_size > (info->size_dpfs_lvl1 << (3 + info->log_dpfs_lvl2)))) {
        return 1;
    }

    // allocate memory
    u8* lvl1 = (u8*) malloc(info->size_dpfs_lvl1);
    if (!lvl1) return 1; // this is never more than 8 byte in reality -___-

    // open file pointer
    if (DisaDiffOpen(path) != FR_OK) {
        free(lvl1);
        return 1;
    }

    // read lvl1
    u32 ret = 0;
    if ((ret != 0) || DisaDiffRead(lvl1, info->size_dpfs_lvl1, offset_lvl1)) ret = 1;

    // read full lvl2_0 to cache
    if ((ret != 0) || DisaDiffRead(cache, info->size_dpfs_lvl2, info->offset_dpfs_lvl2)) ret = 1;

    // cherry-pick lvl2_1
    u32 log_lvl2 = info->log_dpfs_lvl2;
    u32 offset_lvl2_1 = info->offset_dpfs_lvl2 + info->size_dpfs_lvl2;
    for (u32 i = 0; (ret == 0) && ((i << (3 + log_lvl2)) < min_cache_size); i += 4) {
        u32 dword = *(u32*) (void*) (lvl1 + i);
        for (u32 b = 0; b < 32; b++) {
            if ((dword >> (31 - b)) & 1) {
                u32 offset = ((i << 3) + b) << log_lvl2;
                if (DisaDiffRead((u8*) cache + offset, 1 << log_lvl2, offset_lvl2_1 + offset) != FR_OK) ret = 1;
            }
        }
    }

    ((DisaDiffRWInfo*) info)->dpfs_lvl2_cache = cache;
    free(lvl1);
    DisaDiffClose();
    return ret;
}

static u32 ReadDisaDiffDpfsLvl3(const DisaDiffRWInfo* info, u32 offset, u32 size, void* buffer) { // assumes file is already open
    const u32 offset_start = offset;
    const u32 offset_end = offset_start + size;
    const u8* lvl2 = info->dpfs_lvl2_cache;
    const u32 offset_lvl3_0 = info->offset_dpfs_lvl3;
    const u32 offset_lvl3_1 = offset_lvl3_0 + info->size_dpfs_lvl3;
    const u32 log_lvl3 = info->log_dpfs_lvl3;

    u32 read_start = offset_start;
    u32 read_end = read_start;
    u32 bit_state = 0;

    // full reading below
    while (size && (read_start < offset_end)) {
        // read bits until bit_state does not match
        // idx_lvl2 is a bit offset
        u32 idx_lvl2 = read_end >> log_lvl3;
        if (GET_DPFS_BIT(idx_lvl2, lvl2) == bit_state) {
            read_end = (idx_lvl2+1) << log_lvl3;
            if (read_end >= offset_end) read_end = offset_end;
            else continue;
        }
        // read data if there is any
        if (read_start < read_end) {
            const u32 pos_f = (bit_state ? offset_lvl3_1 : offset_lvl3_0) + read_start;
            const u32 pos_b = read_start - offset_start;
            const u32 btr = read_end - read_start;
            if (DisaDiffRead(((u8*) buffer) + pos_b, btr, pos_f) != FR_OK) size = 0;
            read_start = read_end;
        }
        // flip the bit_state
        bit_state = ~bit_state & 0x1;
    }

    return size;
}

static u32 WriteDisaDiffDpfsLvl3(const DisaDiffRWInfo* info, u32 offset, u32 size, const void* buffer) { // assumes file is already open, does not fix hashes
    const u32 offset_start = offset;
    const u32 offset_end = offset_start + size;
    const u8* lvl2 = info->dpfs_lvl2_cache;
    const u32 offset_lvl3_0 = info->offset_dpfs_lvl3;
    const u32 offset_lvl3_1 = offset_lvl3_0 + info->size_dpfs_lvl3;
    const u32 log_lvl3 = info->log_dpfs_lvl3;

    u32 write_start = offset_start;
    u32 write_end = write_start;
    u32 bit_state = 0;

    // full reading below
    while (size && (write_start < offset_end)) {
        // write bits until bit_state does not match
        // idx_lvl2 is a bit offset
        u32 idx_lvl2 = write_end >> log_lvl3;
        if (GET_DPFS_BIT(idx_lvl2, lvl2) == bit_state) {
            write_end = (idx_lvl2+1) << log_lvl3;
            if (write_end >= offset_end) write_end = offset_end;
            else continue;
        }
        // write data if there is any
        if (write_start < write_end) {
            const u32 pos_f = (bit_state ? offset_lvl3_1 : offset_lvl3_0) + write_start;
            const u32 pos_b = write_start - offset_start;
            const u32 btw = write_end - write_start;
            if (DisaDiffWrite(((u8*) buffer) + pos_b, btw, pos_f) != FR_OK) size = 0;
            write_start = write_end;
        }
        // flip the bit_state
        bit_state = ~bit_state & 0x1;
    }

    return size;
}

u32 FixDisaDiffPartitionHash(const DisaDiffRWInfo* info) {
    const u32 size = info->size_table;
    u8 sha_buf[0x20];
    u8* buf;

    if (!(buf = malloc(size)))
        return 1;

    if (DisaDiffRead(buf, size, info->offset_table) != FR_OK) {
        free(buf);
        return 1;
    }

    sha_quick(sha_buf, buf, size, SHA256_MODE);

    free(buf);

    if (DisaDiffWrite(sha_buf, 0x20, info->offset_partition_hash) != FR_OK)
        return 1;

    return 0;
}

u32 FixDisaDiffIvfcLevel(const DisaDiffRWInfo* info, u32 level, u32 offset, u32 size, u32* next_offset, u32* next_size) {
    if (level == 0)
        return FixDisaDiffPartitionHash(info);

    if (level > 4)
        return 1;

    const u32 offset_ivfc_lvl = (&(info->offset_ivfc_lvl1))[level - 1];
    const u32 size_ivfc_lvl = (&(info->size_ivfc_lvl1))[level - 1];
    const u32 log_ivfc_lvl = (&(info->log_ivfc_lvl1))[level - 1];
    const u32 block_size = 1 << log_ivfc_lvl;
    u32 read_size = block_size;
    u32 align_offset = (offset >> log_ivfc_lvl) << log_ivfc_lvl; // align starting offset
    u32 align_size = size + offset - align_offset; // increase size by the amount starting offset decreased when aligned

    if (level != 1) {
        if (next_offset) *next_offset = (align_offset >> log_ivfc_lvl) * 0x20;
        if (next_size) *next_size = ((align_size >> log_ivfc_lvl) + (((align_size % block_size) == 0) ? 0 : 1)) * 0x20;
    }

    u8 sha_buf[0x20];
    u8* buf;

    if (!(buf = malloc(block_size)))
        return 1;

    while (align_size > 0) {
        if (align_offset + block_size > size_ivfc_lvl) {
            memset(buf, 0, block_size);
            read_size -= (align_offset + block_size - size_ivfc_lvl);
        }

        if (((level == 4) && info->ivfc_use_extlvl4) ? (DisaDiffRead(buf, read_size, align_offset + offset_ivfc_lvl) != FR_OK) :
            (ReadDisaDiffDpfsLvl3(info, align_offset + offset_ivfc_lvl, read_size, buf) != read_size)) {
            free(buf);
            return 1;
        }

        sha_quick(sha_buf, buf, block_size, SHA256_MODE);


        if ((level == 1) ? (DisaDiffWrite(sha_buf, 0x20, info->offset_difi + info->offset_master_hash + ((align_offset >> log_ivfc_lvl) * 0x20)) != FR_OK) :
            (WriteDisaDiffDpfsLvl3(info, (&(info->offset_ivfc_lvl1))[level - 2] + ((align_offset >> log_ivfc_lvl) * 0x20), 0x20, sha_buf) != 0x20)) {
            free(buf);
            return 1;
        }

        align_offset += block_size;
        align_size = ((align_size < block_size) ? 0 : (align_size - block_size));
    }

    free(buf);

    return 0;
}

u32 ReadDisaDiffIvfcLvl4(const char* path, const DisaDiffRWInfo* info, u32 offset, u32 size, void* buffer) { // offset: offset inside IVFC lvl4
    // DisaDiffRWInfo not provided?
    DisaDiffRWInfo info_l;
    u8* cache = NULL;
    if (!info) {
        info = &info_l;
        if (GetDisaDiffRWInfo(path, (DisaDiffRWInfo*) info, false) != 0) return 0;
        cache = malloc(info->size_dpfs_lvl2);
        if (!cache) return 0;
        if (BuildDisaDiffDpfsLvl2Cache(path, info, cache, info->size_dpfs_lvl2) != 0) {
            free(cache);
            return 0;
        }
    }

    // open file pointer
    if (DisaDiffOpen(path) != FR_OK)
        size = 0;

    // sanity checks - offset & size
    if (offset > info->size_ivfc_lvl4) return 0;
    else if (offset + size > info->size_ivfc_lvl4) size = info->size_ivfc_lvl4 - offset;

    if (info->ivfc_use_extlvl4) {
        if (DisaDiffRead(buffer, size, info->offset_ivfc_lvl4 + offset) != FR_OK)
            size = 0;
    } else {
        size = ReadDisaDiffDpfsLvl3(info, info->offset_ivfc_lvl4 + offset, size, buffer);
    }

    DisaDiffClose();
    if (cache) free(cache);
    return size;
}

u32 WriteDisaDiffIvfcLvl4(const char* path, const DisaDiffRWInfo* info, u32 offset, u32 size, const void* buffer) { // offset: offset inside IVFC lvl4. cmac still needs fixed after calling this.
    // DisaDiffRWInfo not provided?
    DisaDiffRWInfo info_l;
    u8* cache = NULL;
    if (!info) {
        info = &info_l;
        if (GetDisaDiffRWInfo(path, (DisaDiffRWInfo*) info, false) != 0)
            return 0;
        cache = malloc(info->size_dpfs_lvl2);
        if (!cache)
            return 0;
        if (BuildDisaDiffDpfsLvl2Cache(path, info, cache, info->size_dpfs_lvl2) != 0) {
            free(cache);
            return 0;
        }
    }

    // sanity check - offset & size
    if (offset + size > info->size_ivfc_lvl4)
        return 0;

    // open file pointer
    if (DisaDiffOpen(path) != FR_OK)
        size = 0;

    if (info->ivfc_use_extlvl4) {
        if (DisaDiffWrite(buffer, size, info->offset_ivfc_lvl4 + offset) != FR_OK)
            size = 0;
    } else {
        size = WriteDisaDiffDpfsLvl3(info, info->offset_ivfc_lvl4 + offset, size, buffer);
    }

    if ((size != 0) && ddfp) { // if we're writing to a mounted image, the hash chain will be handled later by vdisadiff
        u32 hashfix_offset = offset, hashfix_size = size;
        for (int i = 4; i >= 0; i--) {
            if (FixDisaDiffIvfcLevel(info, i, hashfix_offset, hashfix_size, &hashfix_offset, &hashfix_size) != 0) {
                size = 0;
                break;
            }
        }
    }

    DisaDiffClose();
    if (cache) free(cache);
    return size;
}