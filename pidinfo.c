#include <linux/cdev.h>   // cdev_init, cdev_add
#include <linux/device.h> // class_create, device_create
#include <linux/fs.h>     // alloc_chrdev_region
#include <linux/init.h>
#include <linux/mm.h> // get_mm_rss — resident memory usage
#include <linux/module.h>
#include <linux/pid.h>          // find_get_pid, pid_task
#include <linux/sched.h>        // task_struct
#include <linux/sched/signal.h> // get_nr_threads
#include <linux/slab.h>         // kzalloc, kfree
#include <linux/uaccess.h>      // copy_from_user, copy_to_user

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tono");
MODULE_DESCRIPTION("PID info character device driver");

// Major number assigned by kernel, used to identify our device
static int major;
// Kernel's internal handle for our character device
static struct cdev pidinfo_cdev;
// Unique class for our driver, used by kernel to group devices
static struct class *pidinfo_class;

// Last PID written to the device — global so it survives across separate opens
// (echo and cat open the file independently, so per-open state won't work)
static pid_t target_pid = -1;

// Per-open state — holds the output buffer for this particular open/read
// session
struct pidinfo_state {
  char outbuf[1024]; // buffer we fill on read, holds the formatted process info
  size_t outlen;     // how many bytes are in outbuf
};

// Translate numeric state to a human readable string
// task->__state is a bitmask defined in linux/sched.h
static const char *state_str(unsigned int state) {
  if (state == 0)
    return "RUNNING";
  if (state & 0x01)
    return "SLEEPING";
  if (state & 0x02)
    return "DISK_SLEEP";
  if (state & 0x04)
    return "STOPPED";
  return "UNKNOWN";
}

// Called when someone opens /dev/pidinfo
// Allocates a fresh pidinfo_state and attaches it to this file descriptor
// so read() can find it later via filp->private_data
static int pidinfo_open(struct inode *inode, struct file *filp) {
  struct pidinfo_state *st;

  // kzalloc = kernel malloc, zeroes the memory, GFP_KERNEL = normal allocation
  st = kzalloc(sizeof(*st), GFP_KERNEL);
  if (!st)
    return -ENOMEM;

  // stash our state on the file descriptor so read can find it
  filp->private_data = st;
  return 0;
}

// Called when they close the file
// Free whatever we allocated in open()
static int pidinfo_release(struct inode *inode, struct file *filp) {
  kfree(filp->private_data);
  return 0;
}

// Called when someone runs echo 1234 > /dev/pidinfo
// ubuf is the string "1234\n" sitting in USER memory — we cannot read it
// directly we must use copy_from_user to safely bring it into kernel memory
// first
static ssize_t pidinfo_write(struct file *filp, const char __user *ubuf,
                             size_t count, loff_t *ppos) {
  char kbuf[16];
  long pid_val;

  if (count >= sizeof(kbuf))
    return -EINVAL;

  // safely copy the string from user memory into our kernel buffer
  if (copy_from_user(kbuf, ubuf, count))
    return -EFAULT;

  kbuf[count] = '\0'; // null terminate so kstrtol can parse it

  // parse "1234\n" into the integer 1234
  if (kstrtol(kbuf, 10, &pid_val))
    return -EINVAL;

  target_pid = (pid_t)pid_val; // save globally so the next read() can find it
  printk(KERN_INFO "pidinfo: target_pid set to %d\n", target_pid);
  return count;
}

// Called when someone does cat /dev/pidinfo
// We look up the PID that was written, dig into its task_struct, format the
// info, and send it back to userspace via copy_to_user
static ssize_t pidinfo_read(struct file *filp, char __user *ubuf, size_t count,
                            loff_t *ppos) {
  struct pidinfo_state *st = filp->private_data;
  struct task_struct *task;
  struct pid *pid_struct;
  unsigned long rss_kb;
  unsigned long utime_ms, stime_ms;
  int threads;

  if (target_pid < 0)
    return -EINVAL; // nothing written yet, nothing to look up

  // only build the output once per open, ppos tracks how far cat has read
  if (*ppos == 0) {
    // rcu_read_lock to avoid kernel from freeing process while read
    rcu_read_lock();

    pid_struct = find_get_pid(target_pid);    // look up the PID
    task = pid_task(pid_struct, PIDTYPE_PID); // get the actual task_struct

    if (!task) {
      rcu_read_unlock();
      st->outlen = scnprintf(st->outbuf, sizeof(st->outbuf),
                             "error: PID %d not found\n", target_pid);
    } else {
      // RSS = resident set size = actual RAM pages this process is using
      // get_mm_rss returns pages, shift converts to kB
      rss_kb = task->mm ? (get_mm_rss(task->mm) << (PAGE_SHIFT - 10)) : 0;

      // utime/stime are in nanoseconds, divide to get milliseconds
      utime_ms = (unsigned long)(task->utime / NSEC_PER_MSEC);
      stime_ms = (unsigned long)(task->stime / NSEC_PER_MSEC);

      // get_nr_threads counts all threads in the process group
      threads = get_nr_threads(task);

      // format everything into our kernel buffer
      st->outlen = scnprintf(st->outbuf, sizeof(st->outbuf),
                             "name       : %s\n"
                             "pid        : %d\n"
                             "parent pid : %d\n"
                             "state      : %s\n"
                             "threads    : %d\n"
                             "memory     : %lu kB\n"
                             "user cpu   : %lu ms\n"
                             "kernel cpu : %lu ms\n",
                             task->comm, // process name e.g. "firefox"
                             target_pid,
                             task->real_parent->pid, // who spawned this process
                             state_str((unsigned int)task->__state), threads,
                             rss_kb,   // RAM usage in kB
                             utime_ms, // time spent in userspace
                             stime_ms  // time spent in kernel
      );
      rcu_read_unlock();
      put_pid(pid_struct); // release the reference we got from find_get_pid
    }
  }

  // ppos is the read cursor. If we're past the end, tell cat we're done
  if (*ppos >= st->outlen)
    return 0;

  size_t to_copy = min(count, st->outlen - (size_t)*ppos);

  // safely copy our kernel buffer back to userspace
  if (copy_to_user(ubuf, st->outbuf + *ppos, to_copy))
    return -EFAULT;

  *ppos += to_copy;
  return to_copy;
}

// Maps open/read/write/release to our functions
// The kernel calls into this table whenever someone touches /dev/pidinfo
static const struct file_operations pidinfo_fops = {
    .owner = THIS_MODULE,
    .open = pidinfo_open,
    .release = pidinfo_release,
    .write = pidinfo_write,
    .read = pidinfo_read,
};

static int __init pidinfo_init(void) {
  dev_t dev;
  // Ask kernel for a major number
  alloc_chrdev_region(&dev, 0, 1, "pidinfo");
  major = MAJOR(dev);
  // Attach our file operations table to the cdev
  cdev_init(&pidinfo_cdev, &pidinfo_fops);
  cdev_add(&pidinfo_cdev, dev, 1);
  // Create the class and make /dev/pidinfo appear
  pidinfo_class = class_create("pidinfo");
  device_create(pidinfo_class, NULL, dev, NULL, "pidinfo");
  printk(KERN_INFO "pidinfo: loaded, major %d\n", major);
  return 0;
}

static void __exit pidinfo_exit(void) {
  // Tear down in reverse order of how we set up
  device_destroy(pidinfo_class, MKDEV(major, 0));
  class_destroy(pidinfo_class);
  cdev_del(&pidinfo_cdev);
  unregister_chrdev_region(MKDEV(major, 0), 1);
  printk(KERN_INFO "pidinfo: bye\n");
}

// Kernel calls these functions when loading / unloading
module_init(pidinfo_init);
module_exit(pidinfo_exit);
