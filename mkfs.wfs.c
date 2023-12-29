// #define FUSE_USE_VERSION 30
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>
#include <stdlib.h>
#include "wfs.h"

int main(int argc, char *argv[]) {

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <disk_path>\n", argv[0]);
        exit(1);
    }

    const char *disk_path = argv[1];
    int fd = open(disk_path, O_RDWR, 0644);
    if (fd == -1) {
        perror("Error opening disk image\n");
        exit(1);
    }

    struct stat sb;

    if(stat(argv[1], &sb) == -1) {
        perror("stat\n");
        exit(1);
    }

    // Map the superblock and enough space for the root directory entry into memory
    char *map = mmap(NULL, sb.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        perror("Error mmapping the file");
        close(fd);
        exit(1);
    }

    struct wfs_sb *mappedSB = (struct wfs_sb *)map;
    mappedSB->magic = WFS_MAGIC;
    mappedSB->head = sizeof(struct wfs_sb);
    
    time_t current_time = time(NULL);

    struct wfs_log_entry *root_entry = (struct wfs_log_entry *)(map + sizeof(struct wfs_sb));
    root_entry->inode.inode_number = 0;
    root_entry->inode.deleted = 0;
    root_entry->inode.mode = S_IFDIR;
    root_entry->inode.uid = getuid();
    root_entry->inode.gid = getgid();
    root_entry->inode.flags = 0;
    root_entry->inode.size = 0;
    root_entry->inode.atime = current_time;
    root_entry->inode.mtime = current_time;
    root_entry->inode.ctime = current_time;
    root_entry->inode.links = 1;

    mappedSB->head = mappedSB->head + sizeof(struct wfs_log_entry);

    if(munmap(map, sb.st_size) == -1) {
        perror("Error unmapping the file\n");
        exit(1);
    }

    close(fd);

    return 0;
}
