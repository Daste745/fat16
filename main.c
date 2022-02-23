#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "file_reader.h"

int main() {
    struct disk_t* disk = disk_open_from_file("example-fat16.img");
    if (disk == NULL) {
        perror("disk_open_from_file");
    }

    struct volume_t* volume = fat_open(disk, 0);
    if (volume == NULL) {
        perror("fat_open");
    }

    // Test root dir read
    struct dir_t* dir = dir_open(volume, "\\");
    struct dir_entry_t entry;
    while (dir_read(dir, &entry) == 0) {
        printf("%12s size=%5d dir=%d\n", entry.name, entry.size, entry.is_directory);
    }

    if (dir_close(dir) == -1) {
        perror("dir_close");
    }

    // Test file read
    struct file_t* file = file_open(volume, "\\SOME.TXT");
    if (file == NULL) {
        perror("file_open");
    }

    uint8_t* buf = calloc(file->size, 1);
    size_t n = file_read(buf, 1, file->size, file);
    printf("Read %ld bytes\n", n);

    for (int i = 0; i < file->size; i++) {
        printf("%c", buf[i]);
    }

    free(buf);

    if (file_close(file) == -1) {
        perror("file_close");
    }

    if (fat_close(volume) == -1) {
        perror("fat_close");
    }

    if (disk_close(disk) == -1) {
        perror("disk_close");
    }

    return 0;
}
