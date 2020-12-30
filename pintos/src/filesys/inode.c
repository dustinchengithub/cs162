#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include <stdlib.h>
#include "filesys/cache.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

#define DIRECT_SIZE 122

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk {
  block_sector_t direct[DIRECT_SIZE];  
  block_sector_t indirect;              
  block_sector_t doubly_indirect;       
  bool directory;                           
  block_sector_t parent_node;			    
    
  off_t length;                             /* File size in bytes. */
  unsigned magic;                           /* Magic number. */
};


/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    // struct inode_disk data;             /* Inode content. */
  };


block_sector_t
sector_ptr ( const struct inode *inode) {
  return inode->sector;
}

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos)
{
  ASSERT (inode != NULL);

  char buf[BLOCK_SECTOR_SIZE];
  cache_read(inode->sector, &buf);
  struct inode_disk *data = (struct inode_disk*) buf;

  if (pos < data->length && pos >= 0 ) {
  	off_t idx = pos / BLOCK_SECTOR_SIZE;
  	if (idx < DIRECT_SIZE) {
  		return data->direct[idx];

  	} else if (idx < DIRECT_SIZE + 128) {
  		block_sector_t buffer[128] = {0};
  		cache_read(data->indirect, buffer);
  		idx -= DIRECT_SIZE;
  		return buffer[idx];

  	} else if (idx < DIRECT_SIZE + 128 * 128){
  		block_sector_t buffer[128] = {0};
  		idx -= DIRECT_SIZE + 128;
  		cache_read(data->doubly_indirect, buffer);
  		block_sector_t sector = buffer[idx/128];
  		cache_read(sector, buffer);
  		return buffer[idx % 128];
  	} else {
  		return -1;
  	}
  } else {
  	return -1;
  }
}

struct indirect_block {
	block_sector_t block_ptrs[128];
};

bool
indirect_blocker(struct indirect_block *block, off_t start, off_t stop) {
	static char buf[BLOCK_SECTOR_SIZE];
	for (int i = start; i <= stop; i++) {
		if (!free_map_allocate(1, &block->block_ptrs[i])) {
			return false;
		}
		cache_write(block->block_ptrs[i], buf);
	}
	
	return true;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void)
{
  list_init (&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length, bool isdir)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);


  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      size_t sectors = bytes_to_sectors (length);
      disk_inode->directory = isdir;
      disk_inode->magic = INODE_MAGIC;

  	  success = fm_allo(disk_inode) && inode_extend(disk_inode, length);

  	  cache_write(sector, disk_inode);
      free (disk_inode);
    }

  return success;
}

bool
fm_allo(struct inode_disk *data) {
	static char buf[BLOCK_SECTOR_SIZE];

	if (!free_map_allocate(1, &data->indirect)) {
		return false;
	}
	cache_write(data->indirect, buf);

	if (!free_map_allocate(1, &data->doubly_indirect)) {
		return false;
	}
	cache_write(data->doubly_indirect, buf);

	return true;
}


bool
inode_dealloc(struct inode *inode) {
	char buf[BLOCK_SECTOR_SIZE];
    cache_read(inode->sector, &buf);
    struct inode_disk *data = (struct inode_disk*) buf;

	size_t cur = bytes_to_sectors(data->length);
	size_t left = bytes_to_sectors(data->length);

	fm_release(data->direct, DIRECT_SIZE);
	left -= DIRECT_SIZE;
	if (left <= 0) {
		return true;
	}

	if (cur > DIRECT_SIZE + 128) {
		struct indirect_block *inode_indirect = calloc(1, sizeof(struct indirect_block));
		cache_read(data->indirect, inode_indirect);

		fm_release( inode_indirect->block_ptrs, 128);
		left -= 128;
		free(inode_indirect);

		if (left <= 0) {
			fm_release(data->indirect, 128);
			return true;
		}
	}

	if (cur > DIRECT_SIZE + 128 + 128) {
		struct indirect_block *doubly_indirect = calloc(1, sizeof(struct indirect_block));
		cache_read(data->doubly_indirect, doubly_indirect);

		for (off_t i = 0; i < 128; i++) {
			struct indirect_block *inode_indirect = calloc(1, sizeof(struct indirect_block));
			cache_read(doubly_indirect->block_ptrs[i], inode_indirect);

			fm_release(inode_indirect->block_ptrs, 128);
			left -= 128;
			free(inode_indirect);

			if (left <= 0) {
				fm_release(data->doubly_indirect, 128);
				free(doubly_indirect);
				return true;
			}
			fm_release(doubly_indirect->block_ptrs, 128);
		}
	}
	
  return true;
}

bool
inode_extend(struct inode_disk *data, off_t length)
{
	size_t cur = bytes_to_sectors(data->length);
	size_t new = bytes_to_sectors(length);
	
  if (new < cur) {
		return false;
	} else if (new == cur) {
		return true;
	} else if (length > (DIRECT_SIZE + 128 + 128 * DIRECT_SIZE) * BLOCK_SECTOR_SIZE || length < data->length) {
		return false;
	}

	static char buf[BLOCK_SECTOR_SIZE];


	if (cur < DIRECT_SIZE) {
		size_t s = new < DIRECT_SIZE ? new : DIRECT_SIZE;
		for (int i = cur; i < s; i++) {
				if (!free_map_allocate(1, &data->direct[i])) {
					return false;
				}
				cache_write(data->direct[i], buf);
			}
			cur = DIRECT_SIZE;

	}

	if (cur < DIRECT_SIZE + 128 && new > DIRECT_SIZE) {
		struct indirect_block *inode_indirect = calloc(1, sizeof(struct indirect_block));
		cache_read(data->indirect, inode_indirect);
		size_t s = new >= (DIRECT_SIZE + 128) ? 128 : (new - DIRECT_SIZE);

		for (int i = cur - DIRECT_SIZE; i < s; i++) {
				if (!free_map_allocate(1, &inode_indirect->block_ptrs[i])) {
					return false;
				}
				cache_write(inode_indirect->block_ptrs[i], buf);
			}
			cur = DIRECT_SIZE + 128;
		cache_write(data->indirect, inode_indirect);
		free(inode_indirect);
	}

	if (cur < DIRECT_SIZE + 128 + 128 * 128 && new > DIRECT_SIZE + 128) {
		off_t mini = (cur - DIRECT_SIZE + 128)/ 128;
		off_t maxi = ((new - DIRECT_SIZE + 128) / 128) - 1;

		struct indirect_block *doubly_indirect = calloc(1, sizeof(struct indirect_block));
		cache_read(data->doubly_indirect, doubly_indirect);

		if (!indirect_blocker(doubly_indirect, mini, maxi)) {
			return false;
		}

		for (off_t i = mini; i <= maxi; i++) {
			off_t min = 0;
			off_t max = 128;

			if (i == mini) {
				min = (cur - DIRECT_SIZE + 128) % 128;
			}
			if (i == maxi) {
				max = (new - DIRECT_SIZE + 128) % 128 - 1;
			}
			
			struct indirect_block *indirect_block = calloc(1, sizeof(struct indirect_block));
			cache_read(doubly_indirect->block_ptrs[i], indirect_block);
			if (!indirect_blocker(indirect_block, min, max)) {
				return false;
			}
			cache_write(doubly_indirect->block_ptrs[i], indirect_block);
			free(indirect_block);
		}
		cache_write(data->doubly_indirect, doubly_indirect);
		free(doubly_indirect);
	}

	data->length = length;
	return true;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e))
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector)
        {
          inode_reopen (inode);
          return inode;
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  char buf[BLOCK_SECTOR_SIZE];
  cache_read(inode->sector, &buf);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode)
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);

      /* Deallocate blocks if removed. */
      if (inode->removed)
        {
          free_map_release (inode->sector, 1);
		      inode_dealloc(inode);
        }

      free (inode);
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode)
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset)
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;

  while (size > 0)
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Read full sector directly into caller's buffer. */
          //block_read (fs_device, sector_idx, buffer + bytes_read);
          cache_read(sector_idx, buffer + bytes_read);
        }
      else
        {
          /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
          if (bounce == NULL)
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }
          //block_read (fs_device, sector_idx, bounce);
          cache_read(sector_idx, bounce);
          memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  free (bounce);

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset)
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t *bounce = NULL;

  if (inode->deny_write_cnt)
    return 0;

  char buf[BLOCK_SECTOR_SIZE];
  cache_read(inode->sector, &buf);
  struct inode_disk *data = (struct inode_disk*) buf;

  if (byte_to_sector(inode, offset + size - 1) == -1) {
    if (!inode_extend(data, offset + size)) {
      return 0;
    }

    data->length = offset + size;
    cache_write(inode->sector, data);
  }

  while (size > 0)
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Write full sector directly to disk. */
          //block_write(fs_device, sector_idx, buffer + bytes_written);
          cache_write(sector_idx, buffer + bytes_written);
        }
      else
        {
          /* We need a bounce buffer. */
          if (bounce == NULL)
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }

          /* If the sector contains data before or after the chunk
             we're writing, then we need to read in the sector
             first.  Otherwise we start with a sector of all zeros. */
          if (sector_ofs > 0 || chunk_size < sector_left) {
            //block_read (fs_device, sector_idx, bounce);
        	cache_read(sector_idx, bounce);
          } else {
            memset (bounce, 0, BLOCK_SECTOR_SIZE);
          }
          memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
          //block_write (fs_device, sector_idx, bounce);
          cache_write(sector_idx, bounce);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  free (bounce);

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode)
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode)
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  char buf[BLOCK_SECTOR_SIZE];
  cache_read(inode->sector, &buf);
  struct inode_disk *data = (struct inode_disk*) buf;
  return data->length;
}

bool
inode_isdir(struct inode *inode) {
  char buf[BLOCK_SECTOR_SIZE];
  cache_read(inode->sector, &buf);
  struct inode_disk *data = (struct inode_disk*) buf;
  return data->directory;
}

block_sector_t
inode_get_parent(struct inode *inode) {
  char buf[BLOCK_SECTOR_SIZE];
  cache_read(inode->sector, &buf);
  struct inode_disk *data = (struct inode_disk*) buf;
  return data->parent_node;
}

bool
inode_removed(struct inode *inode) {
  return inode->removed;
}