/**
 * fs.c  -  file system implementation
 *  FSO 24/25
 *  FCT UNL
 *
 *  students:
 *  68739 Oleksandra Kozlova
 *  67258 Ilia Taitsel 
 *
 **/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include "bitmap.h"

#include "fs.h"
#include "disk.h"

/*******
 * FSO FS layout
 * FS block size = disk block size (2K)
 * block#
 * 0            super block (includes the number of inodes)
 * 1 ...        bitmap with free/used blocks begins
 * after bitmap follows blocks with inodes (root dir is inode 0),
 *              assuming on average that each file uses 10 blocks, we need
 *              1 inode per 10 blocks (10%) to fill the disk with files
 * after inodes follows the data blocks
 */

#define BLOCKSZ		(DISK_BLOCK_SIZE)
#define SBLOCK		0	// superblock is in disk block 0
#define BITMAPSTART 1	// free/use block bitmap starts in block 1
#define INODESTART  (rootSB.first_inodeblk)  // inodes start in this block
#define ROOTINO		0 	// root dir is described in inode 0


#define FS_MAGIC    (0xf50f5024) // when formated the SB starts with this number
#define DIRBLOCK_PER_INODE 11	 // direct block's index per inode
#define MAXFILENAME   62         // max name size in a dirent

#define INODESZ		((int)sizeof(struct fs_inode))
#define INODES_PER_BLOCK		(BLOCKSZ/INODESZ)
#define DIRENTS_PER_BLOCK		(BLOCKSZ/sizeof(struct fs_dirent))

#define IFDIR	4	// inode is dir
#define IFREG	8	// inode is regular file


#define FREE 0
#define NOT_FREE 1

#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))

/*** FSO FileSystem in memory structures ***/


// Super block with file system parameters
struct fs_sblock {
    uint32_t magic;     // when formated this field should have FS_MAGIC
    uint32_t block_cnt; // number of blocks in disk
    uint16_t bmap_size; // number of blocks with free/use block bitmap
    uint16_t first_inodeblk; // first block with inodes
    uint16_t inode_cnt;      // number of inodes
    uint16_t inode_blocks;   // number of blocks with inodes
    uint16_t first_datablk;  // first block with data or dir
};

// inode describing a file or directory
struct fs_inode {
    uint16_t type;   // node type (FREE, IFDIR, IFREG, etc)
    uint16_t nlinks; // number of links to this inode (not used)
    uint32_t size;   // file size (bytes)
    uint16_t dir_block[DIRBLOCK_PER_INODE]; // direct data blocks
    uint16_t indir_block; // indirect index block
};

// directory entry
struct fs_dirent {
    uint16_t d_ino;           // inode number
    char d_name[MAXFILENAME]; // name (C string)
};

// generic block: a variable of this type may be used as a
// superblock, a block of inodes, a block of dirents or data (byte array)
union fs_block {
    struct fs_sblock super;
    struct fs_inode inode[INODES_PER_BLOCK];
    struct fs_dirent dirent[DIRENTS_PER_BLOCK];
    char data[BLOCKSZ];
};

/**  Super block from mounted File System (Global Variable)**/
struct fs_sblock rootSB;


/*****************************************************/

/** check that the global rootSB contains a valid super block of a formated disk
 *  returns -1 if error; 0 if is OK
 */
int check_rootSB() {
    if (rootSB.magic != FS_MAGIC) {
        printf("Unformatted disk!\n");
        return -1;
    }
    return 0;
}

/** finds the disk block number that contains the byte at the given file offset
 *  for the file or directory described by the given inode;
 *  returns the block number
 */
int offset2block(struct fs_inode *inode, int offset) {
    int block = offset / BLOCKSZ;

    if (block < DIRBLOCK_PER_INODE) { // just for direct blocks
        return inode->dir_block[block];
    } else if (block < DIRBLOCK_PER_INODE + BLOCKSZ / 2) {
        // first indirect block index
        uint16_t data[BLOCKSZ / 2];

        disk_read(inode->indir_block, (char*)data);
        printf("returning block %d, indirect %d, %d with content %d\n", block, inode->indir_block,
               block - DIRBLOCK_PER_INODE, data[block - DIRBLOCK_PER_INODE]);
        return data[block - DIRBLOCK_PER_INODE];
    } else {
        printf("offset to big!\n");
        return -1;
    }
}



/** load from disk the inode ino_number into ino (must be an initialized pointer);
 *  returns -1 ino_number outside the existing limits;
 *  returns 0 if inode read. The ino.type == FREE if ino_number is of a free inode
 */
int inode_load(int ino_number, struct fs_inode *ino) {
    union fs_block block;

    if ((unsigned)ino_number > rootSB.inode_cnt * INODES_PER_BLOCK) {
        printf("inode number too big \n");
        ino->type = FREE;
        return -1;
    }
    int inodeBlock = rootSB.first_inodeblk + (ino_number / INODES_PER_BLOCK);
    disk_read(inodeBlock, block.data);
    *ino = block.inode[ino_number % INODES_PER_BLOCK];
    return 0;
}

/** save to disk the inode ino to the ino_number position;
 *  if ino_number is outside limits, nothing is done and returns -1
 *  returns 0 if saved
 */
int inode_save(int ino_number, struct fs_inode *ino) {
    union fs_block block;

    if ((unsigned)ino_number > rootSB.inode_cnt * INODES_PER_BLOCK) {
        printf("inode number too big \n");
        return -1;
    }
    int inodeBlock = rootSB.first_inodeblk + (ino_number / INODES_PER_BLOCK);
    disk_read(inodeBlock, block.data); // read full block
    block.inode[ino_number % INODES_PER_BLOCK] = *ino; // update inode
    disk_write(inodeBlock, block.data); // write block
    return 0;
}


/*****************************************************/

/** dump Super block (usually block 0) from disk to stdout for debugging
 */
void dumpSB(int numb) {
    union fs_block block;

    disk_read(numb, block.data);
    printf("Disk superblock %d:\n", numb);
    printf("    magic = %x\n", block.super.magic);
    printf("    disk size %d blocks\n", block.super.block_cnt);
    printf("    bmap_size: %d\n", block.super.bmap_size);
    printf("    first inode block: %d\n", block.super.first_inodeblk);
    printf("    inode_blocks: %d (%d inodes)\n", block.super.inode_blocks,
           block.super.inode_cnt);
    printf("    first data block: %d\n", block.super.first_datablk);
    printf("    data blocks: %d\n", block.super.block_cnt - block.super.first_datablk );
}


/** prints information details about file system for debugging
 */
void fs_debug() {
    union fs_block block;

    dumpSB(SBLOCK);
    if ( check_rootSB() == -1) return;

    disk_read(SBLOCK, block.data);
    rootSB = block.super;
    printf("**************************************\n");
    printf("blocks in use - bitmap:\n");
    int nblocks = rootSB.block_cnt;
    for (int i = 0; i < rootSB.bmap_size; i++) {
        disk_read(BITMAPSTART + i, block.data);
        bitmap_print(block.data, MIN(BLOCKSZ*8, nblocks));
        nblocks -= BLOCKSZ * 8;
    }
    printf("**************************************\n");
    printf("inodes in use:\n");
    for (int i = 0; i < rootSB.inode_blocks; i++) {
        disk_read(INODESTART + i, block.data);
        for (int j = 0; j < INODES_PER_BLOCK; j++)
            if (block.inode[j].type != FREE)
                printf(" %d: type=%d;", j + i * INODES_PER_BLOCK, block.inode[j].type);
    }
    printf("\n**************************************\n");
}


/** mount root FS;
 *  open device image or create it;
 *  loads superblock from device into global variable rootSB;
 *  returns -1 if error
 */
int fs_mount(char *device, int size) {
    union fs_block block;

    if (rootSB.magic == FS_MAGIC) {
        printf("A disc is already mounted!\n");
        return -1;
    }
    if (disk_init(device, size)<0) return -1; // open disk image or create if it does not exist
    disk_read(SBLOCK, block.data);
    if (block.super.magic != FS_MAGIC) {
        printf("Unformatted disc! Not mounted.\n");
        return 0;
    }
    rootSB = block.super;
    return 0;
}


/*****************************************************/

/** list the directory dirname
 */
int fs_ls(char *dirname) {
    if ( check_rootSB() == -1) return -1;

    // recommended formats
    //  printf("listing dir %s (inode %d):\n", dirname, ino_number);
    //  printf("ino:type bytes name\n");
    //  printf("%3d:%c%9d %s\n", ... );


    return -1;
}


/** open file name;
 *  returns the inode number for named file (there are no file descriptors)
 */
int fs_open(char *name, int openmode) {
    if (check_rootSB() == -1) return -1;


    return -1;  // no space for more open files
}


/** close file descriptor;
 *  returns 0 or -1 if fd is not a valid file descriptor
 */
int fs_close(int fd) {

    return -1;
}


/************************************************************/
int fs_read(int fd, char *data, int length) {
    if (check_rootSB() == -1) return -1;
    int bytes_read = 0;


    return bytes_read;
}
