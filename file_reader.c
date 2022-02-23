#include "file_reader.h"

#include <byteswap.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

uint8_t* clean_file_name(uint8_t* name, uint8_t* ext) {
    if (name == NULL || ext == NULL) {
        return NULL;
    }

    uint8_t* out = calloc(13, 1);
    if (out == NULL) {
        return NULL;
    }

    memcpy(out, name, 8);

    uint8_t name_end = 8;
    for (uint8_t i = 0; i < name_end; i++) {
        if (out[i] == ' ') {
            name_end = i;
            break;
        }
    }

    // End here if no extension
    if (ext[0] == ' ') {
        out[name_end] = '\0';
        return out;
    }

    out[name_end] = '.';
    name_end++;

    memcpy(out + name_end, ext, 3);
    for (; name_end < 13; name_end++) {
        if (out[name_end] == ' ') {
            out[name_end] = '\0';
            break;
        }
    }

    return out;
}

char* make_all_caps(const char* text, size_t n) {
    if (text == NULL) {
        return NULL;
    }

    char* out = calloc(n + 1, 1);
    if (out == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < n; i++) {
        if (text[i] >= 'a' && text[i] <= 'z') {
            out[i] = text[i] - 32;
        } else {
            out[i] = text[i];
        }
    }

    return out;
}

struct disk_t* disk_open_from_file(const char* volume_file_name) {
    if (volume_file_name == NULL) {
        errno = EFAULT;
        return NULL;
    }

    FILE* fd = fopen(volume_file_name, "r");
    if (fd == NULL) {
        return NULL;
    }

    struct disk_t* disk = malloc(sizeof(struct disk_t));
    if (disk == NULL) {
        fclose(fd);
        errno = ENOMEM;
        return NULL;
    }
    disk->fd = fd;

    fseek(fd, 0, SEEK_END);
    disk->file_len = ftell(fd);
    disk->sectors = disk->file_len / BYTES_PER_SECTOR;
    fseek(fd, 0, SEEK_SET);

    return disk;
}

int disk_read(struct disk_t* pdisk, int32_t first_sector, void* buffer,
              int32_t sectors_to_read) {
    if (pdisk == NULL || buffer == NULL) {
        errno = EFAULT;
        return -1;
    }

    int first_byte = first_sector * BYTES_PER_SECTOR;

    if ((uint32_t)(first_sector + sectors_to_read) > pdisk->sectors) {
        errno = ERANGE;
        return -1;
    }

    fseek(pdisk->fd, first_byte, SEEK_SET);
    fread(buffer, BYTES_PER_SECTOR, sectors_to_read, pdisk->fd);
    fseek(pdisk->fd, 0, SEEK_SET);

    return sectors_to_read;
}

int disk_close(struct disk_t* pdisk) {
    if (pdisk == NULL) {
        errno = EFAULT;
        return -1;
    }

    fclose(pdisk->fd);
    free(pdisk);

    return 0;
}

struct volume_t* fat_open(struct disk_t* pdisk, uint32_t first_sector) {
    unsigned char buf[BYTES_PER_SECTOR];
    if (disk_read(pdisk, first_sector, buf, 1) == -1) {
        // disk_read sets errno for invalid pdisk pointer and buffer
        return NULL;
    }

    struct boot_record_t* boot_record = (struct boot_record_t*)buf;

    // Extended boot record signature
    if (boot_record->ebpb_signature != 0x28 && boot_record->ebpb_signature != 0x29)
        goto validation_error;

    // Boot sector signature
    if (boot_record->boot_signature[0] != 0x55 ||
        boot_record->boot_signature[1] != 0xAA)
        goto validation_error;

    struct volume_t* volume = malloc(sizeof(struct volume_t));
    if (volume == NULL) {
        free(boot_record);
        goto memory_error;
    }

    uint16_t fat_size = boot_record->sectors_per_fat * BYTES_PER_SECTOR;

    uint16_t* fat = malloc(fat_size);
    if (fat == NULL) {
        free(boot_record);
        free(volume);
        goto memory_error;
    }

    if (disk_read(pdisk, boot_record->reserved_sectors, fat,
                  boot_record->sectors_per_fat) == -1) {
        free(boot_record);
        free(volume);
        free(fat);
        goto memory_error;
    }

    uint8_t dir_entry_size = sizeof(struct root_entry_t);
    uint32_t root_dir_size = boot_record->root_entries * dir_entry_size;
    uint16_t root_dir_sectors = root_dir_size / BYTES_PER_SECTOR;
    uint32_t root_dir_start = boot_record->reserved_sectors +
                              boot_record->fat_number * boot_record->sectors_per_fat;

    uint8_t* root_dir = malloc(root_dir_size);
    if (root_dir == NULL) {
        free(boot_record);
        free(volume);
        free(fat);
        goto memory_error;
    }

    if (disk_read(pdisk, root_dir_start, root_dir, root_dir_sectors) == -1) {
        free(boot_record);
        free(volume);
        free(fat);
        free(root_dir);
        goto memory_error;
    }

    volume->root_entries_n = 0;
    volume->root_entries = NULL;

    for (uint16_t i = 0; i < boot_record->root_entries; i++) {
        uint32_t offset = i * dir_entry_size;
        struct root_entry_t* entry = (struct root_entry_t*)&root_dir[offset];

        if (entry->name[0] == 0) break;
        if (entry->name[0] == 0xE5 || entry->attributes == 0x0F) continue;

        volume->root_entries_n++;

        struct root_entry_t** newptr =
            realloc(volume->root_entries,
                    volume->root_entries_n * sizeof(struct root_entry_t*));
        if (newptr == NULL) {
            free(boot_record);
            free(volume);
            free(fat);
            free(root_dir);
            if (volume->root_entries != NULL) free(volume->root_entries);
            goto memory_error;
        }

        volume->root_entries = newptr;
        volume->root_entries[volume->root_entries_n - 1] = entry;
    }

    volume->boot_record = boot_record;
    volume->disk = pdisk;
    volume->fat = fat;
    volume->root_dir = root_dir;
    volume->first_data_sector = root_dir_start;
    volume->sectors_per_cluster = boot_record->sectors_per_cluster;
    volume->bytes_per_cluster = boot_record->sectors_per_cluster * BYTES_PER_SECTOR;
    volume->data_start = boot_record->reserved_sectors + boot_record->hidden_sectors +
                         (boot_record->fat_number * boot_record->sectors_per_fat) +
                         (boot_record->root_entries / 16);

    return volume;

memory_error:
    errno = ENOMEM;
    return NULL;

validation_error:
    errno = EINVAL;
    return NULL;
}

int fat_close(struct volume_t* pvolume) {
    if (pvolume == NULL) {
        errno = EFAULT;
        return -1;
    }

    free(pvolume->root_entries);
    free(pvolume->fat);
    free(pvolume->root_dir);
    free(pvolume);

    return 0;
}

struct root_entry_t* find_file(struct volume_t* pvolume, const char* path) {
    if (pvolume == NULL || path == NULL) {
        return NULL;
    }

    uint8_t dir_entry_size = sizeof(struct root_entry_t);
    struct root_entry_t* current_entry = NULL;

    char* part;
    while ((part = strsep((char**)&path, "\\")) != NULL) {
        if (strlen(part) == 0) continue;

        bool new_entry = false;
        bool found_root = false;

        // Root dir
        if (current_entry == NULL) {
            for (uint16_t i = 0; i < pvolume->root_entries_n; i++) {
                struct root_entry_t* entry = pvolume->root_entries[i];
                uint8_t* name = clean_file_name(entry->name, entry->ext);

                if (strcmp((char*)name, part) != 0) {
                    free(name);
                    continue;
                }

                free(name);

                current_entry = malloc(dir_entry_size);
                memcpy(current_entry, entry, dir_entry_size);
                new_entry = true;
                break;
            }

            // No entry was found - path doesn't exist
            if (!new_entry) {
                return NULL;
            }

            continue;
        }

        // Other entries
        else if ((current_entry->attributes >> 4) & 1) {
            uint16_t current_cluster = current_entry->first_cluster;

            uint8_t* buf = malloc(pvolume->bytes_per_cluster);
            struct root_entry_t* entry = malloc(dir_entry_size);

            while (!new_entry) {
                uint32_t sector = pvolume->data_start + ((current_cluster - 2) *
                                                         pvolume->sectors_per_cluster);

                if (disk_read(pvolume->disk, sector, buf,
                              pvolume->sectors_per_cluster) == -1) {
                    free(buf);
                    return NULL;
                }

                // Read dir entries
                uint16_t i = 0;
                while (true) {
                    uint32_t offset = i * dir_entry_size;
                    i++;
                    if (entry == NULL) break;
                    memcpy(entry, buf + offset, dir_entry_size);

                    if (entry->name[0] == 0) break;
                    if (entry->name[0] == 0xE5 || entry->attributes == 0x0F) continue;

                    uint8_t* name = clean_file_name(entry->name, entry->ext);

                    if (strcmp((char*)name, part) != 0) {
                        free(name);
                        continue;
                    }

                    new_entry = true;

                    free(name);

                    if (entry->first_cluster != 0) {
                        memcpy(current_entry, entry, dir_entry_size);
                    } else {
                        found_root = true;
                    }

                    break;
                }

                if (!new_entry) {
                    current_cluster = pvolume->fat[current_cluster];
                }
            }

            free(entry);
            free(buf);

            // No file found - path doesn't exist
            if (!new_entry) {
                return NULL;
            }

            if (found_root) {
                free(current_entry);
                current_entry = NULL;
            }
        } else {
            // If previous part is a file then there can't be any more parts after
            return NULL;
        }
    }

    return current_entry;
}

struct file_t* file_open(struct volume_t* pvolume, const char* file_name) {
    if (pvolume == NULL || file_name == NULL) {
        errno = EFAULT;
        return NULL;
    }

    char* search_name = make_all_caps(file_name, strlen(file_name));

    // Find an entry with the correct path
    struct root_entry_t* entry = find_file(pvolume, search_name);
    if (entry == NULL) {
        free(search_name);
        errno = ENOENT;
        return NULL;
    }

    free(search_name);

    // Don't try opening directories or volumes
    if ((entry->attributes >> 3) & 1 || (entry->attributes >> 4) & 1) {
        free(entry);
        errno = EISDIR;
        return NULL;
    }

    struct file_t* fd = malloc(sizeof(struct file_t));
    if (fd == NULL) {
        free(entry);
        goto memory_error;
    }

    uint8_t* name = clean_file_name(entry->name, entry->ext);
    memcpy(fd->name, name, strlen((const char*)name) + 1);
    free(name);
    fd->size = entry->size;
    fd->attributes = entry->attributes;
    fd->read_head = 0;
    fd->volume = pvolume;

    struct cluster_t* current_cluster = malloc(sizeof(struct cluster_t));
    if (current_cluster == NULL) {
        free(entry);
        free(fd);
        goto memory_error;
    }

    uint16_t sectors_per_cluster = pvolume->sectors_per_cluster;

    current_cluster->number = entry->first_cluster;
    current_cluster->sector =
        pvolume->data_start + ((entry->first_cluster - 2) * sectors_per_cluster);
    current_cluster->next = NULL;

    fd->clusters = current_cluster;

    while (true) {
        uint16_t new_cluster_number = pvolume->fat[current_cluster->number];

        if (new_cluster_number >= 0xFFF8) {
            break;
        }

        struct cluster_t* new_cluster = malloc(sizeof(struct cluster_t));
        if (new_cluster == NULL) {
            struct cluster_t* tmp;
            while (fd->clusters != NULL) {
                tmp = fd->clusters;
                fd->clusters = fd->clusters->next;
                free(tmp);
            }
            free(entry);
            free(fd);
            goto memory_error;
        }

        new_cluster->number = new_cluster_number;
        new_cluster->sector =
            pvolume->data_start + ((new_cluster_number - 2) * sectors_per_cluster);
        new_cluster->next = NULL;

        current_cluster->next = new_cluster;
        current_cluster = new_cluster;
    }

    free(entry);

    return fd;

memory_error:
    errno = ENOMEM;
    return NULL;
}

int file_close(struct file_t* stream) {
    if (stream == NULL) {
        errno = EFAULT;
        return -1;
    }

    struct cluster_t* head = stream->clusters;
    struct cluster_t* tmp;
    while (head != NULL) {
        tmp = head;
        head = head->next;
        free(tmp);
    }
    free(stream);

    return 0;
}

size_t file_read(void* ptr, size_t size, size_t nmemb, struct file_t* stream) {
    if (ptr == NULL || stream == NULL) {
        // puts("ptr stream null");
        errno = EFAULT;
        return -1;
    }

    if (stream->read_head >= stream->size) {
        return 0;
    }

    uint16_t sectors_per_cluster = stream->volume->sectors_per_cluster;
    uint32_t bytes_per_cluster = stream->volume->bytes_per_cluster;
    struct cluster_t* current_cluster = stream->clusters;
    uint8_t* out = (uint8_t*)ptr;

    uint8_t* buf = malloc(bytes_per_cluster);
    if (buf == NULL) {
        errno = ENOMEM;
        return -1;
    }

    uint32_t bytes_left = size * nmemb;
    uint32_t pos = 0;
    uint32_t bytes_read = 0;

    while (current_cluster != NULL && bytes_left > 0) {
        // Read a full sector to `buf`
        if (disk_read(stream->volume->disk, current_cluster->sector, buf,
                      sectors_per_cluster) == -1) {
            free(buf);
            return -1;
        }

        current_cluster = current_cluster->next;

        uint32_t pos_in_cluster = 0;

        while (bytes_left > 0 && pos_in_cluster < bytes_per_cluster &&
               pos < stream->size) {
            if (pos >= stream->read_head) {
                out[bytes_read] = buf[pos_in_cluster];

                stream->read_head++;

                bytes_read++;
                bytes_left--;
            }

            pos_in_cluster++;
            pos++;
        }
    }

    free(buf);

    return (size_t)floor(bytes_read / size);
}

int32_t file_seek(struct file_t* stream, int32_t offset, int whence) {
    if (stream == NULL) {
        errno = EFAULT;
        return -1;
    }

    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
        errno = EINVAL;
        return -1;
    }

    switch (whence) {
        case SEEK_SET:
            // Offset can't be larger than stream->size
            if (offset > stream->size) goto bounds_error;
            stream->read_head = offset;
            break;
        case SEEK_END:
            // Offset must not be positive
            if (stream->size + offset > stream->size) goto bounds_error;
            stream->read_head = stream->size + offset;
            break;
        case SEEK_CUR:
            // New position must not exceed stream->size
            if (stream->read_head + offset > stream->size) goto bounds_error;
            stream->read_head += offset;
            break;
    }

    return stream->read_head;

bounds_error:
    errno = ENXIO;
    return -1;
}

struct dir_t* dir_open(struct volume_t* pvolume, const char* dir_path) {
    if (pvolume == NULL) {
        errno = EFAULT;
        return NULL;
    }

    struct dir_t* dir = malloc(sizeof(struct dir_t));
    if (dir == NULL) {
        goto memory_error;
    }
    dir->read_head = 0;
    dir->volume = pvolume;

    char* clean_path = make_all_caps(dir_path, strlen(dir_path));
    if (clean_path == NULL) {
        free(dir);
        goto memory_error;
    }

    // No need to search for root dir
    // FIXME: If a path leads back to the root dir then we have a problem
    if (strcmp(clean_path, "\\") == 0) {
        dir->entries = pvolume->root_entries;
        dir->entries_n = pvolume->root_entries_n;
        free(clean_path);
        return dir;
    }

    // FIXME: Should probalby write a separate function for directories
    struct root_entry_t* entry = find_file(pvolume, clean_path);
    if (entry == NULL) {
        free(dir);
        free(clean_path);
        errno = ENOENT;
        return NULL;
    }

    free(clean_path);

    // Check if entry is a directory
    if (!((entry->attributes >> 4) & 1) || ((entry->attributes >> 3) & 1)) {
        free(dir);
        free(entry);
        errno = ENOTDIR;
        return NULL;
    }

    uint8_t* buf = malloc(pvolume->bytes_per_cluster);
    uint8_t dir_entry_size = sizeof(struct root_entry_t);
    struct root_entry_t** entries = NULL;
    size_t entries_n = 0;
    uint16_t current_cluster = entry->first_cluster;
    bool searching = true;

    while (searching) {
        uint32_t sector = pvolume->data_start +
                          ((current_cluster - 2) * pvolume->sectors_per_cluster);

        if (disk_read(pvolume->disk, sector, buf, pvolume->sectors_per_cluster) == -1) {
            free(dir);
            free(buf);
            return NULL;
        }

        int i = 0;
        while (true) {
            uint32_t offset = i * dir_entry_size;
            if (offset > (pvolume->bytes_per_cluster - dir_entry_size)) break;

            i++;
            struct root_entry_t* e = malloc(dir_entry_size);
            if (e == NULL) {
                free(dir);
                free(buf);
                goto memory_error;
            }

            memcpy(e, buf + offset, dir_entry_size);

            // Saerch ends with the first entry that has name[0] == 0x00
            if (e->name[0] == 0) {
                searching = false;
                free(e);
                break;
            }
            if (e->name[0] == 0xE5 || e->attributes == 0x0F) {
                free(e);
                continue;
            }

            entries_n++;

            struct root_entry_t** newptr = realloc(entries, entries_n * dir_entry_size);
            if (newptr == NULL) {
                free(dir);
                free(buf);
                goto memory_error;
            }

            entries = newptr;
            entries[entries_n - 1] = e;
        }

        if (pvolume->fat[current_cluster] >= 0xFFF8) {
            searching = false;
        }

        if (searching) {
            current_cluster = pvolume->fat[current_cluster];
        }
    }

    free(entry);
    free(buf);

    dir->entries = entries;
    dir->entries_n = entries_n;

    return dir;

memory_error:
    errno = ENOMEM;
    return NULL;
}

int dir_read(struct dir_t* pdir, struct dir_entry_t* pentry) {
    if (pdir == NULL || pentry == NULL) {
        errno = EFAULT;
        return -1;
    }

    if (pdir->entries == NULL) {
        errno = EIO;
        return -1;
    }

    // Reached end of dir
    if (pdir->read_head == pdir->entries_n) return 1;

    struct root_entry_t* entry = pdir->entries[pdir->read_head];

    uint8_t* name = clean_file_name(entry->name, entry->ext);
    memcpy(pentry->name, name, strlen((const char*)name) + 1);
    free(name);

    pentry->size = entry->size;
    pentry->is_readonly = ((entry->attributes >> 0) & 1);
    pentry->is_hidden = ((entry->attributes >> 1) & 1);
    pentry->is_system = ((entry->attributes >> 2) & 1);
    pentry->is_archived = ((entry->attributes >> 5) & 1);
    pentry->is_directory = entry->size == 0;

    pdir->read_head++;

    return 0;
}

int dir_close(struct dir_t* pdir) {
    if (pdir == NULL) {
        errno = EFAULT;
        return -1;
    }

    // Only free pdir->entries if they are not the root directory
    if (pdir->entries != pdir->volume->root_entries) {
        for (uint16_t i = 0; i < pdir->entries_n; i++) {
            free(pdir->entries[i]);
        }

        free(pdir->entries);
    }

    free(pdir);

    return 0;
}
