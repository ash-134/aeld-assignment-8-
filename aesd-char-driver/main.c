/**
 * @file aesdchar.c
 * @brief Functions and data related to the aesd char driver implementation
 *
 * @author Dan Walkes
 * @date 2020-03-01
 * @copyright Copyright (c) 2020
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include "aesd-circular-buffer.h"
#include "aesdchar.h"

int aesd_major = 0;
int aesd_minor = 0;

MODULE_AUTHOR("Justin Wu");
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    struct aesd_dev *dev;
    PDEBUG("open");
    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    filp->private_data = dev;
    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry = NULL;
    size_t entry_offset_byte = 0;
    size_t bytes_to_copy = 0;
    ssize_t retval = 0;

    PDEBUG("read %zu bytes with offset %lld", count, *f_pos);

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->circular_buffer, *f_pos, &entry_offset_byte);
    if (!entry) {
        goto out;
    }

    bytes_to_copy = entry->size - entry_offset_byte;
    if (bytes_to_copy > count) {
        bytes_to_copy = count;
    }

    if (copy_to_user(buf, entry->buffptr + entry_offset_byte, bytes_to_copy)) {
        retval = -EFAULT;
        goto out;
    }

    *f_pos += bytes_to_copy;
    retval = bytes_to_copy;

out:
    mutex_unlock(&dev->lock);
    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    ssize_t retval = -ENOMEM;
    size_t i;
    bool found_newline = false;

    PDEBUG("write %zu bytes", count);

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    char *kbuf = kmalloc(count, GFP_KERNEL);
    if (!kbuf) {
        goto out;
    }

    if (copy_from_user(kbuf, buf, count)) {
        retval = -EFAULT;
        kfree(kbuf);
        goto out;
    }

    for (i = 0; i < count; i++) {
        if (kbuf[i] == '\n') {
            found_newline = true;
            break;
        }
    }

    char *new_temp = krealloc(dev->temp_write_buf, dev->temp_write_size + count, GFP_KERNEL);
    if (!new_temp) {
        kfree(kbuf);
        retval = -ENOMEM;
        goto out;
    }
    dev->temp_write_buf = new_temp;
    memcpy(dev->temp_write_buf + dev->temp_write_size, kbuf, count);
    dev->temp_write_size += count;
    kfree(kbuf);

    if (found_newline) {
        struct aesd_buffer_entry new_entry;
        new_entry.buffptr = dev->temp_write_buf;
        new_entry.size = dev->temp_write_size;

        const char *overwritten_ptr = NULL;
        if (dev->circular_buffer.full) {
            overwritten_ptr = dev->circular_buffer.entry[dev->circular_buffer.in_offs].buffptr;
        }

        aesd_circular_buffer_add_entry(&dev->circular_buffer, &new_entry);

        if (overwritten_ptr) {
            kfree(overwritten_ptr);
        }

        dev->temp_write_buf = NULL;
        dev->temp_write_size = 0;
    }

    retval = count;

out:
    mutex_unlock(&dev->lock);
    return retval;
}

struct file_operations aesd_fops = {
    .owner   = THIS_MODULE,
    .open    = aesd_open,
    .release = aesd_release,
    .read    = aesd_read,
    .write   = aesd_write,
};

static int __init aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1, "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    cdev_init(&aesd_device.cdev, &aesd_fops);
    aesd_device.cdev.owner = THIS_MODULE;
    aesd_device.cdev.ops = &aesd_fops;
    result = cdev_add(&aesd_device.cdev, dev, 1);
    if (result) {
        printk(KERN_NOTICE "Error %d adding aesd cdev\n", result);
        unregister_chrdev_region(dev, 1);
        return result;
    }
    mutex_init(&aesd_device.lock);
    aesd_circular_buffer_init(&aesd_device.circular_buffer);
    aesd_device.temp_write_buf = NULL;
    aesd_device.temp_write_size = 0;
    return 0;
}

static void __exit aesd_cleanup_module(void)
{
    dev_t dev_no = MKDEV(aesd_major, aesd_minor);
    cdev_del(&aesd_device.cdev);
    unregister_chrdev_region(dev_no, 1);

    uint8_t index;
    struct aesd_buffer_entry *entry;
    AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.circular_buffer, index) {
        if (entry->buffptr) {
            kfree(entry->buffptr);
            entry->buffptr = NULL;
        }
    }
    if (aesd_device.temp_write_buf) {
        kfree(aesd_device.temp_write_buf);
        aesd_device.temp_write_buf = NULL;
    }
    mutex_destroy(&aesd_device.lock);
}

module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
