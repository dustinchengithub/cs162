#ifndef FILESYS_INODE_H
#define FILESYS_INODE_H

#include <stdbool.h>
#include "filesys/off_t.h"
#include "devices/block.h"

struct bitmap;
struct inode_disk;
struct indirect_block;

void inode_init (void);
bool inode_create (block_sector_t, off_t, bool);
struct inode *inode_open (block_sector_t);
struct inode *inode_reopen (struct inode *);
block_sector_t inode_get_inumber (const struct inode *);
void inode_close (struct inode *);
void inode_remove (struct inode *);
off_t inode_read_at (struct inode *, void *, off_t size, off_t offset);
off_t inode_write_at (struct inode *, const void *, off_t size, off_t offset);
void inode_deny_write (struct inode *);
void inode_allow_write (struct inode *);
off_t inode_length (const struct inode *);
bool inode_isdir (const struct inode *);
bool indirect_blocker(struct indirect_block *block, off_t start, off_t stop);
bool fm_allo(struct inode_disk *data);
bool inode_dealloc(struct inode *inode);
bool inode_extend(struct inode_disk *data, off_t length);
block_sector_t sector_ptr (const struct inode *);
bool inode_removed(struct inode *inode);
block_sector_t inode_get_parent(struct inode *inode);
#endif /* filesys/inode.h */