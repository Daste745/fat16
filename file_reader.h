#ifndef FAT_H
#define FAT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define BYTES_PER_SECTOR 512

struct disk_t {
    FILE* fd;
    uint32_t file_len;
    uint32_t sectors;
};

struct volume_t {
    struct boot_record_t* boot_record;
    struct disk_t* disk;
    uint16_t* fat;
    uint8_t* root_dir;
    size_t root_entries_n;
    struct root_entry_t** root_entries;
    uint32_t first_data_sector;
    uint8_t sectors_per_cluster;
    uint32_t bytes_per_cluster;
    uint32_t data_start;
};

struct boot_record_t {
    // BPB
    uint8_t jump_code[3];
    uint8_t identifier[8];
    uint16_t small_sector_count;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_number;
    uint16_t root_entries;
    uint16_t total_sectors;
    uint8_t media_descriptor;
    uint16_t sectors_per_fat;
    uint16_t sectors_per_track;
    uint16_t sides;
    uint32_t hidden_sectors;
    uint32_t large_sector_count;
    // EBPB
    uint8_t drive_number;
    uint8_t reserved;
    uint8_t ebpb_signature;
    uint32_t volume_id;
    uint8_t label[11];
    uint8_t fat_type[8];
    uint8_t boot_code[448];
    uint8_t boot_signature[2];
} __attribute__((packed));

struct root_entry_t {
    uint8_t name[8];
    uint8_t ext[3];
    uint8_t attributes;
    uint8_t reserved;
    uint8_t creation_time_tenths;
    uint16_t creation_time;
    uint16_t creation_date;
    uint16_t last_access;
    uint16_t exfat_reserved;
    uint16_t mod_time;
    uint16_t mod_date;
    uint16_t first_cluster;
    // Size in bytes
    uint32_t size;
} __attribute__((packed));

struct file_t {
    char name[13];
    uint16_t attributes;
    uint16_t size;
    uint16_t read_head;
    // Linked list of clusters
    struct cluster_t* clusters;
    struct volume_t* volume;
};

struct cluster_t {
    uint16_t number;
    uint32_t sector;
    struct cluster_t* next;
};

struct dir_t {
    struct root_entry_t** entries;
    uint16_t entries_n;
    uint16_t read_head;
    struct volume_t* volume;
};

struct dir_entry_t {
    char name[13];
    uint32_t size;
    bool is_archived;
    bool is_readonly;
    bool is_system;
    bool is_hidden;
    bool is_directory;
};

struct disk_t* disk_open_from_file(const char* volume_file_name);
int disk_read(struct disk_t* pdisk, int32_t first_sector, void* buffer,
              int32_t sectors_to_read);
int disk_close(struct disk_t* pdisk);

struct volume_t* fat_open(struct disk_t* pdisk, uint32_t first_sector);
int fat_close(struct volume_t* pvolume);

struct file_t* file_open(struct volume_t* pvolume, const char* file_name);
int file_close(struct file_t* stream);
size_t file_read(void* ptr, size_t size, size_t nmemb, struct file_t* stream);
int32_t file_seek(struct file_t* stream, int32_t offset, int whence);

struct dir_t* dir_open(struct volume_t* pvolume, const char* dir_path);
int dir_read(struct dir_t* pdir, struct dir_entry_t* pentry);
int dir_close(struct dir_t* pdir);

#endif  // FAT_H
