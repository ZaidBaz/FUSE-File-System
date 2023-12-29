#define FUSE_USE_VERSION 30
#include <fuse.h>
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

char *map;
int currNum;

struct wfs_log_entry *searchLastInodeMatch(unsigned long inode_num) {

    struct wfs_log_entry* latest_match = NULL;

    struct wfs_log_entry *curr = (struct wfs_log_entry*)( (char*) map + sizeof(struct wfs_sb));

    while(curr < (struct wfs_log_entry *) ((char*)map + ((struct wfs_sb *)map)->head)) {

        if(curr->inode.deleted == 0 && curr->inode.inode_number == inode_num) {

            latest_match = curr;
        
        }

        curr = (struct wfs_log_entry*) ((char *)curr + sizeof(struct wfs_inode) + curr->inode.size);
    }

    return latest_match;

}

struct wfs_log_entry *find_dirfile_lentry(char *fullpath) {

    struct wfs_log_entry *curr = searchLastInodeMatch(0);
    const char delim[1] = "/";

    char *token = strtok(fullpath, delim);
    int found = 0;

    if(token == NULL) {
        return curr;
    }

    while(token != NULL) {

        found = 0;
        struct wfs_dentry *currDEntry = (struct wfs_dentry *) (curr->data);
        int inDirectory = curr->inode.size / sizeof(struct wfs_dentry);

        for(int i = 0; i < inDirectory; i++) {
            
            if(strcmp(token, currDEntry->name) == 0) {

                curr = searchLastInodeMatch(currDEntry->inode_number);
                found = 1;

            }

            currDEntry++;

        }
        token = strtok(NULL, delim);

    }

    if(found != 1) {
        return NULL;
    }
    return curr;

}

static int wfs_getattr(const char *path, struct stat *stbuf) {

    char *fullpath;
    
    fullpath = strdup(path);

    struct wfs_log_entry *log_entr = find_dirfile_lentry(fullpath);

    if(log_entr == (struct wfs_log_entry*) NULL){

        free(fullpath);
        return -ENOENT;

    }

    memset(stbuf, 0, sizeof(*stbuf));
    stbuf->st_uid = log_entr->inode.uid;
    stbuf->st_gid = log_entr->inode.gid;
    stbuf->st_mtime = log_entr->inode.mtime;
    stbuf->st_atime = log_entr->inode.atime;
    stbuf->st_ctime = log_entr->inode.ctime;
    stbuf->st_mode = log_entr->inode.mode;
    stbuf->st_nlink = log_entr->inode.links;
    stbuf->st_size = log_entr->inode.size;

    free(fullpath);
    return 0;

}

static int wfs_mknod(const char* path, mode_t mode, dev_t rdev) {

    char *fullpath;

    fullpath = strdup(path);

    char *beforeLastSlash;
    beforeLastSlash = malloc(sizeof(char) * 256);
    const char *lastSlash = strrchr(fullpath, '/');
    
    if (lastSlash != NULL) {

        if(lastSlash == fullpath) {
            strcpy(beforeLastSlash, "/");
        }
        else {
            ptrdiff_t length = lastSlash - fullpath;
            strncpy(beforeLastSlash, fullpath, length);
            beforeLastSlash[length] = '\0';
        }

    }

    struct wfs_log_entry *log_entr = find_dirfile_lentry(beforeLastSlash);

    if(log_entr == (struct wfs_log_entry *) NULL) {

        free(fullpath);
        return -ENOENT;

    }

    size_t newSizeOfNewEntry = sizeof(struct wfs_log_entry) + sizeof(struct wfs_dentry) + log_entr->inode.size;

    struct wfs_log_entry *newEntry = (struct wfs_log_entry *) ((char *)map + ((struct wfs_sb *) map)->head);
    memcpy(newEntry, &log_entr->inode, sizeof(struct wfs_inode));
    memcpy(newEntry->data, log_entr->data, log_entr->inode.size);

    struct wfs_dentry *addedDentry = (struct wfs_dentry *)((char *) newEntry->data + newEntry->inode.size);
    addedDentry->inode_number = currNum;
    strncpy(addedDentry->name, lastSlash + 1, (int) strlen(path) - ((char *) lastSlash - (char *) fullpath) - 1);
     
    newEntry->inode.atime = time(NULL);
    newEntry->inode.mtime = time(NULL);
    newEntry->inode.size += sizeof(struct wfs_dentry);

    ((struct wfs_sb *)map)->head += newSizeOfNewEntry;

    struct wfs_log_entry *newFileDirLogEntry = (struct wfs_log_entry *) ((char *)map + ((struct wfs_sb *)map)->head);

    newFileDirLogEntry->inode.inode_number = currNum;
    newFileDirLogEntry->inode.uid = getuid();
    newFileDirLogEntry->inode.gid = getgid();
    newFileDirLogEntry->inode.deleted = 0;
    if(S_ISDIR(mode)) { 
        newFileDirLogEntry->inode.mode = S_IFDIR | mode;
    }
    else if(S_ISREG(mode)) {
        newFileDirLogEntry->inode.mode = S_IFREG | mode;
    }
    newFileDirLogEntry->inode.flags = 0;
    newFileDirLogEntry->inode.size = 0;
    newFileDirLogEntry->inode.atime = time(NULL);
    newFileDirLogEntry->inode.mtime = time(NULL);
    newFileDirLogEntry->inode.ctime = time(NULL);
    newFileDirLogEntry->inode.links = 1;

    ((struct wfs_sb *)map)->head += sizeof(struct wfs_inode);

    free(fullpath);
    currNum++;
    return 0;

}

static int wfs_mkdir(const char* path, mode_t mode) {

    int result = wfs_mknod(path, S_IFDIR | mode, 0);

    if(result != 0) {
        return -ENOENT;
    }

    return 0;
}

static int wfs_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* fi) {

    char *fullpath;
    
    fullpath = strdup(path);

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    struct wfs_log_entry *log_entr = find_dirfile_lentry(fullpath);
    struct wfs_dentry *currDentry = (void *)log_entr->data;

    size_t totalEntries = log_entr->inode.size / sizeof(struct wfs_dentry);
    for(size_t i = 0; i < totalEntries; i++) {
        filler(buf, (currDentry+i)->name, NULL, 0);
    }

    free(fullpath);
    return 0;

}

static int wfs_read(const char* path, char *buf, size_t size, off_t offset, struct fuse_file_info* fi) {

    char *fullpath;
    
    fullpath = strdup(path);

    struct wfs_log_entry *log_entr = find_dirfile_lentry(fullpath);

    size_t updated_size;

    if(log_entr->inode.size > size){
        
        updated_size = log_entr->inode.size;
    }
    else{

        updated_size = size;
    
    }

    memcpy(buf, log_entr->data + offset, updated_size);

    free(fullpath);
    return updated_size;
}

static int wfs_write(const char* path, const char *buf, size_t size, off_t offset, struct fuse_file_info* fi) {
    
    char *fullpath;
    
    fullpath = strdup(path);

    struct wfs_log_entry *log_entr = find_dirfile_lentry(fullpath);

    size_t updated_size;

    if(log_entr->inode.size > size){
        
        updated_size = log_entr->inode.size;
    }
    else{

        updated_size = size;
    
    }

    memcpy(log_entr->data + offset, buf, updated_size);

    log_entr->inode.size = updated_size;
    log_entr->inode.mtime = time(NULL);
    log_entr->inode.atime = time(NULL);

    free(fullpath);
    return updated_size;
}

void deleteAll(unsigned long inode_num) {

    struct wfs_log_entry *curr = (struct wfs_log_entry*)( (char*) map + sizeof(struct wfs_sb));

    while(curr < (struct wfs_log_entry *) ((char*)map + ((struct wfs_sb *)map)->head)) {

        // CHECK IF DELETED OR NOT
        if(curr->inode.deleted == 0 && curr->inode.inode_number == inode_num) {

            curr->inode.deleted = 1;
        
        }

        curr = (struct wfs_log_entry*) ((char *)curr + sizeof(struct wfs_inode) + curr->inode.size);
    }

    return;

}

void keepExcept(struct wfs_log_entry* subDir, unsigned long inodeToDel) {

    struct wfs_log_entry *newLogEntry = (struct wfs_log_entry *) ((char *) map + ((struct wfs_sb *)map)->head);

    memcpy(newLogEntry, &subDir->inode, sizeof(struct wfs_inode));

    int inDirectory = subDir->inode.size / sizeof(struct wfs_dentry);

    struct wfs_dentry *currDEntry = (struct wfs_dentry *) (subDir->data);

    struct wfs_dentry *currData = (struct wfs_dentry *) ((char *)newLogEntry + sizeof(struct wfs_inode));

    for(int i = 0; i < inDirectory; i++) {

        if(currDEntry->inode_number != inodeToDel) {

            currData->inode_number = currDEntry->inode_number;
            strcpy(currData->name, currDEntry->name);
            currData++;

        }

        currDEntry++;

    }

    newLogEntry->inode.size -= sizeof(struct wfs_dentry);

    return;

}

static int wfs_unlink(const char* path) {

    char *fullpath;

    fullpath = strdup(path);

    char *beforeLastSlash;
    beforeLastSlash = malloc(sizeof(char) * 256);
    const char *lastSlash = strrchr(fullpath, '/');
    
    if (lastSlash != NULL) {

        if(lastSlash == fullpath) {
            strcpy(beforeLastSlash, "/");
        }
        else {
            ptrdiff_t length = lastSlash - fullpath;
            strncpy(beforeLastSlash, fullpath, length);
            beforeLastSlash[length] = '\0';
        }

    }

    struct wfs_log_entry *subDirOfDelete  = find_dirfile_lentry(beforeLastSlash);
    struct wfs_log_entry *toDelete = find_dirfile_lentry(fullpath);

    if(subDirOfDelete == (struct wfs_log_entry *) NULL || toDelete == (struct wfs_log_entry *) NULL) {

        free(fullpath);
        return -ENOENT;

    }

    deleteAll(toDelete->inode.inode_number);

    keepExcept(subDirOfDelete, toDelete->inode.inode_number);

    free(fullpath);
    return 0;
}

static struct fuse_operations ops = {

    .getattr	= wfs_getattr,
    .mknod      = wfs_mknod,
    .mkdir      = wfs_mkdir,
    .read	    = wfs_read,
    .write      = wfs_write,
    .readdir	= wfs_readdir,
    .unlink    	= wfs_unlink,

};

int main(int argc, char *argv[]) {

    struct stat sb;

    if(stat(argv[argc-2], &sb) == -1) {
        perror("stat\n");
        exit(1);
    }

    int fd = open(argv[argc-2], O_RDWR);

    map = mmap(0, sb.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if(map < (char*) 0) {
        printf("map error\n");
    }

    argv[argc-2] = argv[argc-1];
    argv[argc-1] = '\0';

    currNum = 1;

    int returnedFuse = fuse_main(--argc, argv, &ops, NULL);

    if(munmap(map, sb.st_size) == -1) {
        perror("Error unmapping the file\n");
        exit(1);
    }
    close(fd);

    return returnedFuse;
}
