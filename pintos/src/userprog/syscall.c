#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/init.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "filesys/inode.h"
#include "devices/shutdown.h"
#include "userprog/process.h"
#include "filesys/directory.h"
#include "userprog/pagedir.h"
#include "devices/input.h"


static void syscall_handler (struct intr_frame *);
struct file_info *fd_to_file (int fd);
bool valid_addr (void *addr);
int add_file(struct file *file_, bool isdir);
void valid_ptr(void *ptr, size_t size);



struct lock lox;


struct file_info *fd_to_file (int fd) {
  struct list *list_ = &thread_current()->open_files;
  struct list_elem *e = list_begin (list_);
  for (; e != list_end (list_); e = list_next (e)) {
    struct file_info *f = list_entry (e, struct file_info, elem);
    if (f->fd == fd)
      return f;
  }
  return NULL;
}

void
syscall_init (void)
{
  lock_init(&lox);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}



bool valid_addr (void *addr) {
  return addr && is_user_vaddr (addr + 3) && pagedir_get_page (thread_current ()->pagedir, addr)&&pagedir_get_page (thread_current ()->pagedir, addr+3);
}

void valid_ptr (void *ptr, size_t size) {
  if (!valid_addr (ptr) || !valid_addr (ptr + size)) {
    printf ("%s: exit(%d)\n", thread_current()->name, -1);
    thread_exit ();
  }
}

int add_file(struct file *file_) {
  struct thread *t = thread_current ();
  struct file_info *f = malloc (sizeof (struct file_info));

  f->file = file_;
  f->fd = t->fd++;
  list_push_back (&t->open_files, &f->elem);
  return f->fd;
}


static void
syscall_handler (struct intr_frame *f)
{
  uint32_t* args = ((uint32_t*) f->esp);

  valid_ptr(args, sizeof(uint32_t));

  /*
   * The following print statement, if uncommented, will print out the syscall
   * number whenever a process enters a system call. You might find it useful
   * when debugging. It will cause tests to fail, however, so you should not
   * include it in your final submission.
   */

  /* printf("System call number: %d\n", args[0]); */

  if (args[0] == SYS_EXIT)
    {
      f->eax = args[1];
      thread_current()->exit_code = args[1];
      printf ("%s: exit(%d)\n", thread_current()->name, thread_current()->exit_code);
      thread_exit ();
    }


  if (args[0] == SYS_PRACTICE) {
    args[1]++;
    f->eax = (int)args[1];
  }
  if (args[0] == SYS_HALT) {
    shutdown_power_off();
  }
  if (args[0] == SYS_EXEC) {
    valid_ptr((void *)args[1], sizeof(char *));
    f->eax = process_execute((char *) args[1]);
  }
  if (args[0] == SYS_WAIT) {
    valid_ptr((void *)&args[1], sizeof(tid_t));
    f->eax = process_wait((tid_t) args[1]);
  }
  if (args[0] == SYS_CREATE) {
    lock_acquire(&lox);
    valid_ptr((void *)args[1], sizeof(char *));
    f->eax = filesys_create((char *) args[1], args[2], false);
    lock_release(&lox);
  }
  if (args[0] == SYS_REMOVE) {
    f->eax = filesys_remove ((char *) args[1]);
  }
  if (args[0] == SYS_OPEN) {
    valid_ptr((void *)args[1], sizeof(char *));
    lock_acquire(&lox);
    struct file *phile = filesys_open ((char *) args[1]);
    if (phile) {
      f->eax = add_file(phile, file_isdir(phile));
    } else {
      f->eax = -1;
    }
    lock_release(&lox);
  }
  if (args[0] == SYS_FILESIZE) {
    struct file_info *fi = fd_to_file(args[1]);
    if (fi->dir != NULL) {
      f->eax = file_length(fi->dir);
    } else {
      f->eax = file_length(fi->file);
    }
  }
  if (args[0] == SYS_READ) {
    lock_acquire(&lox);
    valid_ptr((void *)args[2], args[3]);
    struct file_info *fi = fd_to_file(args[1]);
    // if (fi->dir == NULL) {
    //   f->eax = -1;
    // } else {

      if (args[1] == 0) {
        uint8_t *buf = (uint8_t *) args[2];
        uint8_t i = 0;
        while (i < args[3]) {
            buf[i] = input_getc();
            if (buf[i++] == '\n')
              break;
          }
        f->eax = i;
      } else {
        if (fi) {
          f->eax = file_read (fi->file, (uint8_t *) args[2], args[3]);
        } else {
          f->eax = -1;
        }
      // }
  }
    lock_release(&lox);
  }
  if (args[0] == SYS_WRITE) {
    lock_acquire(&lox);
    valid_ptr((void *)args[2], sizeof(uint32_t));
    struct file_info *fi = fd_to_file(args[1]);
    if (args[1] == 1) {
      putbuf ((void *) args[2], args[3]);
      f->eax = args[3];
    } else {
      if (fi && fi->file) {
          f->eax = file_write (fi->file, (void *) args[2], args[3]);
      } else {
        f->eax = -1;
      }
    }

    lock_release(&lox);
  }
  if (args[0] == SYS_SEEK) {
    lock_acquire(&lox);
    struct file_info *fi = fd_to_file(args[1]);
    if(fi){
      file_seek(fi->file, args[2]);
    } else {
      f->eax = -1;
    }
    lock_release(&lox);
  }
  if (args[0] == SYS_TELL) {
    lock_acquire(&lox);
    struct file_info *fi = fd_to_file(args[1]);
    if(fi){
      f->eax = file_tell(fi->file);
    } else {
      f->eax = -1;
    }
    lock_release(&lox);
  }
  if (args[0] == SYS_CLOSE) {
    struct file_info *fi = fd_to_file(args[1]);
    lock_acquire(&lox);
    if (fi != NULL) {
      if (fi->dir) {
        dir_close(fi->dir);
      } else {
        file_close(fi->file);
      }
      list_remove(&fi->elem);
      free(fi);
    }
    lock_release(&lox);
  }
  if (args[0] == SYS_READDIR) {
    valid_ptr((void *)args[2], (NAME_MAX + 1) * sizeof (char));
    struct file_info *fi = fd_to_file(args[1]);
    f->eax = dir_readdir (fi->dir, (char *) args[2]);
  }
  if (args[0] == SYS_MKDIR) {
    f->eax = filesys_create((char *) args[1], 0, true);
  }
  if (args[0] == SYS_CHDIR) {
    f->eax = filesys_chdir((char *) args[1]);
  }
  if (args[0] == SYS_INUMBER) {
    struct file_info *fi = fd_to_file(args[1]);
    if (fi->file != NULL) {
      f->eax = file_inumber (fi->file);
    } else {
      f->eax = inode_get_inumber (fi->dir->inode);
    }
  }
  if (args[0] == SYS_ISDIR) {
    struct file_info *fi = fd_to_file(args[1]);
    f->eax = fi->dir != NULL;   
  }
}
