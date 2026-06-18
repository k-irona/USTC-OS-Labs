#include "types.h"
#include "defs.h"
#include "buf.h"
#include "file.h"
#include "spinlock.h"
#include "bptree.h"

#define FAT16_NAME_LEN 11
#define FAT16_DIR_ENTRY_SIZE 32
#define FAT16_ATTR_READONLY 0x01
#define FAT16_ATTR_HIDDEN 0x02
#define FAT16_ATTR_SYSTEM 0x04
#define FAT16_ATTR_VOLUME 0x08
#define FAT16_ATTR_DIRECTORY 0x10
#define FAT16_ATTR_KW 0x0f
#define FAT16_NAME_FREE 0x00
#define FAT16_NAME_DELETED 0xe5
#define FAT16_CLUSTER_FREE 0x0000
#define FAT16_CLUSTER_MIN 0x0002
#define FAT16_CLUSTER_EOC 0xfff8
#define FAT16_CLUSTER_END 0xffff

#define FAT16_PENDING_MAGIC 0x46415450U
#define FAT16_TEMP_INUM_BASE 0x80000000U
#define FAT16_ROOT_INUM ROOTINO
#define FAT16_ENTRIES_PER_SECTOR (BSIZE / FAT16_DIR_ENTRY_SIZE)

#define FAT16_KW_TYPE 0x90
#define FAT16_KW_TYPE_MASK 0xf0
#define FAT16_KW_MAX_ENTRIES 16
#define FAT16_KW_BYTES_PER_ENTRY 30
#define FAT16_KW_MAX_BYTES (FAT16_KW_MAX_ENTRIES * FAT16_KW_BYTES_PER_ENTRY)
#define FAT16_QUERY_MAX_TERMS 32

/* FAT16基础定义和全局状态 */

// 文件系统元数据，注意：以下数据是从BPB（BIOS Parameter Block，是磁盘的第一个扇区）中提取的，真实的BPB
// 并不是按照以下顺序组织的，对 meta 的初始化详见 fat16fs_fsinit() 函数
struct fat16_meta {
    uint dev;
    uint bytes_per_sec; // 每扇区字节数
    uint sec_per_clus; // 每簇扇区数
    uint reserved; // 保留扇区数，通常是1，也就是说只有BPB是保留区，保留扇区数也是FAT1的起始扇区号
    uint fats; // FAT表的个数，FAT1作主表，FAT2作备份
    uint root_entries; // 根目录下最大目录项数，这意味着根目录最多只能有如此多个文件/文件夹
    uint total_sec; // 磁盘总扇区数
    uint sec_per_fat; // 每个FAT表占用的扇区数
    uint fat_sec; // FAT1的起始扇区号
    uint root_sec; // 根目录项的起始扇区号
    uint root_sectors; // 根目录项的扇区数
    uint data_sec; // 数据段的起始扇区号
    uint clusters; // 数据段的总簇数
    uint cluster_size; // 每个簇的大小
};

// 文件目录项槽，表示一个文件目录项在磁盘上的位置，以及它的内容
struct fat16_slot {
    struct dirent entry; // 文件目录项
    uint sector; // 文件目录项所在的扇区号
    uint offset; // 文件目录项在所在的扇区内的字节偏移
    uint index; // 文件目录项在其所在目录中的按目录项索引
};

static struct fat16_meta meta;
static int rootdev = 0;
static uint next_temp_inum = FAT16_TEMP_INUM_BASE;

struct inode *fat16fs_nameiparent(const char *path, char *name);
static void fat16_kw_index_init_once(void);
static void fat16_kw_index_rebuild(void);
static void fat16_kw_index_update_file(uint old_sector, uint old_offset,
                                       uint new_sector, uint new_offset,
                                       const char *path,
                                       const char *old_keywords,
                                       const char *new_keywords);
static void fat16_kw_index_remove_file(uint sector, uint offset, const char *old_keywords);
static void *fat16_kw_index_alloc(uint n, void *arg);
static int fat16_read_keywords_before(struct inode *dp, uint std_index, char *buf, int max);
static void fat16_mark_keyword_entries_before_deleted(struct inode *dp, uint std_index);

// FAT16文件系统本身没有inode表，下面的itable是为了适配上层的inode接口而设置的
// inode中的inum包括三种：ROOTINO表示根目录，FAT16_TEMP_INUM_BASE及以上的表示新
// 创建但尚未持久化的文件，其他值表示已放到磁盘上的文件，inum编码了文件目录项在磁盘上的位置
// addrs[0]存储文件首簇号，addrs[1]和addrs[2]缓存文件目录项在磁盘上的扇区号和扇区内偏移
static struct {
    struct spinlock lock;
    struct inode inode[NINODE];
} itable;

/* 通用辅助函数 */

static uint min_uint(uint a, uint b) {
    return a < b ? a : b;
}

static int kstrlen(const char *s) {
    int n = 0;
    while (s && s[n] != 0) {
        n++;
    }
    return n;
}

static int kstrcmp(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return (int)(uchar)*a - (int)(uchar)*b;
}

static int kstrncmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        uchar ac = (uchar)a[i];
        uchar bc = (uchar)b[i];
        if (ac != bc) {
            return (int)ac - (int)bc;
        }
        if (ac == 0) {
            return 0;
        }
    }
    return 0;
}

static uchar upper_char(uchar c) {
    if (c >= 'a' && c <= 'z') {
        return (uchar)(c - 'a' + 'A');
    }
    return c;
}

static uchar lower_char(uchar c) {
    if (c >= 'A' && c <= 'Z') {
        return (uchar)(c - 'A' + 'a');
    }
    return c;
}

static ushort get16(const uchar *p) {
    return (ushort)p[0] | ((ushort)p[1] << 8);
}

static uint get32(const uchar *p) {
    return (uint)p[0] | ((uint)p[1] << 8) | ((uint)p[2] << 16) | ((uint)p[3] << 24);
}

static void put16(uchar *p, ushort value) {
    p[0] = (uchar)(value & 0xff);
    p[1] = (uchar)((value >> 8) & 0xff);
}

/* FAT16磁盘布局、FAT表和目录项 */

static int fat16_fat_value_is_eoc(uint fat_value) {
    return fat_value >= FAT16_CLUSTER_EOC;
}

static int fat16_cluster_inuse(uint cluster) {
    return cluster >= FAT16_CLUSTER_MIN && cluster < FAT16_CLUSTER_EOC && cluster < meta.clusters + 2;
}

/*
 * 将 FAT16 的数据簇号转换成该簇在磁盘上的第一个扇区号。
 */
static uint fat16_cluster_first_sector(uint cluster) {
    /*
     * LAB TODO [1.1]
     *
     * 请根据 meta.data_sec、meta.sec_per_clus 和 FAT16_CLUSTER_MIN 计算 cluster 的首扇区
     */
    /* LAB TODO [1.1] BEGIN */
    return meta.data_sec + (cluster - FAT16_CLUSTER_MIN) * meta.sec_per_clus;

    /* LAB TODO [1.1] END */
}

// 根据目录项所在的扇区号和扇区内偏移计算该目录项对应的inum
// 只需保证(sector, offset)与inum是一一映射即可
static uint fat16_slot_inum(uint sector, uint offset) {
    return 2U + sector * FAT16_ENTRIES_PER_SECTOR + offset / FAT16_DIR_ENTRY_SIZE;
}

// 根据目录项的inum计算该目录项在磁盘上的位置
static int fat16_slot_from_inum(uint inum, uint *sector, uint *offset) {
    if (inum < 2 || inum >= FAT16_TEMP_INUM_BASE) {
        return -1;
    }
    uint x = inum - 2;
    *sector = x / FAT16_ENTRIES_PER_SECTOR;
    *offset = (x % FAT16_ENTRIES_PER_SECTOR) * FAT16_DIR_ENTRY_SIZE;
    return 0;
}

static int fat16_make_shortname(const char *name, int len, uchar out[FAT16_NAME_LEN]) {
    if (name == 0 || len <= 0) {
        return -1;
    }
    memset(out, ' ', FAT16_NAME_LEN);

    if (len == 1 && name[0] == '.') {
        out[0] = '.';
        return 0;
    }
    if (len == 2 && name[0] == '.' && name[1] == '.') {
        out[0] = '.';
        out[1] = '.';
        return 0;
    }

    int dot = -1;
    for (int i = 0; i < len; i++) {
        if (name[i] == 0 || name[i] == '/') {
            len = i;
            break;
        }
        if (name[i] == '.' && i != 0 && dot < 0) {
            dot = i;
        }
    }
    if (len <= 0) {
        return -1;
    }

    int base_len = dot >= 0 ? dot : len;
    int ext_start = dot >= 0 ? dot + 1 : len;
    int ext_len = dot >= 0 ? len - ext_start : 0;

    if (base_len <= 0) {
        return -1;
    }

    for (int i = 0; i < base_len && i < 8; i++) {
        uchar c = (uchar)name[i];
        if (c < 0x21 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') {
            return -1;
        }
        out[i] = upper_char(c);
    }
    for (int i = 0; i < ext_len && i < 3; i++) {
        uchar c = (uchar)name[ext_start + i];
        if (c < 0x21 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') {
            return -1;
        }
        out[8 + i] = upper_char(c);
    }

    if (out[0] == FAT16_NAME_DELETED) {
        out[0] = 0x05;
    }
    return 0;
}

static int fat16_nameeq(const char *name, const uchar fatname[FAT16_NAME_LEN]) {
    uchar tmp[FAT16_NAME_LEN];
    int len = kstrlen(name);
    if (fat16_make_shortname(name, len, tmp) < 0) {
        return 0;
    }
    for (int i = 0; i < FAT16_NAME_LEN; i++) {
        if (tmp[i] != fatname[i]) {
            return 0;
        }
    }
    return 1;
}

// 将目录项中的FAT16格式的文件名转化为带'.'的完整文件名存到out中，cap是指out的容量
static void fat16_name_to_long(const uchar fatname[FAT16_NAME_LEN], char *out, int cap) {
    if (cap <= 0) {
        return;
    }
    int k = 0;
    if (fatname[0] == '.') {
        while (k < cap - 1 && k < FAT16_NAME_LEN && fatname[k] != ' ') {
            out[k] = (char)fatname[k];
            k++;
        }
        out[k] = 0;
        return;
    }

    for (int i = 0; i < 8 && k < cap - 1; i++) {
        if (fatname[i] == ' ') {
            break;
        }
        out[k++] = (char)lower_char(fatname[i]);
    }

    if (fatname[8] != ' ' && k < cap - 1) {
        out[k++] = '.';
        for (int i = 8; i < 11 && k < cap - 1; i++) {
            if (fatname[i] == ' ') {
                break;
            }
            out[k++] = (char)lower_char(fatname[i]);
        }
    }
    out[k] = 0;
}

// 检查一个目录项是否是标准文件/目录项：非空闲、非删除、非关键词伪目录项、非卷标项
static int fat16_dirent_visible(const struct dirent *entry) {
    if (entry->name[0] == FAT16_NAME_FREE || entry->name[0] == FAT16_NAME_DELETED) {
        return 0;
    }
    if ((entry->attr & FAT16_ATTR_KW) == FAT16_ATTR_KW) {
        return 0;
    }
    if (entry->attr & FAT16_ATTR_VOLUME) {
        return 0;
    }
    return 1;
}

// 检查一个目录项是否可以被当作一个空槽使用，即表示这个位置没有文件/文件夹，或者是已经被删除的文件/文件夹
static int fat16_dirent_available(const struct dirent *entry) {
    return entry->name[0] == FAT16_NAME_FREE || entry->name[0] == FAT16_NAME_DELETED;
}

// 检查一个目录项是否是文件夹
static int fat16_dirent_is_directory(const struct dirent *entry) {
    return (entry->attr & FAT16_ATTR_DIRECTORY) != 0;
}

// FAT16 普通文件按“标准目录项且不是目录”判断，不依赖额外属性位
static int fat16_dirent_is_regular_file(const struct dirent *entry) {
    return fat16_dirent_visible(entry) && !fat16_dirent_is_directory(entry);
}

// 返回目录项的首簇号
static uint fat16_dirent_first_cluster(const struct dirent *entry) {
    return ((uint)entry->first_cluster_hi << 16) | (uint)entry->first_cluster_lo;
}

// 将目录项的首簇号设置为cluster
static void fat16_dirent_set_first_cluster(struct dirent *entry, uint cluster) {
    entry->first_cluster_hi = (ushort)((cluster >> 16) & 0xffff);
    entry->first_cluster_lo = (ushort)(cluster & 0xffff);
}

/*
 * 读取FAT表中索引为cluster的FAT16表项。
 *
 * FAT[cluster] 保存的是文件/目录簇链中的“下一个簇号”，或者保存 EOC 表示链结束。
 */
static uint fat16_read_fat(uint cluster) {
    /*
     * LAB TODO [1.1]
     *
     * 请使用bread()读取FAT表并解析出FAT[cluster]的值
     */
    /* LAB TODO [1.1] BEGIN */
    uint byteoff = cluster * 2;
    uint sector = meta.fat_sec + byteoff / BSIZE;
    uint off = byteoff % BSIZE;
    struct buf *bp = bread(meta.dev, sector);
    uint fat_value = get16(bp->data + off);
    brelse(bp);
    return fat_value;
    /* LAB TODO [1.1] END */
}

// 将FAT表中索引为cluster的项的值设置为value，即FAT[cluster] = value
static void fat16_write_fat(uint cluster, uint value) {
    uint byteoff = cluster * 2;
    uint sec_delta = byteoff / BSIZE;
    uint off = byteoff % BSIZE;
    for (uint i = 0; i < meta.fats; i++) {
        uint sector = meta.fat_sec + i * meta.sec_per_fat + sec_delta;
        struct buf *bp = bread(meta.dev, sector);
        put16(bp->data + off, (ushort)value);
        bwrite(bp);
        brelse(bp);
    }
}

// 将簇号为cluster的簇中所有扇区清零
static void fat16_zero_cluster(uint cluster) {
    uint first = fat16_cluster_first_sector(cluster);
    for (uint i = 0; i < meta.sec_per_clus; i++) {
        struct buf *bp = bread(meta.dev, first + i);
        memset(bp->data, 0, BSIZE);
        bwrite(bp);
        brelse(bp);
    }
}

// 分配一个新的簇
static int fat16_alloc_cluster(uint *out) {
    for (uint cluster = FAT16_CLUSTER_MIN; cluster < meta.clusters + 2; cluster++) {
        if (fat16_read_fat(cluster) == FAT16_CLUSTER_FREE) {
            fat16_write_fat(cluster, FAT16_CLUSTER_END);
            fat16_zero_cluster(cluster);
            *out = cluster;
            return 0;
        }
    }
    return -1;
}

// 释放一个簇链，从簇first开始，沿着FAT表将链上所有簇标记为未使用
static void fat16_free_chain(uint first) {
    uint current_cluster = first;
    uint guard = 0;
    while (fat16_cluster_inuse(current_cluster) && guard <= meta.clusters) {
        uint next_cluster = fat16_read_fat(current_cluster);
        fat16_write_fat(current_cluster, FAT16_CLUSTER_FREE);
        if (fat16_fat_value_is_eoc(next_cluster) || next_cluster == FAT16_CLUSTER_FREE) {
            break;
        }
        current_cluster = next_cluster;
        guard++;
    }
}

// 计算一个簇链的字节长度，从簇first开始，沿着FAT表将链上所有簇的大小相加得到总字节数
static uint fat16_chain_bytes(uint first) {
    if (!fat16_cluster_inuse(first)) {
        return 0;
    }
    uint bytes = 0;
    uint current_cluster = first;
    uint guard = 0;
    while (fat16_cluster_inuse(current_cluster) && guard <= meta.clusters) {
        bytes += meta.cluster_size;
        uint next_cluster = fat16_read_fat(current_cluster);
        if (fat16_fat_value_is_eoc(next_cluster)) {
            break;
        }
        current_cluster = next_cluster;
        guard++;
    }
    return bytes;
}

// 将一个新的簇追加到簇链first的末尾
static int fat16_append_cluster(uint first, uint *new_cluster) {
    if (fat16_alloc_cluster(new_cluster) < 0) {
        return -1;
    }
    if (!fat16_cluster_inuse(first)) {
        return 0;
    }
    uint current_cluster = first;
    uint guard = 0;
    while (fat16_cluster_inuse(current_cluster) && guard <= meta.clusters) {
        uint next_cluster = fat16_read_fat(current_cluster);
        if (fat16_fat_value_is_eoc(next_cluster)) {
            fat16_write_fat(current_cluster, *new_cluster);
            return 0;
        }
        current_cluster = next_cluster;
        guard++;
    }
    return -1;
}

// 从磁盘中读取一个目录项到entry中，sector是目录项所在的扇区号，offset是目录项在该扇区内的字节偏移
static void fat16_read_entry(uint sector, uint offset, struct dirent *entry) {
    struct buf *bp = bread(meta.dev, sector);
    memmove(entry, bp->data + offset, sizeof(*entry));
    brelse(bp);
}

// 将目录项entry写回磁盘
static void fat16_write_entry(uint sector, uint offset, const struct dirent *entry) {
    struct buf *bp = bread(meta.dev, sector);
    memmove(bp->data + offset, entry, sizeof(*entry));
    bwrite(bp);
    brelse(bp);
}

// 计算在dp对应的目录中可存放的32B目录项数量
static uint fat16_dir_slot_count(struct inode *dp) {
    if (dp->inum == FAT16_ROOT_INUM || dp->addrs[0] == 0) {
        return meta.root_entries;
    }
    return fat16_chain_bytes(dp->addrs[0]) / FAT16_DIR_ENTRY_SIZE;
}

// 上层读目录时直接读到FAT16 32B目录项字节流，因此目录大小按可寻址目录项区域计算
static uint fat16_root_dir_size(void) {
    return meta.root_entries * sizeof(struct dirent);
}

// 普通子目录的目录项区域跟随簇链增长，大小等于当前簇链能容纳的目录项字节数
static uint fat16_dir_size(struct inode *dp) {
    return fat16_dir_slot_count(dp) * sizeof(struct dirent);
}

// 将目录项索引转换成上层读写目录时使用的字节偏移
static uint fat16_dir_entry_offset(uint slot_index) {
    return slot_index * sizeof(struct dirent);
}

/*
 * 根据目录项序号index定位目录中的第index个FAT16 32B目录项。
 *
 * FAT16 有两类目录：
 *   根目录：位于BPB后面的固定根目录区，不能像普通文件一样按簇链增长。
 *   普通子目录：内容存放在数据区簇链中，查找时需要沿FAT链定位。
 */
static int fat16_slot_by_index(struct inode *dp, uint index, struct fat16_slot *slot) {
    /*
     * LAB TODO [1.2]
     *
     * 提示：普通子目录分支在下面，它需要走簇链；根目录分支不需要读FAT表。
     */
    /* LAB TODO [1.2] BEGIN */
    // 请在以下 if 块中实现根目录的目录项定位
    if (dp->inum == FAT16_ROOT_INUM || dp->addrs[0] == 0) {
        if (index >= meta.root_entries) {
            return -1;
        }
        uint entry_byte_offset = index * FAT16_DIR_ENTRY_SIZE;
        slot->sector = meta.root_sec + entry_byte_offset / BSIZE;
        slot->offset = entry_byte_offset % BSIZE;
        slot->index = index;
        fat16_read_entry(slot->sector, slot->offset, &slot->entry);
        return 0;
    }

    /* LAB TODO [1.2] END */

    uint slots_per_cluster = meta.cluster_size / FAT16_DIR_ENTRY_SIZE;
    uint cluster_index = index / slots_per_cluster;
    uint within = index % slots_per_cluster;
    uint current_cluster = dp->addrs[0];
    for (uint i = 0; i < cluster_index; i++) {
        uint next_cluster = fat16_read_fat(current_cluster);
        if (fat16_fat_value_is_eoc(next_cluster) || !fat16_cluster_inuse(next_cluster)) {
            return -1;
        }
        current_cluster = next_cluster;
    }

    uint entry_byte_offset = within * FAT16_DIR_ENTRY_SIZE;
    slot->sector = fat16_cluster_first_sector(current_cluster) + entry_byte_offset / BSIZE;
    slot->offset = entry_byte_offset % BSIZE;
    slot->index = index;
    fat16_read_entry(slot->sector, slot->offset, &slot->entry);
    return 0;
}

// 扩大dp对应的文件夹
static int fat16_grow_dir(struct inode *dp) {
    if (dp->inum == FAT16_ROOT_INUM || dp->addrs[0] == 0) {
        return -1;
    }
    uint new_cluster = 0;
    if (fat16_append_cluster(dp->addrs[0], &new_cluster) < 0) {
        return -1;
    }
    dp->size = fat16_dir_size(dp);
    return 0;
}

// 查找dp对应的文件夹中:
// want_empty == 0，查找名字为name的目录项，并将其内容读入slot
// want_empty != 0，查找一个空目录项（可以是未使用的，也可以是已经被删除的），并将其内容读入slot
static int fat16_find_slot(struct inode *dp, const char *name, int want_empty,
                           struct fat16_slot *slot, uint *logical_off) {
    uint nslot = fat16_dir_slot_count(dp);
    struct fat16_slot deleted;
    int have_deleted = 0;

    for (uint i = 0; i < nslot; i++) {
        struct fat16_slot s;
        if (fat16_slot_by_index(dp, i, &s) < 0) {
            return -1;
        }
        if (fat16_dirent_visible(&s.entry)) {
            if (name && fat16_nameeq(name, s.entry.name)) {
                if (slot) {
                    *slot = s;
                }
                if (logical_off) {
                    *logical_off = fat16_dir_entry_offset(i);
                }
                return 1;
            }
            continue;
        }
        if (want_empty && s.entry.name[0] == FAT16_NAME_DELETED && !have_deleted) {
            deleted = s;
            have_deleted = 1;
        }
        if (s.entry.name[0] == FAT16_NAME_FREE) {
            if (want_empty) {
                if (slot) {
                    *slot = have_deleted ? deleted : s;
                }
                if (logical_off) {
                    *logical_off = fat16_dir_entry_offset(have_deleted ? deleted.index : s.index);
                }
                return 0;
            }
            return 0;
        }
    }

    if (want_empty && have_deleted) {
        if (slot) {
            *slot = deleted;
        }
        if (logical_off) {
            *logical_off = fat16_dir_entry_offset(deleted.index);
        }
        return 0;
    }

    if (want_empty && fat16_grow_dir(dp) == 0) {
        nslot = fat16_dir_slot_count(dp);
        if (fat16_slot_by_index(dp, nslot - meta.cluster_size / FAT16_DIR_ENTRY_SIZE, slot) == 0) {
            if (logical_off) {
                *logical_off = fat16_dir_entry_offset(slot->index);
            }
            return 0;
        }
    }

    return -1;
}

// 构造标准 FAT16 短名目录项
static void fat16_prepare_dirent(struct dirent *entry, const uchar shortname[FAT16_NAME_LEN],
                                 short type, uint first_cluster, uint size) {
    memset(entry, 0, sizeof(*entry));
    memmove(entry->name, shortname, FAT16_NAME_LEN);
    entry->attr = (type == T_DIR) ? FAT16_ATTR_DIRECTORY : 0;
    fat16_dirent_set_first_cluster(entry, first_cluster);
    entry->file_size = (type == T_FILE) ? size : 0;
}

/* FAT16文件系统主体 */

// 根据设备号和inum查找对应的inode，如果已经在内存中加载了就返回该inode，否则分配一个新的inode并返回
static struct inode *fat16_iget(uint dev, uint inum) {
    struct inode *empty = 0;

    acquire(&itable.lock);
    for (int i = 0; i < NINODE; i++) {
        struct inode *ip = &itable.inode[i];
        if (ip->ref > 0 && ip->dev == dev && ip->inum == inum) {
            ip->ref++;
            release(&itable.lock);
            return ip;
        }
        if (empty == 0 && ip->ref == 0) {
            empty = ip;
        }
    }

    if (empty == 0) {
        release(&itable.lock);
        panic("fat16_iget: no inodes");
    }

    empty->dev = dev;
    empty->inum = inum;
    empty->ref = 1;
    empty->valid = 0;
    empty->type = 0;
    empty->major = 0;
    empty->minor = 0;
    empty->nlink = 0;
    empty->size = 0;
    memset(empty->addrs, 0, sizeof(empty->addrs));
    release(&itable.lock);
    return empty;
}

// 根据inum查找已经加载到内存中的inode，如果找到就返回该inode，否则返回0
static struct inode *fat16_find_loaded(uint inum) {
    for (int i = 0; i < NINODE; i++) {
        if (itable.inode[i].ref > 0 && itable.inode[i].inum == inum && itable.inode[i].dev == (uint)rootdev) {
            return &itable.inode[i];
        }
    }
    return 0;
}

static int fat16_inode_pending(struct inode *ip) {
    return ip && ip->valid && ip->inum >= FAT16_TEMP_INUM_BASE;
}

// 根据 FAT16 标准目录项填充内存 inode
static void fat16_fill_inode_from_entry(struct inode *ip, const struct fat16_slot *slot) {
    uint cluster = fat16_dirent_first_cluster(&slot->entry);
    ip->type = fat16_dirent_is_directory(&slot->entry) ? T_DIR : T_FILE;
    ip->major = 0;
    ip->minor = 0;
    ip->nlink = 1;
    ip->size = ip->type == T_DIR ? fat16_dir_size(ip) : slot->entry.file_size;
    memset(ip->addrs, 0, sizeof(ip->addrs));
    ip->addrs[0] = cluster;
    ip->addrs[1] = slot->sector;
    ip->addrs[2] = slot->offset;
    ip->valid = 1;
}

// 初始化根目录inode
static void fat16_fill_root_inode(struct inode *ip) {
    ip->type = T_DIR;
    ip->major = 0;
    ip->minor = 0;
    ip->nlink = 1;
    ip->size = fat16_root_dir_size();
    memset(ip->addrs, 0, sizeof(ip->addrs));
    ip->addrs[1] = meta.root_sec;
    ip->valid = 1;
}

/*
 * 初始化并挂载 FAT16 文件系统。
 */
void fat16fs_fsinit(int dev) {

    /* FAT16文件系统初始化 */

    static int once = 0;
    if (once == 0) {
        initlock(&itable.lock, "fat16_itable");
        for (int i = 0; i < NINODE; i++) {
            initsleeplock(&itable.inode[i].lock, "fat16_inode");
            itable.inode[i].ref = 0;
            itable.inode[i].valid = 0;
        }
        fat16_kw_index_init_once();
        once = 1;
    }

    rootdev = dev;
    meta.dev = (uint)dev;

    /* 读取引导扇区 */
    struct buf *bp = bread((uint)dev, 0);
    uchar *d = bp->data;

    /*
     * LAB TODO [1.1]
     *
     * 第一步：从 BPB 中读取后续几何计算需要的字段。
     */
    /* LAB TODO [1.1] BEGIN: read BPB fields */

    // !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!

    // 包裹在 TODO 中的 panic 是用来帮助同学们定位需要实现的功能的，实现完后请一定记得要注释掉

    // !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!

    meta.bytes_per_sec = get16(d + 0x0b);
    meta.sec_per_clus = d[0x0d];
    meta.reserved = get16(d + 0x0e);
    meta.fats = d[0x10];
    meta.root_entries = get16(d + 0x11);
    meta.total_sec = get16(d + 0x13);
    if (meta.total_sec == 0) {
        meta.total_sec = get32(d + 0x20);
    }
    meta.sec_per_fat = get16(d + 0x16);
    /* LAB TODO [1.1] END: read BPB fields */

    ushort sig = get16(d + 510);
    brelse(bp);

    if (meta.bytes_per_sec != BSIZE || meta.sec_per_clus == 0 || meta.fats == 0 ||
        meta.root_entries == 0 || meta.total_sec == 0 || meta.sec_per_fat == 0 || sig != 0xaa55) {
        panic("fat16fs: bad boot sector");
    }

    /*
     * LAB TODO [1.1]
     *
     * 第二步：根据BPB字段推导FAT16的各个区域。
     */
    /* LAB TODO [1.1] BEGIN: compute FAT16 regions */

    meta.fat_sec = meta.reserved;
    meta.root_sectors =
        (meta.root_entries * FAT16_DIR_ENTRY_SIZE + meta.bytes_per_sec - 1) / meta.bytes_per_sec;
    meta.root_sec = meta.fat_sec + meta.fats * meta.sec_per_fat;
    meta.data_sec = meta.root_sec + meta.root_sectors;
    meta.clusters = (meta.total_sec - meta.data_sec) / meta.sec_per_clus;
    meta.cluster_size = meta.bytes_per_sec * meta.sec_per_clus;
    /* LAB TODO [1.1] END: compute FAT16 regions */

    printf("[fat16fs] mounted: sectors=%d clusters=%d spc=%d root=%d data=%d\n",
           meta.total_sec, meta.clusters, meta.sec_per_clus, meta.root_sec, meta.data_sec);
    fat16_kw_index_rebuild();
}

struct inode *fat16fs_idup(struct inode *ip) {
    if (ip == 0) {
        return 0;
    }
    acquire(&itable.lock);
    if (ip->ref < 1) {
        release(&itable.lock);
        panic("fat16fs_idup");
    }
    ip->ref++;
    release(&itable.lock);
    return ip;
}

void fat16fs_ilock(struct inode *ip) {
    if (ip == 0 || ip->ref < 1) {
        panic("fat16fs_ilock");
    }
    acquiresleep(&ip->lock);
    if (ip->valid == 0) {
        if (ip->inum == FAT16_ROOT_INUM) {
            fat16_fill_root_inode(ip);
        } else if (ip->inum >= FAT16_TEMP_INUM_BASE) {
            panic("fat16fs_ilock: invalid temp inode");
        } else {
            uint sector = 0, offset = 0;
            struct fat16_slot slot;
            if (fat16_slot_from_inum(ip->inum, &sector, &offset) < 0) {
                panic("fat16fs_ilock: bad inum");
            }
            slot.sector = sector;
            slot.offset = offset;
            slot.index = 0;
            fat16_read_entry(sector, offset, &slot.entry);
            if (!fat16_dirent_visible(&slot.entry)) {
                panic("fat16fs_ilock: no entry");
            }
            fat16_fill_inode_from_entry(ip, &slot);
        }
    }
}

void fat16fs_iunlock(struct inode *ip) {
    if (ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1) {
        panic("fat16fs_iunlock");
    }
    releasesleep(&ip->lock);
}

void fat16fs_iupdate(struct inode *ip) {
    if (ip == 0 || ip->inum == FAT16_ROOT_INUM || fat16_inode_pending(ip)) {
        return;
    }
    if (ip->addrs[1] == 0 && ip->addrs[2] == 0) {
        return;
    }

    struct dirent entry;
    fat16_read_entry(ip->addrs[1], ip->addrs[2], &entry);
    if (ip->type == 0 || ip->nlink == 0) {
        entry.name[0] = FAT16_NAME_DELETED;
    } else {
        entry.attr = (ip->type == T_DIR) ? FAT16_ATTR_DIRECTORY : 0;
        fat16_dirent_set_first_cluster(&entry, ip->addrs[0]);
        entry.file_size = (ip->type == T_FILE) ? ip->size : 0;
    }
    fat16_write_entry(ip->addrs[1], ip->addrs[2], &entry);
}

static int fat16_inode_lock_if_needed(struct inode *ip) {
    if (holdingsleep(&ip->lock)) {
        return 0;
    }
    fat16fs_ilock(ip);
    return 1;
}

void fat16fs_itrunc(struct inode *ip) {
    if (ip == 0) {
        return;
    }
    int need_unlock = fat16_inode_lock_if_needed(ip);
    if (ip->inum != FAT16_ROOT_INUM && fat16_cluster_inuse(ip->addrs[0])) {
        fat16_free_chain(ip->addrs[0]);
    }
    ip->addrs[0] = 0;
    ip->size = 0;
    fat16fs_iupdate(ip);
    if (need_unlock) {
        fat16fs_iunlock(ip);
    }
}

void fat16fs_iput(struct inode *ip) {
    if (ip == 0) {
        return;
    }

    acquire(&itable.lock);
    if (ip->ref < 1) {
        release(&itable.lock);
        panic("fat16fs_iput");
    }

    if (ip->ref == 1 && ip->valid && ip->nlink == 0) {
        release(&itable.lock);
        fat16fs_ilock(ip);
        fat16fs_itrunc(ip);
        ip->type = 0;
        fat16fs_iupdate(ip);
        ip->valid = 0;
        fat16fs_iunlock(ip);
        acquire(&itable.lock);
    }

    ip->ref--;
    if (ip->ref == 0) {
        ip->valid = 0;
        ip->type = 0;
        ip->size = 0;
        memset(ip->addrs, 0, sizeof(ip->addrs));
    }
    release(&itable.lock);
}

void fat16fs_iunlockput(struct inode *ip) {
    fat16fs_iunlock(ip);
    fat16fs_iput(ip);
}

// 分配一个新的inode，类型为type，设备号为dev，返回该inode的指针
struct inode *fat16fs_ialloc(uint dev, short type) {
    struct inode *empty = 0;
    acquire(&itable.lock);
    for (int i = 0; i < NINODE; i++) {
        if (itable.inode[i].ref == 0) {
            empty = &itable.inode[i];
            break;
        }
    }
    if (empty == 0) {
        release(&itable.lock);
        return 0;
    }

    empty->dev = dev;
    empty->inum = next_temp_inum++;
    empty->ref = 1;
    empty->valid = 1;
    empty->type = type;
    empty->major = 0;
    empty->minor = 0;
    empty->nlink = 1;
    empty->size = 0;
    memset(empty->addrs, 0, sizeof(empty->addrs));
    release(&itable.lock);
    return empty;
}

/*
 * 根据文件内偏移 off 找到文件簇链中的目标簇。
 *
 * 读文件时 alloc == 0：如果偏移在当前簇链之外，说明读不到数据，返回 -1。
 * 写文件时 alloc != 0：如果偏移超过当前簇链，需要分配新簇并接到链尾。
 */
static int fat16_cluster_for_offset(struct inode *ip, uint off, int alloc, uint *cluster_out) {
    uint cluster_index = off / meta.cluster_size;

    if (!fat16_cluster_inuse(ip->addrs[0])) {
        if (!alloc) {
            return -1;
        }
        uint first_cluster = 0;
        if (fat16_alloc_cluster(&first_cluster) < 0) {
            return -1;
        }
        ip->addrs[0] = first_cluster;
    }

    uint current_cluster = ip->addrs[0];

    /*
     * LAB TODO [1.3]
     *
     * 沿FAT链走到cluster_index指定的目标簇，如果提前到了链尾并且alloc为0，则说明off对应的数据不存在，返回-1；
     * 如果alloc不为0，则需要分配新簇接到链尾继续走，直到走到目标簇
     */
    /* LAB TODO [1.3] BEGIN: walk or grow FAT chain */
    for (uint i = 0; i < cluster_index; i++) {
        uint next_cluster = fat16_read_fat(current_cluster);
        if (fat16_fat_value_is_eoc(next_cluster) || !fat16_cluster_inuse(next_cluster)) {
            if (!alloc) {
                return -1;
            }
            uint new_cluster = 0;
            if (fat16_alloc_cluster(&new_cluster) < 0) {
                return -1;
            }
            fat16_write_fat(current_cluster, new_cluster);
            next_cluster = new_cluster;
        }
        current_cluster = next_cluster;
    }

    *cluster_out = current_cluster;
    return 0;
    /* LAB TODO [1.3] END: walk or grow FAT chain */
}

// 从目录中读取FAT16 32B目录项字节流，不做legacyfs目录项转换
static int fat16_read_dir(struct inode *ip, uint64 off, void *dst, uint n) {
    uchar *dst_bytes = (uchar *)dst;
    uint copied = 0;
    while (copied < n && off + copied < ip->size) {
        uint position = (uint)(off + copied);
        uint slot_index = position / sizeof(struct dirent);
        uint entry_offset = position % sizeof(struct dirent);
        struct fat16_slot slot;
        if (fat16_slot_by_index(ip, slot_index, &slot) < 0) {
            break;
        }
        uint copy_bytes = min_uint(n - copied, sizeof(struct dirent) - entry_offset);
        memmove(dst_bytes + copied, ((uchar *)&slot.entry) + entry_offset, copy_bytes);
        copied += copy_bytes;
    }
    return (int)copied;
}

/*
 * 从文件或目录 inode 读取数据。
 *
 * 目录读取交给 fat16_read_dir()；普通文件读取需要根据文件偏移沿FAT链找到对应簇，
 * 再把磁盘扇区中的字节拷贝到 dst。
 */
int fat16fs_readi(struct inode *ip, uint64 off, void *dst, uint n) {
    if (ip == 0 || dst == 0) {
        return -1;
    }
    if (n == 0) {
        return 0;
    }
    int need_unlock = fat16_inode_lock_if_needed(ip);
    if (ip->type == T_DIR) {
        int r = fat16_read_dir(ip, off, dst, n);
        if (need_unlock) {
            fat16fs_iunlock(ip);
        }
        return r;
    }
    if (ip->type != T_FILE) {
        if (need_unlock) {
            fat16fs_iunlock(ip);
        }
        return -1;
    }
    if (off > ip->size) {
        if (need_unlock) {
            fat16fs_iunlock(ip);
        }
        return 0;
    }
    if (off + n > ip->size) {
        n = (uint)(ip->size - off);
    }

    /*
     * LAB TODO [1.3]
     *
     * 普通文件读取循环。
     *
     * 用 fat16_cluster_for_offset()找到off和已经读出的字节数对应的簇，根据簇大小和簇内偏移计算需要读的扇区和扇区内偏移，
     * 从磁盘读入扇区到内存后把数据拷贝到dst，直到读完n字节。
     */
    /* LAB TODO [1.3] BEGIN */

    uint copied = 0;
    uchar *dst_bytes = (uchar *)dst;
    while (copied < n) {
        uint position = (uint)off + copied;
        uint cluster = 0;
        if (fat16_cluster_for_offset(ip, position, 0, &cluster) < 0) {
            break;
        }
        uint cluster_off = position % meta.cluster_size;
        uint sector = fat16_cluster_first_sector(cluster) + cluster_off / BSIZE;
        uint sector_off = cluster_off % BSIZE;
        uint copy_bytes = min_uint(n - copied, BSIZE - sector_off);
        struct buf *bp = bread(ip->dev, sector);
        memmove(dst_bytes + copied, bp->data + sector_off, copy_bytes);
        brelse(bp);
        copied += copy_bytes;
    }
    /* LAB TODO [1.3] END */

    if (need_unlock) {
        fat16fs_iunlock(ip);
    }
    return (int)copied;
}

// 处理上层 unlink 使用的目录写入协议：只允许把已有可见目录项删除，不支持任意覆盖目录项，
// 因为上层的 unlink 的删除逻辑是将原来的目录项写入 zero_dirent 来删除，这与 FAT16 删除做法不同，
// 因此这里做特殊处理，注意：此函数不用来将数据写入目录！！！
static int fat16_write_dir(struct inode *ip, uint64 off, const void *src, uint n) {
    if (n != sizeof(struct dirent) || (off % sizeof(struct dirent)) != 0) {
        return -1;
    }
    const struct dirent *entry = (const struct dirent *)src;
    uint slot_index = (uint)(off / sizeof(struct dirent));
    struct fat16_slot slot = {0};
    if (fat16_slot_by_index(ip, slot_index, &slot) < 0) {
        return -1;
    }
    if (!dirent_is_visible(entry)) {
        char old_keywords[FAT16_KW_MAX_BYTES + 1];
        if (fat16_read_keywords_before(ip, slot_index, old_keywords, sizeof(old_keywords)) < 0) {
            return -1;
        }
        fat16_kw_index_remove_file(slot.sector, slot.offset, old_keywords);
        fat16_mark_keyword_entries_before_deleted(ip, slot_index);
        slot.entry.name[0] = FAT16_NAME_DELETED;
        fat16_write_entry(slot.sector, slot.offset, &slot.entry);
        return (int)n;
    }
    return -1;
}

/*
 * 向文件或目录inode写入数据。
 */
int fat16fs_writei(struct inode *ip, uint64 off, const void *src, uint n) {
    if (ip == 0 || src == 0) {
        return -1;
    }
    if (n == 0) {
        return 0;
    }
    int need_unlock = fat16_inode_lock_if_needed(ip);
    if (ip->type == T_DIR) {
        int r = fat16_write_dir(ip, off, src, n);
        if (need_unlock) {
            fat16fs_iunlock(ip);
        }
        return r;
    }
    if (ip->type != T_FILE || off > ip->size || off + n < off) {
        if (need_unlock) {
            fat16fs_iunlock(ip);
        }
        return -1;
    }

    /*
     * LAB TODO [1.4]
     *
     * 普通文件写入循环。
     *
     * 用fat16_cluster_for_offset()找到对应簇，根据off和已经写入的字节数计算需要读的扇区和扇区内偏移
     * 将数据写入扇区中，直到写完n字节。注意，fat16_cluster_for_offset()的alloc参数要设置为1，允许它
     * 在写路径上分配新簇
     */
    /* LAB TODO [1.4] BEGIN: write data */

    uint copied = 0;
    const uchar *src_bytes = (const uchar *)src;
    while (copied < n) {
        uint position = (uint)off + copied;
        uint cluster = 0;
        if (fat16_cluster_for_offset(ip, position, 1, &cluster) < 0) {
            break;
        }
        uint cluster_off = position % meta.cluster_size;
        uint sector = fat16_cluster_first_sector(cluster) + cluster_off / BSIZE;
        uint sector_off = cluster_off % BSIZE;
        uint copy_bytes = min_uint(n - copied, BSIZE - sector_off);
        struct buf *bp = bread(ip->dev, sector);
        memmove(bp->data + sector_off, src_bytes + copied, copy_bytes);
        bwrite(bp);
        brelse(bp);
        copied += copy_bytes;
    }

    /* LAB TODO [1.4] END: write data */

    /* LAB TODO [1.4] BEGIN: update file size */
    uint end = (uint)off + copied;
    if (end > ip->size) {
        ip->size = end;
        fat16fs_iupdate(ip);
    }
    /* LAB TODO [1.4] END: update file size */
    if (need_unlock) {
        fat16fs_iunlock(ip);
    }
    return (int)copied;
}

int fat16fs_namecmp(const char *s, const char *t) {
    return kstrncmp(s, t, DIRSIZ);
}

// 在dp对应的目录中查找名字为name的目录项，返回对应的inode，并将目录项在目录中的偏移写入poff
struct inode *fat16fs_dirlookup(struct inode *dp, const char *name, uint *poff) {
    int need_unlock = fat16_inode_lock_if_needed(dp);
    if (dp->type != T_DIR) {
        if (need_unlock) {
            fat16fs_iunlock(dp);
        }
        return 0;
    }
    if (kstrcmp(name, ".") == 0) {
        if (poff) {
            *poff = 0;
        }
        struct inode *r = fat16fs_idup(dp);
        if (need_unlock) {
            fat16fs_iunlock(dp);
        }
        return r;
    }
    if (kstrcmp(name, "..") == 0 && (dp->inum == FAT16_ROOT_INUM || dp->addrs[0] == 0)) {
        if (poff) {
            *poff = 0;
        }
        struct inode *r = fat16fs_idup(dp);
        if (need_unlock) {
            fat16fs_iunlock(dp);
        }
        return r;
    }

    struct fat16_slot slot = {0};
    uint off = 0;
    int found = fat16_find_slot(dp, name, 0, &slot, &off);
    if (found == 1) {
        if (poff) {
            *poff = off;
        }
        uint inum = fat16_slot_inum(slot.sector, slot.offset);
        if (need_unlock) {
            fat16fs_iunlock(dp);
        }
        return fat16_iget(dp->dev, inum);
    }
    if (need_unlock) {
        fat16fs_iunlock(dp);
    }
    return 0;
}

// 将dp对应目录中名字为name的目录项链接到inum对应的inode上
int fat16fs_dirlink(struct inode *dp, const char *name, uint inum) {
    if (dp == 0 || name == 0) {
        return -1;
    }
    int need_unlock = fat16_inode_lock_if_needed(dp);
    if (dp->type != T_DIR) {
        if (need_unlock) {
            fat16fs_iunlock(dp);
        }
        return -1;
    }

    if (fat16_inode_pending(dp) && dp->type == T_DIR && !fat16_cluster_inuse(dp->addrs[0])) {
        uint cluster = 0;
        if (fat16_alloc_cluster(&cluster) < 0) {
            if (need_unlock) {
                fat16fs_iunlock(dp);
            }
            return -1;
        }
        dp->addrs[0] = cluster;
        dp->size = fat16_dir_size(dp);
    }

    int dot_link = (kstrcmp(name, ".") == 0 || kstrcmp(name, "..") == 0);
    if (!dot_link) {
        struct inode *old = fat16fs_dirlookup(dp, name, 0);
        if (old != 0) {
            fat16fs_iput(old);
            if (need_unlock) {
                fat16fs_iunlock(dp);
            }
            return -1;
        }
    }

    uchar shortname[FAT16_NAME_LEN];
    if (fat16_make_shortname(name, kstrlen(name), shortname) < 0) {
        if (need_unlock) {
            fat16fs_iunlock(dp);
        }
        return -1;
    }

    acquire(&itable.lock);
    struct inode *target = fat16_find_loaded(inum);
    release(&itable.lock);
    if (target == 0 || (!fat16_inode_pending(target) && !dot_link)) {
        if (need_unlock) {
            fat16fs_iunlock(dp);
        }
        return -1;
    }

    struct fat16_slot slot = {0};
    uint logical = 0;
    if (fat16_find_slot(dp, 0, 1, &slot, &logical) < 0) {
        if (need_unlock) {
            fat16fs_iunlock(dp);
        }
        return -1;
    }

    struct dirent new_entry;
    fat16_prepare_dirent(&new_entry, shortname, target->type, target->addrs[0], target->size);
    fat16_write_entry(slot.sector, slot.offset, &new_entry);

    if (fat16_inode_pending(target) && !dot_link) {
        target->inum = fat16_slot_inum(slot.sector, slot.offset);
        target->addrs[1] = slot.sector;
        target->addrs[2] = slot.offset;
        target->addrs[9] = 0;
        target->valid = 1;
    }
    dp->size = fat16_dir_size(dp);

    if (need_unlock) {
        fat16fs_iunlock(dp);
    }
    return 0;
}

// 从路径中取出下一个路径分量
// 比如对于路径 "/a/b/c"，第一次调用返回 "a"，第二次 "b"，依次类推
static const char *fat16_skipelem(const char *path, char *name) {
    while (*path == '/') {
        path++;
    }
    if (*path == 0) {
        return 0;
    }
    const char *s = path;
    while (*path != '/' && *path != 0) {
        path++;
    }
    int len = (int)(path - s);
    if (len >= DIRSIZ) {
        memmove(name, s, DIRSIZ);
        name[DIRSIZ] = 0;
    } else {
        memmove(name, s, (uint)len);
        name[len] = 0;
    }
    while (*path == '/') {
        path++;
    }
    return path;
}

// 返回目标inode或者它的父目录的inode
static struct inode *fat16_namex(const char *path, int wantparent, char *name) {
    if (path == 0 || path[0] == 0) {
        return 0;
    }
    struct inode *ip = 0;
    if (path[0] == '/') {
        ip = fat16_iget((uint)rootdev, FAT16_ROOT_INUM);
    } else {
        ip = proc_cwddup();
        if (ip == 0) {
            ip = fat16_iget((uint)rootdev, FAT16_ROOT_INUM);
        }
    }

    const char *p = path;
    while ((p = fat16_skipelem(p, name)) != 0) {
        fat16fs_ilock(ip);
        if (ip->type != T_DIR) {
            fat16fs_iunlockput(ip);
            return 0;
        }
        if (wantparent && *p == 0) {
            fat16fs_iunlock(ip);
            return ip;
        }
        struct inode *next = fat16fs_dirlookup(ip, name, 0);
        fat16fs_iunlockput(ip);
        ip = next;
        if (ip == 0) {
            return 0;
        }
    }
    if (wantparent) {
        fat16fs_iput(ip);
        return 0;
    }
    return ip;
}

struct inode *fat16fs_namei(const char *path) {
    char name[DIRSIZ + 1];
    return fat16_namex(path, 0, name);
}

struct inode *fat16fs_nameiparent(const char *path, char *name) {
    return fat16_namex(path, 1, name);
}

static int fat16_same_dir_inode(struct dirent *entry, struct inode *ip, uint inum) {
    if (!fat16_dirent_visible(entry) || !fat16_dirent_is_directory(entry)) {
        return 0;
    }
    uint cluster = fat16_dirent_first_cluster(entry);
    if (inum == ip->inum) {
        return 1;
    }
    return cluster == ip->addrs[0];
}

static int fat16_dirfindname(struct inode *dp, struct inode *ip, char *namebuf, int namecap) {
    int need_unlock = fat16_inode_lock_if_needed(dp);
    if (dp->type != T_DIR) {
        if (need_unlock) {
            fat16fs_iunlock(dp);
        }
        return -1;
    }
    uint nslot = fat16_dir_slot_count(dp);
    for (uint i = 0; i < nslot; i++) {
        struct fat16_slot slot;
        if (fat16_slot_by_index(dp, i, &slot) < 0) {
            break;
        }
        if (!fat16_dirent_visible(&slot.entry)) {
            continue;
        }
        if (slot.entry.name[0] == '.') {
            continue;
        }
        uint inum = fat16_slot_inum(slot.sector, slot.offset);
        if (fat16_same_dir_inode(&slot.entry, ip, inum)) {
            fat16_name_to_long(slot.entry.name, namebuf, namecap);
            if (need_unlock) {
                fat16fs_iunlock(dp);
            }
            return 0;
        }
    }
    if (need_unlock) {
        fat16fs_iunlock(dp);
    }
    return -1;
}

static int fat16_inode_is_root_dir(struct inode *ip) {
    int need = fat16_inode_lock_if_needed(ip);
    int r = (ip->type == T_DIR && ip->addrs[0] == 0);
    if (need) {
        fat16fs_iunlock(ip);
    }
    return r;
}

int fat16fs_getcwd_path(struct inode *cwd, char *buf, int max) {
    if (cwd == 0 || buf == 0 || max < 2) {
        return -1;
    }
    char path[MAXPATH];
    path[0] = 0;
    int pathlen = 0;
    struct inode *ip = fat16fs_idup(cwd);
    if (ip == 0) {
        return -1;
    }
    while (1) {
        if (fat16_inode_is_root_dir(ip)) {
            if (pathlen + 2 > max) {
                fat16fs_iput(ip);
                return -1;
            }
            buf[0] = '/';
            memmove(buf + 1, path, (uint)(pathlen + 1));
            fat16fs_iput(ip);
            return 0;
        }
        struct inode *parent = fat16fs_dirlookup(ip, "..", 0);
        if (parent == 0) {
            fat16fs_iput(ip);
            return -1;
        }
        char name[DIRSIZ + 1];
        if (fat16_dirfindname(parent, ip, name, sizeof(name)) < 0) {
            fat16fs_iput(parent);
            fat16fs_iput(ip);
            return -1;
        }
        int namelen = kstrlen(name);
        if (pathlen + namelen + 2 > MAXPATH) {
            fat16fs_iput(parent);
            fat16fs_iput(ip);
            return -1;
        }
        memmove(path + namelen + 1, path, (uint)(pathlen + 1));
        memmove(path, name, (uint)namelen);
        path[namelen] = '/';
        pathlen = namelen + 1 + pathlen;
        fat16fs_iput(ip);
        ip = parent;
    }
}

/* 文件属性系统 */

// 检查一个目录项是否是一个合法的存储某标准目录项的关键词伪目录项
static int fat16_dirent_is_keyword(const struct dirent *entry) {
    return entry->attr == FAT16_ATTR_KW && (entry->name[0] & FAT16_KW_TYPE_MASK) == FAT16_KW_TYPE;
}

static uchar fat16_kw_get_data_byte(const struct dirent *entry, int pos) {
    const uchar *entry_bytes = (const uchar *)entry;
    if (pos < 0 || pos >= FAT16_KW_BYTES_PER_ENTRY) {
        return 0;
    }
    if (pos < 10) {
        return entry_bytes[1 + pos];
    }
    return entry_bytes[12 + (pos - 10)];
}

static void fat16_kw_set_data_byte(struct dirent *entry, int pos, uchar byte) {
    uchar *entry_bytes = (uchar *)entry;
    if (pos < 0 || pos >= FAT16_KW_BYTES_PER_ENTRY) {
        return;
    }
    if (pos < 10) {
        entry_bytes[1 + pos] = byte;
    } else {
        entry_bytes[12 + (pos - 10)] = byte;
    }
}

static int fat16_keywords_len(const char *s) {
    int n = 0;
    while (s && s[n] != 0) {
        n++;
        if (n >= FAT16_KW_MAX_BYTES) {
            return -1;
        }
    }
    return n;
}

// 计算存储关键词需要多少个32B的伪目录项
static int fat16_keyword_entries_needed(const char *keywords) {
    int len = fat16_keywords_len(keywords);
    if (len < 0) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    return (len + 1 + FAT16_KW_BYTES_PER_ENTRY - 1) / FAT16_KW_BYTES_PER_ENTRY;
}

/*
 * 统计标准目录项std_index前面连续有多少个关键词伪目录项。
 *
 * 本文件系统把文件关键词存放在标准 FAT16 文件目录项之前：
 *   [kw0][kw1]...[kwN-1][standard file entry]
 * 因此读取或修改某个文件的关键词时，需要从标准目录项向前扫描，找到属于它的伪目录项数量。
 */
static int fat16_keyword_count_before(struct inode *dp, uint std_index) {
    /*
     * LAB TODO [2.1]
     *
     * 提示：fat16_dirent_is_keyword() 只判断目录项是否是本实验定义的关键词伪目录项。
     */
    int count = 0;
    /* LAB TODO [2.1] BEGIN */
    while (count < FAT16_KW_MAX_ENTRIES && std_index > (uint)count) {
        struct fat16_slot slot;
        if (fat16_slot_by_index(dp, std_index - (uint)count - 1, &slot) < 0) {
            break;
        }
        if (!fat16_dirent_is_keyword(&slot.entry)) {
            break;
        }
        count++;
    }
    /* LAB TODO [2.1] END */
    return count;
}

// 将dp对应文件夹下索引为index的目录项标记为删除
static void fat16_mark_slot_deleted(struct inode *dp, uint index) {
    struct fat16_slot s;
    if (fat16_slot_by_index(dp, index, &s) < 0) {
        return;
    }
    s.entry.name[0] = FAT16_NAME_DELETED;
    fat16_write_entry(s.sector, s.offset, &s.entry);
}

// 将dp对应文件夹中从标准目录项索引std_index开始往前数的关键词伪目录项都标记为删除
static void fat16_mark_keyword_entries_before_deleted(struct inode *dp, uint std_index) {
    int count = fat16_keyword_count_before(dp, std_index);
    uint start = std_index - (uint)count;
    for (int i = 0; i < count; i++) {
        fat16_mark_slot_deleted(dp, start + (uint)i);
    }
}

static int fat16_slot_in_range(uint idx, uint start, uint count) {
    return count != 0 && idx >= start && idx < start + count;
}

static void fat16_mark_range_deleted_except(struct inode *dp, uint start, uint count,
                                            uint keep_start, uint keep_count) {
    for (uint i = 0; i < count; i++) {
        uint idx = start + i;
        if (fat16_slot_in_range(idx, keep_start, keep_count)) {
            continue;
        }
        fat16_mark_slot_deleted(dp, idx);
    }
}

// 检查index对应的目录项槽是否可以被用来存放一个文件目录项
static int fat16_slot_available_for_run(struct inode *dp, uint index,
                                        uint old_start, uint old_count) {
    if (fat16_slot_in_range(index, old_start, old_count)) {
        return 1;
    }
    struct fat16_slot s;
    if (fat16_slot_by_index(dp, index, &s) < 0) {
        return 0;
    }
    return fat16_dirent_available(&s.entry);
}

/*
 * 在目录dp中查找一段长度为need的连续可用目录项槽。
 *
 * Part2 设置关键词时，一个文件的关键词伪目录项必须连续放在标准文件目录项前面：
 *   [kw0][kw1]...[kwN-1][standard file entry]
 * 因此当新关键词需要更多槽时，不能随便找零散空槽，而必须找一段连续空间。
 *
 * old_start/old_count表示这个文件原来占用的一段槽。更新关键词时，旧范围可以被复用，
 * 所以fat16_slot_available_for_run()会把旧范围也视为可用。
 */
static int fat16_find_free_run(struct inode *dp, uint need, uint old_start, uint old_count,
                               struct fat16_slot *first_slot, uint *first_index) {
    if (need == 0) {
        return -1;
    }

    /*
     * LAB TODO [2.2]
     *
     * 请借助fat16_slot_available_for_run()实现这个函数，如果目录空间不足
     * 就调用fat16_grow_dir()扩大目录后继续查找。
     */
    /* LAB TODO [2.2] BEGIN */
    for (;;) {
        uint nslot = fat16_dir_slot_count(dp);
        uint run = 0;
        uint start = 0;

        // 实现查找长度为 need 的连续可用槽位
        for (uint i = 0; i < nslot; i++) {
            if (fat16_slot_available_for_run(dp, i, old_start, old_count)) {
                if (run == 0) {
                    start = i;
                }
                run++;
                if (run == need) {
                    if (fat16_slot_by_index(dp, start, first_slot) < 0) {
                        return -1;
                    }
                    if (first_index) {
                        *first_index = start;
                    }
                    return 0;
                }
            } else {
                run = 0;
            }
        }

        if (dp->inum == FAT16_ROOT_INUM || dp->addrs[0] == 0 || fat16_grow_dir(dp) < 0) {
            return -1;
        }
    }
    /* LAB TODO [2.2] END */
}

// 写入一个关键词伪目录项，seq是该伪目录项在整个关键词伪目录项序列中的序号，keywords是整个关键词字符串，
// data_off是该伪目录项在关键词字符串中的数据起始偏移，total_len是整个关键词字符串的长度
static void fat16_prepare_keyword_entry(struct dirent *entry, int seq,
                                        const char *keywords, int data_off, int total_len) {
    memset(entry, 0, sizeof(*entry));
    entry->name[0] = (uchar)(FAT16_KW_TYPE | (seq & 0x0f));
    entry->attr = FAT16_ATTR_KW;
    for (int i = 0; i < FAT16_KW_BYTES_PER_ENTRY; i++) {
        int p = data_off + i;
        uchar byte = 0;
        if (p < total_len) {
            byte = (uchar)keywords[p];
        }
        fat16_kw_set_data_byte(entry, i, byte);
    }
}

// 将keywords字符串写入从标准目录项索引start开始往前数的连续nentry个关键词伪目录项中
static int fat16_write_keywords_at(struct inode *dp, uint start, const char *keywords, int nentry) {
    int len = fat16_keywords_len(keywords);
    if (len < 0 || nentry < 0 || nentry > FAT16_KW_MAX_ENTRIES) {
        return -1;
    }
    int total_len = len + 1;
    for (int i = 0; i < nentry; i++) {
        struct fat16_slot s;
        if (fat16_slot_by_index(dp, start + (uint)i, &s) < 0) {
            return -1;
        }
        struct dirent keyword_entry;
        fat16_prepare_keyword_entry(&keyword_entry, i, keywords, i * FAT16_KW_BYTES_PER_ENTRY, total_len);
        fat16_write_entry(s.sector, s.offset, &keyword_entry);
    }
    return 0;
}

/*
 * 读取std_index对应文件的关键词字符串
 */
static int fat16_read_keywords_before(struct inode *dp, uint std_index, char *buf, int max) {
    if (buf == 0 || max <= 0) {
        return -1;
    }
    buf[0] = 0;
    int count = fat16_keyword_count_before(dp, std_index);
    if (count == 0) {
        return 0;
    }

    /*
     * LAB TODO [2.1]
     *
     * 使用fat16_slot_by_index()读取count个关键词伪目录项，并用fat16_kw_get_data_byte()取出其中的关键词数据
     */
    /* LAB TODO [2.1] BEGIN */
    int out = 0; // out是字符串的长度，不含末尾 '\0'
    uint start = std_index - (uint)count;
    for (int i = 0; i < count; i++) {
        struct fat16_slot slot;
        if (fat16_slot_by_index(dp, start + (uint)i, &slot) < 0) {
            return -1;
        }
        for (int j = 0; j < FAT16_KW_BYTES_PER_ENTRY; j++) {
            uchar byte = fat16_kw_get_data_byte(&slot.entry, j);
            if (byte == 0) {
                buf[out] = 0;
                return out;
            }
            if (out >= max - 1) {
                return -1;
            }
            buf[out++] = (char)byte;
        }
    }

    if (out >= max) {
        return -1;
    }
    buf[out] = 0;





























    return out;
    /* LAB TODO [2.1] END */
}

// 读取path路径对应的文件的关键词字符串到buf中，max是buf的容量
int fat16fs_get_keywords(const char *path, char *buf, int max) {
    if (path == 0 || buf == 0 || max <= 0) {
        return -1;
    }
    char name[DIRSIZ + 1];
    struct inode *dp = fat16fs_nameiparent(path, name);
    if (dp == 0) {
        return -1;
    }

    int need_unlock = fat16_inode_lock_if_needed(dp);
    if (dp->type != T_DIR) {
        if (need_unlock) {
            fat16fs_iunlock(dp);
        }
        fat16fs_iput(dp);
        return -1;
    }

    struct fat16_slot slot = {0};
    uint logical = 0;
    int found = fat16_find_slot(dp, name, 0, &slot, &logical);
    if (found != 1 || !fat16_dirent_is_regular_file(&slot.entry)) {
        if (need_unlock) {
            fat16fs_iunlock(dp);
        }
        fat16fs_iput(dp);
        return -1;
    }

    int r = fat16_read_keywords_before(dp, slot.index, buf, max);
    if (need_unlock) {
        fat16fs_iunlock(dp);
    }
    fat16fs_iput(dp);
    return r;
}

/*
 * 为path对应的普通文件设置关键词。
 *
 * 关键词以空白字符分隔，存储在标准FAT16文件目录项前面的伪目录项中。
 * 修改关键词时必须同时考虑目录项布局和内存inode状态：
 * 1. 新关键词为空：删除旧关键词伪目录项。
 * 2. 新关键词不比旧关键词长：复用原有槽位，多余旧槽标记删除。
 * 3. 新关键词更长：找一段新的连续空间，移动标准文件目录项。
 */
int fat16fs_set_keywords(const char *path, const char *keywords) {
    if (path == 0 || keywords == 0) {
        return -1;
    }
    int new_count = fat16_keyword_entries_needed(keywords);
    if (new_count < 0 || new_count > FAT16_KW_MAX_ENTRIES) {
        return -1;
    }

    char name[DIRSIZ + 1];
    struct inode *dp = fat16fs_nameiparent(path, name);
    if (dp == 0) {
        return -1;
    }

    int need_unlock = fat16_inode_lock_if_needed(dp);
    if (dp->type != T_DIR) {
        if (need_unlock) {
            fat16fs_iunlock(dp);
        }
        fat16fs_iput(dp);
        return -1;
    }

    struct fat16_slot std_slot = {0};
    uint logical = 0;
    int found = fat16_find_slot(dp, name, 0, &std_slot, &logical);
    if (found != 1 || !fat16_dirent_is_regular_file(&std_slot.entry)) {
        if (need_unlock) {
            fat16fs_iunlock(dp);
        }
        fat16fs_iput(dp);
        return -1;
    }

    char old_keywords[FAT16_KW_MAX_BYTES + 1];
    if (fat16_read_keywords_before(dp, std_slot.index, old_keywords, sizeof(old_keywords)) < 0) {
        if (need_unlock) {
            fat16fs_iunlock(dp);
        }
        fat16fs_iput(dp);
        return -1;
    }

    uint old_sector = std_slot.sector;
    uint old_offset = std_slot.offset;
    uint old_std_index = std_slot.index;
    int old_count = fat16_keyword_count_before(dp, old_std_index);
    uint old_start = old_std_index - (uint)old_count;
    uint old_total = (uint)old_count + 1;

    /*
     * LAB TODO [2.2]
     *
     * 情况一：新关键词为空
     *
     * 需要删除旧关键词占用的 old_count 个槽，但标准文件目录项 std_slot 不移动。
     */
    /* LAB TODO [2.2] BEGIN: clear keywords */

    // !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!

    /*
     * 注意（以下 3 个 TODO 均相同）：
     * 1. 如果你需要做 Bonus，则如果关键词设置成功，必须在返回前使用
     *    fat16_kw_index_update_file()更新关键词索引，否则新记录不会加入到 B+ 树中
     * 2. 无论是否做 Bonus，无论关键词是否成功设置，在返回前必须按照下面的代码释放 dp 的锁和引用，
     *    否则会导致死锁或内存泄漏
     *    if (need_unlock) {
     *        fat16fs_iunlock(dp);
     *    }
     *    fat16fs_iput(dp);
     */

    // !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!

    if (new_count == 0) {
        fat16_mark_range_deleted_except(dp, old_start, old_total, old_std_index, 1);
        fat16_kw_index_update_file(old_sector, old_offset, old_sector, old_offset,
                                   path, old_keywords, keywords);
        if (need_unlock) {
            fat16fs_iunlock(dp);
        }
        fat16fs_iput(dp);
        return 0;
    }

    /* LAB TODO [2.2] END: clear keywords */

    /*
     * LAB TODO [2.2]
     *
     * 情况二：新关键词需要的伪目录项数量不超过旧数量
     *
     * 不需要移动标准文件目录项，复用其之前的槽位写入新关键词，其余的删除
     */
    /* LAB TODO [2.2] BEGIN: reuse old keyword range */

    if (new_count <= old_count) {
        uint new_start = old_std_index - (uint)new_count;
        if (fat16_write_keywords_at(dp, new_start, keywords, new_count) < 0) {
            if (need_unlock) {
                fat16fs_iunlock(dp);
            }
            fat16fs_iput(dp);
            return -1;
        }
        fat16_mark_range_deleted_except(dp, old_start, old_total, new_start, (uint)new_count + 1);
        fat16_kw_index_update_file(old_sector, old_offset, old_sector, old_offset,
                                   path, old_keywords, keywords);
        if (need_unlock) {
            fat16fs_iunlock(dp);
        }
        fat16fs_iput(dp);
        return 0;
    }

    /* LAB TODO [2.2] END: reuse old keyword range */

    /*
     * LAB TODO [2.2]
     *
     * 情况三：新关键词需要更多伪目录项，原位置放不下
     *
     * 请使用fat16_find_free_run()寻找一段新的连续槽位来存放新关键词和文件目录项
     * 如果这个文件的inode被加载到了itable中，你需要更新itable中的inum、sector、offset等信息。
     */
    /* LAB TODO [2.2] BEGIN: move entry to larger keyword run */
    uint new_total = (uint)new_count + 1;
    uint new_start = 0;
    struct fat16_slot first_slot = {0};
    if (fat16_find_free_run(dp, new_total, old_start, old_total, &first_slot, &new_start) < 0) {
        if (need_unlock) {
            fat16fs_iunlock(dp);
        }
        fat16fs_iput(dp);
        return -1;
    }
    if (fat16_write_keywords_at(dp, new_start, keywords, new_count) < 0) {
        if (need_unlock) {
            fat16fs_iunlock(dp);
        }
        fat16fs_iput(dp);
        return -1;
    }

    uint new_std_index = new_start + (uint)new_count;
    struct fat16_slot new_std_slot = {0};
    if (fat16_slot_by_index(dp, new_std_index, &new_std_slot) < 0) {
        if (need_unlock) {
            fat16fs_iunlock(dp);
        }
        fat16fs_iput(dp);
        return -1;
    }
    fat16_write_entry(new_std_slot.sector, new_std_slot.offset, &std_slot.entry);
    fat16_mark_range_deleted_except(dp, old_start, old_total, new_start, new_total);

    uint old_inum = fat16_slot_inum(old_sector, old_offset);
    uint new_inum = fat16_slot_inum(new_std_slot.sector, new_std_slot.offset);
    acquire(&itable.lock);
    struct inode *loaded = fat16_find_loaded(old_inum);
    if (loaded) {
        loaded->inum = new_inum;
        loaded->addrs[1] = new_std_slot.sector;
        loaded->addrs[2] = new_std_slot.offset;
    }
    release(&itable.lock);

    fat16_kw_index_update_file(old_sector, old_offset, new_std_slot.sector, new_std_slot.offset,
                               path, old_keywords, keywords);
    if (need_unlock) {
        fat16fs_iunlock(dp);
    }
    fat16fs_iput(dp);
    return 0;

    /* LAB TODO [2.2] END: move entry to larger keyword run */
    return 0;
}

/* 关键词查询 */

// 对应一次查询，包含解析后的查询字符串、存储查询结果等信息
struct fat16_query_ctx {
    char query[FAT16_KW_MAX_BYTES + 1];
    char *terms[FAT16_QUERY_MAX_TERMS];
    int nterms;
    int top_k;
    char *out;
    int max;
    int out_len;
    int returned;
    char file_keywords[FAT16_KW_MAX_BYTES + 1];
};

static int fat16_keyword_sep(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// 解析查询字符串为关键词数组
static int fat16_parse_query_terms(const char *keywords, struct fat16_query_ctx *ctx) {
    int len = fat16_keywords_len(keywords);
    if (len <= 0) {
        return -1;
    }
    for (int i = 0; i <= len; i++) {
        ctx->query[i] = keywords[i];
    }

    int n = 0;
    char *p = ctx->query;
    while (*p != 0) {
        while (*p != 0 && fat16_keyword_sep(*p)) {
            *p++ = 0;
        }
        if (*p == 0) {
            break;
        }
        if (n >= FAT16_QUERY_MAX_TERMS) {
            return -1;
        }
        ctx->terms[n++] = p;
        while (*p != 0 && !fat16_keyword_sep(*p)) {
            p++;
        }
    }

    if (n == 0) {
        return -1;
    }
    ctx->nterms = n;
    return 0;
}

static int fat16_token_eq(const char *token, int len, const char *term) {
    for (int i = 0; i < len; i++) {
        if (term[i] == 0 || token[i] != term[i]) {
            return 0;
        }
    }
    return term[len] == 0;
}

// 判断文件关键词中是否包含某个完整 term
static int fat16_keywords_contain_term(const char *keywords, const char *term) {
    int i = 0;
    while (keywords[i] != 0) {
        while (keywords[i] != 0 && fat16_keyword_sep(keywords[i])) {
            i++;
        }
        if (keywords[i] == 0) {
            break;
        }
        int start = i;
        while (keywords[i] != 0 && !fat16_keyword_sep(keywords[i])) {
            i++;
        }
        if (fat16_token_eq(keywords + start, i - start, term)) {
            return 1;
        }
    }
    return 0;
}

// 判断文件关键词是否满足整个查询
static int fat16_keywords_match_query(const char *file_keywords, struct fat16_query_ctx *ctx) {
    for (int i = 0; i < ctx->nterms; i++) {
        if (!fat16_keywords_contain_term(file_keywords, ctx->terms[i])) {
            return 0;
        }
    }
    return 1;
}

static int fat16_query_limit_reached(struct fat16_query_ctx *ctx) {
    return ctx->top_k >= 0 && ctx->returned >= ctx->top_k;
}

static int fat16_query_append_result(struct fat16_query_ctx *ctx, const char *path) {
    int len = kstrlen(path);
    if (ctx->out_len + len + 1 >= ctx->max) {
        return -1;
    }
    memmove(ctx->out + ctx->out_len, path, (uint)len);
    ctx->out_len += len;
    ctx->out[ctx->out_len++] = '\n';
    ctx->out[ctx->out_len] = 0;
    ctx->returned++;
    return 0;
}

static int fat16_join_child_path(const char *parent, const char *name, char *out, int cap) {
    int plen = kstrlen(parent);
    int nlen = kstrlen(name);
    int need_slash = plen > 0 && parent[plen - 1] != '/';
    int total = plen + (need_slash ? 1 : 0) + nlen;
    if (cap <= 0 || total >= cap) {
        return -1;
    }

    int pos = 0;
    memmove(out, parent, (uint)plen);
    pos += plen;
    if (need_slash) {
        out[pos++] = '/';
    }
    memmove(out + pos, name, (uint)nlen);
    pos += nlen;
    out[pos] = 0;
    return 0;
}

typedef int (*fat16_file_visit_fn)(struct inode *dir, const struct fat16_slot *slot,
                                   const char *path, void *arg);

typedef int (*fat16_walk_stop_fn)(void *arg);

// 递归遍历目录树，访问所有可见文件
// 可以用来实现全盘的关键词查询以及B+树索引构建
static int fat16_walk_visible_files(struct inode *dp, const char *dirpath,
                                    fat16_file_visit_fn visit_file,
                                    fat16_walk_stop_fn stop,
                                    void *arg) {
    int need_unlock = fat16_inode_lock_if_needed(dp);
    if (dp->type != T_DIR) {
        if (need_unlock) {
            fat16fs_iunlock(dp);
        }
        return -1;
    }

    uint nslot = fat16_dir_slot_count(dp);
    for (uint i = 0; i < nslot; i++) {
        if (stop && stop(arg)) {
            break;
        }

        struct fat16_slot slot;
        if (fat16_slot_by_index(dp, i, &slot) < 0) {
            if (need_unlock) {
                fat16fs_iunlock(dp);
            }
            return -1;
        }
        if (slot.entry.name[0] == FAT16_NAME_FREE) {
            break;
        }
        if (!fat16_dirent_visible(&slot.entry)) {
            continue;
        }

        char name[DIRSIZ + 1];
        fat16_name_to_long(slot.entry.name, name, sizeof(name));
        if (kstrcmp(name, ".") == 0 || kstrcmp(name, "..") == 0) {
            continue;
        }

        char path[MAXPATH];
        if (fat16_join_child_path(dirpath, name, path, sizeof(path)) < 0) {
            if (need_unlock) {
                fat16fs_iunlock(dp);
            }
            return -1;
        }

        if (fat16_dirent_is_directory(&slot.entry)) {
            uint inum = fat16_slot_inum(slot.sector, slot.offset);
            struct inode *child = fat16_iget(dp->dev, inum);
            int r = fat16_walk_visible_files(child, path, visit_file, stop, arg);
            fat16fs_iput(child);
            if (r < 0) {
                if (need_unlock) {
                    fat16fs_iunlock(dp);
                }
                return -1;
            }
            continue;
        }

        if (visit_file && visit_file(dp, &slot, path, arg) < 0) {
            if (need_unlock) {
                fat16fs_iunlock(dp);
            }
            return -1;
        }
    }

    if (need_unlock) {
        fat16fs_iunlock(dp);
    }
    return 0;
}

static int fat16_query_walk_stop(void *arg) {
    return fat16_query_limit_reached((struct fat16_query_ctx *)arg);
}

// 全目录扫描查询时的文件访问回调函数
static int fat16_query_visit_file(struct inode *dir, const struct fat16_slot *slot,
                                  const char *path, void *arg) {
    struct fat16_query_ctx *ctx = (struct fat16_query_ctx *)arg;
    int n = fat16_read_keywords_before(dir, slot->index, ctx->file_keywords,
                                       sizeof(ctx->file_keywords));
    if (n < 0) {
        return -1;
    }
    if (n > 0 && fat16_keywords_match_query(ctx->file_keywords, ctx)) {
        return fat16_query_append_result(ctx, path);
    }
    return 0;
}

// 递归扫描目录树完成关键词查询
static int fat16_query_dir(struct inode *dp, const char *dirpath, struct fat16_query_ctx *ctx) {
    return fat16_walk_visible_files(dp, dirpath, fat16_query_visit_file,
                                    fat16_query_walk_stop, ctx);
}

// 不使用索引，递归遍历整个文件系统进行关键词查询
int fat16fs_query_file(const char *keywords, int top_k, char *buf, int max) {
    if (keywords == 0 || buf == 0 || max <= 0 || top_k < -1) {
        return -1;
    }

    struct fat16_query_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.top_k = top_k;
    ctx.out = buf;
    ctx.max = max;
    ctx.out[0] = 0;

    if (fat16_parse_query_terms(keywords, &ctx) < 0) {
        return -1;
    }
    if (top_k == 0) {
        return 0;
    }

    struct inode *root = fat16_iget((uint)rootdev, FAT16_ROOT_INUM);
    int r = fat16_query_dir(root, "/", &ctx);
    fat16fs_iput(root);
    if (r < 0) {
        return -1;
    }
    return ctx.returned;
}

/* B+树关键词索引 */

// 关键词索引页，用于存放B+树节点数据，分配自内存页，每页维护一个链表指向下一页
struct fat16_kw_index_page {
    struct fat16_kw_index_page *next;
    uint used;
    uchar data[PGSIZE - sizeof(struct fat16_kw_index_page *) - sizeof(uint)];
};

// 对于每一个被存到关键词B+树中的文件，都在内存中维护一个 fat16_kw_index_file 结构，存储该文件对应的目录项位置、路径、关键词等信息
struct fat16_kw_index_file {
    uint sector;
    uint offset;
    int active;
    char path[MAXPATH];
    struct fat16_kw_index_file *next;
};

// 关键词索引系统的全局状态，包括锁、B+树、内存页链表、文件信息链表等
static struct {
    struct spinlock lock;
    int inited;
    int ready;
    int oom;
    struct bptree tree;
    struct fat16_kw_index_file *files;
    struct fat16_kw_index_page *pages;
} fat16_kw_index;

static void fat16_kw_index_init_once(void) {
    if (!fat16_kw_index.inited) {
        initlock(&fat16_kw_index.lock, "fat16_kw_index");
        bptree_init(&fat16_kw_index.tree, fat16_kw_index_alloc, 0);
        fat16_kw_index.inited = 1;
    }
}

static uint fat16_kw_align(uint n) {
    uint a = sizeof(uint64) - 1;
    return (n + a) & ~a;
}

static void fat16_kw_index_free_pages(void) {
    struct fat16_kw_index_page *p = fat16_kw_index.pages;
    while (p) {
        struct fat16_kw_index_page *next = p->next;
        kfree((void *)p);
        p = next;
    }
    fat16_kw_index.pages = 0;
}

// 为关键词索引和 B+ 树分配内存
static void *fat16_kw_index_alloc(uint n, void *arg) {
    (void)arg;
    n = fat16_kw_align(n);
    if (n == 0 || n > sizeof(((struct fat16_kw_index_page *)0)->data)) {
        fat16_kw_index.oom = 1;
        return 0;
    }

    struct fat16_kw_index_page *p = fat16_kw_index.pages;
    if (p == 0 || p->used + n > sizeof(p->data)) {
        p = (struct fat16_kw_index_page *)kalloc();
        if (p == 0) {
            fat16_kw_index.oom = 1;
            return 0;
        }
        memset(p, 0, PGSIZE);
        p->next = fat16_kw_index.pages;
        fat16_kw_index.pages = p;
    }

    void *out = p->data + p->used;
    p->used += n;
    memset(out, 0, n);
    return out;
}

static void *fat16_kw_alloc(uint n) {
    return fat16_kw_index_alloc(n, 0);
}

static void fat16_kw_path_copy(char *dst, const char *src) {
    int i = 0;
    if (src == 0 || src[0] == 0) {
        dst[0] = '/';
        dst[1] = 0;
        return;
    }
    while (i < MAXPATH - 1 && src[i] != 0) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static int fat16_kw_ref_eq(struct fat16_kw_index_file *file, uint sector, uint offset) {
    return file && file->sector == sector && file->offset == offset;
}

// 在索引文件记录链表中查找某个目录项对应的文件记录
static struct fat16_kw_index_file *fat16_kw_index_find_file(uint sector, uint offset) {
    for (struct fat16_kw_index_file *f = fat16_kw_index.files; f; f = f->next) {
        if (fat16_kw_ref_eq(f, sector, offset)) {
            return f;
        }
    }
    return 0;
}

// 创建一个关键词索引用的文件记录
static struct fat16_kw_index_file *fat16_kw_index_create_file(uint sector, uint offset,
                                                          const char *path) {
    struct fat16_kw_index_file *file =
        (struct fat16_kw_index_file *)fat16_kw_alloc(sizeof(*file));
    if (file == 0) {
        return 0;
    }
    file->sector = sector;
    file->offset = offset;
    file->active = 1;
    fat16_kw_path_copy(file->path, path);
    file->next = fat16_kw_index.files;
    fat16_kw_index.files = file;
    return file;
}

// 将某个文件的所有关键词插入 B+ 树索引
static int fat16_kw_index_add_tokens(const char *keywords, struct fat16_kw_index_file *file) {
    int i = 0;
    while (keywords && keywords[i] != 0) {
        while (keywords[i] != 0 && fat16_keyword_sep(keywords[i])) {
            i++;
        }
        if (keywords[i] == 0) {
            break;
        }
        int start = i;
        while (keywords[i] != 0 && !fat16_keyword_sep(keywords[i])) {
            i++;
        }
        if (bptree_insert(&fat16_kw_index.tree, keywords + start, i - start, file) < 0) {
            return -1;
        }
    }
    return 0;
}

// 从 B+ 树索引中移除某文件对应的一组关键词映射
static void fat16_kw_index_remove_tokens(const char *keywords,
                                         struct fat16_kw_index_file *file) {
    int i = 0;
    while (keywords && keywords[i] != 0) {
        while (keywords[i] != 0 && fat16_keyword_sep(keywords[i])) {
            i++;
        }
        if (keywords[i] == 0) {
            break;
        }
        int start = i;
        while (keywords[i] != 0 && !fat16_keyword_sep(keywords[i])) {
            i++;
        }
        struct bptree_values *values =
            bptree_lookup(&fat16_kw_index.tree, keywords + start, i - start);
        if (values) {
            bptree_values_remove(values, file);
        }
    }
}

// 将一个文件加入关键词索引
static int fat16_kw_index_add_file(uint sector, uint offset, const char *path,
                                   const char *keywords) {
    if (keywords == 0 || keywords[0] == 0) {
        return 0;
    }
    struct fat16_kw_index_file *file = fat16_kw_index_find_file(sector, offset);
    if (file == 0) {
        file = fat16_kw_index_create_file(sector, offset, path);
        if (file == 0) {
            return -1;
        }
    } else {
        file->active = 1;
        fat16_kw_path_copy(file->path, path);
    }
    return fat16_kw_index_add_tokens(keywords, file);
}

// 递归扫描目录时的回调函数，用于把一个文件加入关键词索引
static int fat16_kw_index_visit_file(struct inode *dir, const struct fat16_slot *slot,
                                      const char *path, void *arg) {
    (void)arg;
    char file_keywords[FAT16_KW_MAX_BYTES + 1];
    int n = fat16_read_keywords_before(dir, slot->index, file_keywords,
                                       sizeof(file_keywords));
    if (n < 0) {
        return -1;
    }
    if (n == 0) {
        return 0;
    }
    return fat16_kw_index_add_file(slot->sector, slot->offset, path, file_keywords);
}

static int fat16_kw_index_scan_dir(struct inode *dp, const char *dirpath) {
    return fat16_walk_visible_files(dp, dirpath, fat16_kw_index_visit_file, 0, 0);
}

// 重建整个关键词索引
static void fat16_kw_index_rebuild(void) {
    fat16_kw_index_free_pages();
    bptree_reset(&fat16_kw_index.tree);
    fat16_kw_index.files = 0;
    fat16_kw_index.ready = 0;
    fat16_kw_index.oom = 0;

    struct inode *root = fat16_iget((uint)rootdev, FAT16_ROOT_INUM);
    int r = fat16_kw_index_scan_dir(root, "/");
    fat16fs_iput(root);
    if (r == 0 && !fat16_kw_index.oom) {
        fat16_kw_index.ready = 1;
    } else {
        fat16_kw_index.ready = 0;
        printf("[fat16fs] keyword index build failed\n");
    }
}

// 在 setkeywords() 修改关键词或迁移目录项后，同步更新内存 B+ 树索引
static void fat16_kw_index_update_file(uint old_sector, uint old_offset,
                                       uint new_sector, uint new_offset,
                                       const char *path,
                                       const char *old_keywords,
                                       const char *new_keywords) {
    acquire(&fat16_kw_index.lock);
    if (!fat16_kw_index.ready) {
        release(&fat16_kw_index.lock);
        return;
    }
    struct fat16_kw_index_file *file = fat16_kw_index_find_file(old_sector, old_offset);
    if (file) {
        fat16_kw_index_remove_tokens(old_keywords, file);
    }
    if (new_keywords == 0 || new_keywords[0] == 0) {
        if (file) {
            file->sector = new_sector;
            file->offset = new_offset;
            fat16_kw_path_copy(file->path, path);
            file->active = 0;
        }
        release(&fat16_kw_index.lock);
        return;
    }
    if (file == 0) {
        file = fat16_kw_index_create_file(new_sector, new_offset, path);
        if (file == 0) {
            fat16_kw_index.ready = 0;
            release(&fat16_kw_index.lock);
            return;
        }
    }
    file->sector = new_sector;
    file->offset = new_offset;
    file->active = 1;
    fat16_kw_path_copy(file->path, path);
    if (fat16_kw_index_add_tokens(new_keywords, file) < 0) {
        fat16_kw_index.ready = 0;
    }
    release(&fat16_kw_index.lock);
}

// 文件被删除时，从关键词索引中移除它
static void fat16_kw_index_remove_file(uint sector, uint offset, const char *old_keywords) {
    acquire(&fat16_kw_index.lock);
    if (!fat16_kw_index.ready) {
        release(&fat16_kw_index.lock);
        return;
    }
    struct fat16_kw_index_file *file = fat16_kw_index_find_file(sector, offset);
    if (file) {
        fat16_kw_index_remove_tokens(old_keywords, file);
        file->active = 0;
    }
    release(&fat16_kw_index.lock);
}

static int fat16_query_append_file_path(struct fat16_query_ctx *ctx,
                                        struct fat16_kw_index_file *file) {
    if (file == 0 || !file->active) {
        return 0;
    }
    return fat16_query_append_result(ctx, file->path);
}

// 使用 B+ 树关键词索引进行快速查询
int fat16fs_query_file_indexed(const char *keywords, int top_k, char *buf, int max) {
    if (keywords == 0 || buf == 0 || max <= 0 || top_k < -1) {
        return -1;
    }

    struct fat16_query_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.top_k = top_k;
    ctx.out = buf;
    ctx.max = max;
    ctx.out[0] = 0;

    if (fat16_parse_query_terms(keywords, &ctx) < 0) {
        return -1;
    }
    if (top_k == 0) {
        return 0;
    }

    acquire(&fat16_kw_index.lock);
    if (!fat16_kw_index.ready) {
        release(&fat16_kw_index.lock);
        return -1;
    }

    struct bptree_values *postings[FAT16_QUERY_MAX_TERMS];
    int base = 0;
    for (int i = 0; i < ctx.nterms; i++) {
        int len = kstrlen(ctx.terms[i]);
        postings[i] = bptree_lookup(&fat16_kw_index.tree, ctx.terms[i], len);
        if (postings[i] == 0 || postings[i]->count == 0) {
            release(&fat16_kw_index.lock);
            return 0;
        }
        if (postings[i]->count < postings[base]->count) {
            base = i;
        }
    }

    for (struct bptree_value_chunk *chunk = postings[base]->head;
         chunk && !fat16_query_limit_reached(&ctx);
         chunk = chunk->next) {
        for (int i = 0; i < chunk->n && !fat16_query_limit_reached(&ctx); i++) {
            struct fat16_kw_index_file *file = (struct fat16_kw_index_file *)chunk->values[i];
            if (file == 0 || !file->active) {
                continue;
            }
            int matched = 1;
            for (int t = 0; t < ctx.nterms; t++) {
                if (t == base) {
                    continue;
                }
                if (!bptree_values_contains(postings[t], file)) {
                    matched = 0;
                    break;
                }
            }
            if (matched && fat16_query_append_file_path(&ctx, file) < 0) {
                release(&fat16_kw_index.lock);
                return -1;
            }
        }
    }

    release(&fat16_kw_index.lock);
    return ctx.returned;
}

/* 调试辅助函数 */

void fat16fs_read_file(const char *path) {
    if (path == 0) {
        return;
    }
    struct inode *ip = fat16fs_namei(path);
    if (ip == 0) {
        printf("[fat16fs] %s not found\n", (char *)path);
        return;
    }
    fat16fs_ilock(ip);
    if (ip->type != T_FILE) {
        printf("[fat16fs] %s is not a regular file\n", (char *)path);
        fat16fs_iunlockput(ip);
        return;
    }
    uint size = ip->size;
    printf("[fat16fs] Reading %s (%d bytes)\n", (char *)path, size);
    printf("--- File Content Start ---\n");
    uchar chunk[128];
    uint off = 0;
    while (off < size) {
        uint n = min_uint(sizeof(chunk), size - off);
        int rd = fat16fs_readi(ip, off, chunk, n);
        if (rd < 0 || (uint)rd != n) {
            printf("\n[fat16fs] read failed at off=%d\n", off);
            break;
        }
        for (uint i = 0; i < n; i++) {
            uart_putc((char)chunk[i]);
        }
        off += n;
    }
    printf("\n--- File Content End ---\n");
    fat16fs_iunlock(ip);
    fat16fs_iput(ip);
}
