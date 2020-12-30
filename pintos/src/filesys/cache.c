#include "filesys/cache.h"
#include <string.h>
#include "filesys/filesys.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "devices/timer.h"
#include <threads/synch.h>
#include <stdbool.h>
#include <list.h>

#define CACHE_SIZE 64
int clock_hand = 0;

struct cache_entry
  {
    char data[512];
    struct lock lox;
    bool dirty;
    bool valid;
    struct list_elem elem;
    block_sector_t sector;
  };


static struct cache_entry blocks[CACHE_SIZE];

static struct list clock_list; 
static struct lock clock_lock; 

void cache_init (void) 
{
  list_init (&clock_list);
  lock_init (&clock_lock);
  int i = 0;
  struct cache_entry *block;
  for (; i < CACHE_SIZE; i++) 
  {
    block = &blocks[i];
    lock_init (&block->lox);
    block->valid = 0;
    block->dirty = 0;
  }
}


void cache_read (block_sector_t sector, void *buffer) 
{
  lock_acquire(&clock_lock);
  int size = list_size (&clock_list);
  int i = 0;
  struct cache_entry *block;

  for (; i < size; i++) {
    block = &blocks[i];
    if (block->sector == sector) {
      lock_acquire(&block->lox);
      lock_release(&clock_lock);
      memcpy(buffer, block->data, BLOCK_SECTOR_SIZE);
      // lock_release(&clock_lock);
      lock_release(&block->lox);
      return;
    }
  }

  if (size < CACHE_SIZE) {
    block = &blocks[i];
    block->dirty = 0;
    block->valid = 0;
    block->sector = sector;

    list_push_back(&clock_list, &block->elem);
    lock_acquire (&block->lox);
    lock_release (&clock_lock);
    block_read (fs_device, sector, block->data);
    memcpy (buffer, block->data, BLOCK_SECTOR_SIZE);
    // lock_release (&clock_lock);
    lock_release (&block->lox);
    return; 

  } else {
  	int idx = clock_hand;
  	for (int j = 0; j < CACHE_SIZE; j++) {
  		idx = (idx + j) % CACHE_SIZE;
  		block = &blocks[idx];
  		if (block->valid == 0) {
  			block->valid = 1;
  			break;
  		}
  		block->valid = 0;
  	}
  	clock_hand = idx;
  	lock_acquire (&block->lox);
  	block_sector_t old = block->sector;
  	block->sector = sector;
  	lock_release (&clock_lock);

  	if (block->dirty == 1) {
  		block_write (fs_device, old, block->data);
  		block->dirty = 0;
  	}

    block_read (fs_device, sector, block->data);
    memcpy (buffer, block->data, BLOCK_SECTOR_SIZE);

    // lock_release (&clock_lock);
    lock_release (&block->lox);
    return ;
  }
}

void cache_write (block_sector_t sector, const void *buffer) 
{
  lock_acquire (&clock_lock);
  int size = list_size (&clock_list);
  int i = 0;
  struct cache_entry *block;

  for (; i < size; i++) {
    block = &blocks[i];
    if (block->sector == sector) {
      lock_acquire (&block->lox);
      lock_release (&clock_lock);
      block->dirty = 1;
      memcpy (block->data, buffer, BLOCK_SECTOR_SIZE);
      // lock_release (&clock_lock);
      lock_release (&block->lox);
      return;
    }
  }

  if (size < CACHE_SIZE) {
    block = &blocks[i];
    block->dirty = 1;
    block->sector = sector;
    list_push_back(&clock_list, &block->elem);
    lock_acquire (&block->lox);
    lock_release (&clock_lock);
    memcpy (block->data, buffer, BLOCK_SECTOR_SIZE);
    // lock_release (&clock_lock);
    lock_release (&block->lox);
    return ; 
  }
  else {
  	int idx = clock_hand;
  	for (int j = 0; j < CACHE_SIZE; j++) {
  		idx = (idx + j) % CACHE_SIZE;
  		block = &blocks[idx];
  		if (block->valid == 0) {
  			block->valid = 1;
  			break;
  		}
  		block->valid = 0;
  	}
  	clock_hand = idx;
    lock_acquire (&block->lox);
    block_sector_t old = block->sector;
    block->sector = sector;
    lock_release (&clock_lock);
    if (block->dirty == 1) {
      block_write (fs_device, old, block->data);
    }
    block->dirty = 1;
    memcpy (block->data, buffer, BLOCK_SECTOR_SIZE);
    // lock_release (&clock_lock);
    lock_release (&block->lox);
    return ;
  }
}

void cache_flush (void) {
  lock_acquire (&clock_lock);
  int size = list_size (&clock_list);
  struct cache_entry *block;

  for (int i = 0; i < size; i++) {
    block = &blocks[i];
    if (block->dirty) {
      lock_acquire (&block->lox);
      block_write (fs_device, block->sector, block->data);
      block->dirty = 0;
      lock_release (&block->lox);
    }
  }
  lock_release (&clock_lock);
}





