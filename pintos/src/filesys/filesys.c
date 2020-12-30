#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "threads/thread.h"
#include "filesys/cache.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);
static int get_next_part (char part[NAME_MAX + 1], const char **srcp);
static bool traverse_path(const char *path, struct dir **ret_dir, char filename[NAME_MAX + 1]);
static char *relative_path(char *path);


/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format)
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  inode_init ();
  free_map_init ();

  if (format)
    do_format ();

  free_map_open ();

  // thread_current()->cwd = dir_open(inode_open(ROOT_DIR_SECTOR));
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void)
{
  cache_flush();
  free_map_close ();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size, bool isdir)
{
  struct dir * dir = NULL;
  struct inode *inode = NULL;
  char filename[NAME_MAX + 1];
  bool rel = true;
  if (name[0] == '/') {
    inode = dir_get_inode(dir_open_root());
    rel =  false;
  } else if (thread_current()->cwd != NULL) {
    inode = dir_get_inode(dir_reopen(thread_current()->cwd));
  }

  if (rel) {
    if (inode_removed(dir_get_inode(thread_current()->cwd))) {
      return false;
    }
  } 

  dir = traverse_bartell(name, filename);

  bool success = (dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size, isdir)
                  && dir_add (dir, filename, inode_sector));
  
  if (!success && inode_sector != 0)
    free_map_release (inode_sector, 1);
  dir_close (dir);

  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
  if (strcmp(name, "") == 0) {
    return NULL;
  }
  char file_name[NAME_MAX + 1];
  struct dir *dir = dir_open_root();
  struct inode *inode = NULL;
  struct dir *cwd = thread_current()->cwd;
  if (strcmp(name, "/") == 0) {
    struct file *rt = file_open(dir_get_inode(dir));
    dir_close(dir);
    return rt;
  } else if (strcmp(name, ".") == 0) {
    if (inode_removed(dir_get_inode(cwd))) {
      return NULL;
    }
    struct file *wd = file_open(dir_get_inode(dir_reopen(cwd)));
    dir_close(dir);
    return wd;
  }
  char *rp = relative_path(name);
  if (dir == NULL || !dir_lookup(dir, rp, &inode)) {
    dir = traverse_path(name, file_name);
    if (dir != NULL) {
      dir_lookup(dir, file_name, &inode);
    } else {
      return NULL;
    }
  }
  dir_close(dir);
  return file_open(inode);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name)
{
  if (strcmp(name, "/") == 0) {
    return false;
  }

  char filename[NAME_MAX + 1];
  struct dir *dir = NULL;
  bool success = false;

  dir = traverse_path(name, filename);
  struct inode *inode = NULL;
  dir_lookup(dir, filename, &inode);
  bool a = inode_isdir(inode);
  bool b = !(inode_get_inumber(inode) == inode_get_inumber(dir_get_inode(thread_current()->cwd)));
  bool c = true;
  if (!(inode == NULL || thread_current()->cwd == NULL)) {
    c = false;
  } else {
    struct inode *cur = inode_open(inode_get_parent(thread_current()->cwd));
    if (inode_get_inumber(cur) == inode_get_inumber(inode)) {
      c = false;
    }
  }
  if (a && b && c) {
    return false;
  }

  success = dir != NULL && dir_remove (dir, filename);
  dir_close (dir);
  return success;
}

bool
filesys_chdir(const char *path)
{
  struct dir *cwd = thread_current()->cwd;
  if (strcmp(path, "..") == 0) {
    dir_get_inode(cwd);
    cwd = dir_open(inode_open(inode_get_parent(dir_get_inode(cwd))));
    dir_close(dir_reopen(cwd));
    return true;
  }
  
  char filename[NAME_MAX + 1];
  struct dir *dir = traverse_path(path, filename);
  struct inode *inode = NULL;

  if (dir_lookup(dir, filename, &inode)) {
    dir_close(cwd);
    cwd = dir_open(inode);
    return true;
  }
  return false;
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16, NULL))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}

// returns directory containing the file in the last part of path (so return the second to last part)
bool
traverse_path(const char* path, char filename[NAME_MAX + 1]) 
{	
if (strcmp(path, "\0") == 0)
    return NULL;

  char *directory;
  char *copy = malloc(strlen(path) + 1);
  strlcpy(copy, path, sizeof(char) * (strlen(path) + 1));
  int i;

  if(copy == NULL || copy[0] == '\0')
    directory =  "/";
  for(i = strlen(copy) - 1; i >= 0 && copy[i] == '/'; i--);
  if(i == -1) {
    directory = copy;
  } else {
    for(i--; i >= 0 && copy[i] != '/'; i--);
    if(i == -1) {
      directory =  ".";
    } else {
      copy[i] = '\0';
      for(i--; i >= 0 && copy[i] == '/'; i--);
      if(i == -1) {
        directory =  "/";
      } else {
        copy[i+1] = '\0';
        directory = copy;
      }
    }
  }

  struct inode *cur = NULL;
  struct inode *next = NULL;

  if (path[0] == '/') {
    cur = dir_get_inode(dir_open_root());
  } else if (thread_current()->cwd != NULL) {
    cur = dir_get_inode(dir_reopen(thread_current()->cwd));
  }

  if (cur == NULL) {
    return NULL;
  }

  if (strcmp(directory, ".") == 0) {
    strlcpy(filename, path, sizeof(char) * (strlen(path) + 1));
  } else {

    int i = 0;
    while (get_next_part(filename, &directory) > 0 && cur != NULL) {
      struct dir *dire = dir_open(cur);
      if (dir_lookup(dire, filename, &next)) {
        dir_close(dire);
        if (next != NULL && inode_isdir(next)) {
          cur = next;
        }
      }
      else if (get_next_part(filename, &directory) != 0) {
        return NULL;
      }
      else {
        break;
      }
      i++;
    }
  }

  char *rp = relative_path(path);
  strlcpy(filename, rp, sizeof(char) * (strlen(rp) + 1));
  return dir_open(cur);
}

/* Extracts a file name part from *SRCP into PART, and updates *SRCP so that the
next call will return the next file name part. Returns 1 if successful, 0 at
end of string, -1 for a too-long file name part. */
static int
get_next_part (char part[NAME_MAX + 1], const char **srcp) {
	const char *src = *srcp;
	char *dst = part;
	/* Skip leading slashes. If it’s all slashes, we’re done. */
	while (*src == "/")
		src++;
	if (*src == "\0")
		return 0;
	/* Copy up to NAME_MAX character from SRC to DST. Add null terminator. */
	while (*src != "/" && *src != "\0") {
		if (dst < part + NAME_MAX)
			*dst++ = *src;
		else
			return -1;
		src++;
	}
	*dst = "\0";
	/* Advance source pointer. */
	*srcp = src;
	return 1;
}

static char *relative_path(char *path)
{
  char *copy = malloc(strlen(path) + 1);
  strlcpy(copy, path, sizeof(char) * (strlen(path) + 1));

  int i;
  if(copy == NULL || copy[0] == '\0') {
    return "";
  } else {
    for(i = strlen(copy) - 1; i >= 0 && copy[i] == '/'; i--);
    if(i == -1) {
      return "/";
    }
  }
  for(copy[i + 1] = '\0'; i >= 0 && copy[i] != '/'; i--);
  char *filename = malloc(strlen(&copy[i + 1]) + 1);
  strlcpy(filename, &copy[i + 1], sizeof(char) * (strlen(&copy[i+1]) + 1));
  return filename;
}

struct dir *
traverse_bartell(const char *path, char filename[NAME_MAX + 1]) {
  if (strcmp(path, "\0") == 0)
    return NULL;

  char *directory;
  char *copy = malloc(strlen(path) + 1);
  strlcpy(copy, path, sizeof(char) * (strlen(path) + 1));
  int i;

  if(copy == NULL || copy[0] == '\0')
    directory =  "/";
  for(i = strlen(copy) - 1; i >= 0 && copy[i] == '/'; i--);
  if(i == -1) {
    directory = copy;
  } else {
    for(i--; i >= 0 && copy[i] != '/'; i--);
    if(i == -1) {
      directory =  ".";
    } else {
      copy[i] = '\0';
      for(i--; i >= 0 && copy[i] == '/'; i--);
      if(i == -1) {
        directory =  "/";
      } else {
        copy[i+1] = '\0';
        directory = copy;
      }
    }
  }

  struct inode *cur = NULL;
  struct inode *next = NULL;

  if (path[0] == '/') {
    cur = dir_get_inode(dir_open_root());
  } else if (thread_current()->cwd != NULL) {
    cur = dir_get_inode(dir_reopen(thread_current()->cwd));
  }
  if (cur == NULL) {
    return NULL;
  }

  if (strcmp(directory, ".") == 0) {
    strlcpy(filename, path, sizeof(char) * (strlen(path) + 1));
  } else {

    int i = 0;
    while (get_next_part(filename, &directory) > 0 && cur != NULL) {
      struct dir *dire = dir_open(cur);
      if (dir_lookup(dire, filename, &next)) {
        dir_close(dire);
        if (next != NULL && inode_isdir(next)) {
          cur = next;
        }
      }
      else if (get_next_part(filename, &directory) != 0) {
        return NULL;
      }
      else {
        break;
      }
      i++;
    }
  }

  char *rp = relative_path(path);
  strlcpy(filename, rp, sizeof(char) * (strlen(rp) + 1));
  return dir_open(cur);
}






