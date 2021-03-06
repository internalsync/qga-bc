/* Copyright 2012 Red Hat, Inc.
 * Copyright IBM, Corp. 2012
 *
 * Based on Linux 2.6.39 vhost code:
 * Copyright (C) 2009 Red Hat, Inc.
 * Copyright (C) 2006 Rusty Russell IBM Corporation
 *
 * Author: Michael S. Tsirkin <mst@redhat.com>
 *         Stefan Hajnoczi <stefanha@redhat.com>
 *
 * Inspiration, some code, and most witty comments come from
 * Documentation/virtual/lguest/lguest.c, by Rusty Russell
 *
 * This work is licensed under the terms of the GNU GPL, version 2.
 */

#include "trace.h"
#include "hw/dataplane/vring.h"

/* Compiler barrier */
#define barrier()   asm volatile("" ::: "memory")

/* Memory barrier */
#define mb()    __asm__ __volatile__("mfence":::"memory")

/* Map the guest's vring to host memory */
bool vring_setup(Vring *vring, VirtIODevice *vdev, int n)
{
    target_phys_addr_t vring_addr = virtio_queue_get_ring_addr(vdev, n);
    target_phys_addr_t vring_size = virtio_queue_get_ring_size(vdev, n);
    void *vring_ptr;

    vring->broken = false;

    hostmem_init(&vring->hostmem);
    vring_ptr = hostmem_lookup(&vring->hostmem, vring_addr, vring_size, true);
    if (!vring_ptr) {
        error_report("Failed to map vring "
                     "addr " TARGET_FMT_lx " size " TARGET_FMT_lu,
                     vring_addr, vring_size);
        vring->broken = true;
        return false;
    }

    vring_init(&vring->vr, virtio_queue_get_num(vdev, n), vring_ptr, 4096);

    vring->last_avail_idx = 0;
    vring->last_used_idx = 0;
    vring->signalled_used = 0;
    vring->signalled_used_valid = false;

    trace_vring_setup(virtio_queue_get_ring_addr(vdev, n),
                      vring->vr.desc, vring->vr.avail, vring->vr.used);
    return true;
}

void vring_teardown(Vring *vring)
{
    hostmem_finalize(&vring->hostmem);
}

/* Disable guest->host notifies */
void vring_disable_notification(VirtIODevice *vdev, Vring *vring)
{
    if (!(vdev->guest_features & (1 << VIRTIO_RING_F_EVENT_IDX))) {
        vring->vr.used->flags |= VRING_USED_F_NO_NOTIFY;
    }
}

/* Enable guest->host notifies
 *
 * Return true if the vring is empty, false if there are more requests.
 */
bool vring_enable_notification(VirtIODevice *vdev, Vring *vring)
{
    if (vdev->guest_features & (1 << VIRTIO_RING_F_EVENT_IDX)) {
        vring_avail_event(&vring->vr) = vring->vr.avail->idx;
    } else {
        vring->vr.used->flags &= ~VRING_USED_F_NO_NOTIFY;
    }
    mb(); /* ensure update is seen before reading avail_idx */
    return !vring_more_avail(vring);
}

/* This is stolen from linux/drivers/vhost/vhost.c:vhost_notify() */
bool vring_should_notify(VirtIODevice *vdev, Vring *vring)
{
    uint16_t old, new;
    bool v;
    /* Flush out used index updates. This is paired
     * with the barrier that the Guest executes when enabling
     * interrupts. */
    mb();

    if ((vdev->guest_features & VIRTIO_F_NOTIFY_ON_EMPTY) &&
        unlikely(vring->vr.avail->idx == vring->last_avail_idx)) {
        return true;
    }

    if (!(vdev->guest_features & VIRTIO_RING_F_EVENT_IDX)) {
        return !(vring->vr.avail->flags & VRING_AVAIL_F_NO_INTERRUPT);
    }
    old = vring->signalled_used;
    v = vring->signalled_used_valid;
    new = vring->signalled_used = vring->last_used_idx;
    vring->signalled_used_valid = true;

    if (unlikely(!v)) {
        return true;
    }

    return vring_need_event(vring_used_event(&vring->vr), new, old);
}

/* This is stolen from linux/drivers/vhost/vhost.c. */
static int get_indirect(Vring *vring,
                        struct iovec iov[], struct iovec *iov_end,
                        unsigned int *out_num, unsigned int *in_num,
                        struct vring_desc *indirect)
{
    struct vring_desc desc;
    unsigned int i = 0, count, found = 0;

    /* Sanity check */
    if (unlikely(indirect->len % sizeof(desc))) {
        error_report("Invalid length in indirect descriptor: "
                     "len %#x not multiple of %#zx",
                     indirect->len, sizeof(desc));
        vring->broken = true;
        return -EFAULT;
    }

    count = indirect->len / sizeof(desc);
    /* Buffers are chained via a 16 bit next field, so
     * we can have at most 2^16 of these. */
    if (unlikely(count > USHRT_MAX + 1)) {
        error_report("Indirect buffer length too big: %d", indirect->len);
        vring->broken = true;
        return -EFAULT;
    }

    do {
        struct vring_desc *desc_ptr;

        /* Translate indirect descriptor */
        desc_ptr = hostmem_lookup(&vring->hostmem,
                                  indirect->addr + found * sizeof(desc),
                                  sizeof(desc), false);
        if (!desc_ptr) {
            error_report("Failed to map indirect descriptor "
                         "addr %#" PRIx64 " len %zu",
                         (uint64_t)indirect->addr + found * sizeof(desc),
                         sizeof(desc));
            vring->broken = true;
            return -EFAULT;
        }
        desc = *desc_ptr;

        /* Ensure descriptor has been loaded before accessing fields */
        barrier(); /* read_barrier_depends(); */

        if (unlikely(++found > count)) {
            error_report("Loop detected: last one at %u "
                         "indirect size %u", i, count);
            vring->broken = true;
            return -EFAULT;
        }

        if (unlikely(desc.flags & VRING_DESC_F_INDIRECT)) {
            error_report("Nested indirect descriptor");
            vring->broken = true;
            return -EFAULT;
        }

        /* Stop for now if there are not enough iovecs available. */
        if (iov >= iov_end) {
            return -ENOBUFS;
        }

        iov->iov_base = hostmem_lookup(&vring->hostmem, desc.addr, desc.len,
                                       desc.flags & VRING_DESC_F_WRITE);
        if (!iov->iov_base) {
            error_report("Failed to map indirect descriptor"
                         "addr %#" PRIx64 " len %u",
                         (uint64_t)desc.addr, desc.len);
            vring->broken = true;
            return -EFAULT;
        }
        iov->iov_len = desc.len;
        iov++;

        /* If this is an input descriptor, increment that count. */
        if (desc.flags & VRING_DESC_F_WRITE) {
            *in_num += 1;
        } else {
            /* If it's an output descriptor, they're all supposed
             * to come before any input descriptors. */
            if (unlikely(*in_num)) {
                error_report("Indirect descriptor "
                             "has out after in: idx %u", i);
                vring->broken = true;
                return -EFAULT;
            }
            *out_num += 1;
        }
        i = desc.next;
    } while (desc.flags & VRING_DESC_F_NEXT);
    return 0;
}

/* This looks in the virtqueue and for the first available buffer, and converts
 * it to an iovec for convenient access.  Since descriptors consist of some
 * number of output then some number of input descriptors, it's actually two
 * iovecs, but we pack them into one and note how many of each there were.
 *
 * This function returns the descriptor number found, or vq->num (which is
 * never a valid descriptor number) if none was found.  A negative code is
 * returned on error.
 *
 * Stolen from linux/drivers/vhost/vhost.c.
 */
int vring_pop(VirtIODevice *vdev, Vring *vring,
              struct iovec iov[], struct iovec *iov_end,
              unsigned int *out_num, unsigned int *in_num)
{
    struct vring_desc desc;
    unsigned int i, head, found = 0, num = vring->vr.num;
    uint16_t avail_idx, last_avail_idx;

    /* If there was a fatal error then refuse operation */
    if (vring->broken) {
        return -EFAULT;
    }

    /* Check it isn't doing very strange things with descriptor numbers. */
    last_avail_idx = vring->last_avail_idx;
    avail_idx = vring->vr.avail->idx;
    barrier(); /* load indices now and not again later */

    if (unlikely((uint16_t)(avail_idx - last_avail_idx) > num)) {
        error_report("Guest moved used index from %u to %u",
                     last_avail_idx, avail_idx);
        vring->broken = true;
        return -EFAULT;
    }

    /* If there's nothing new since last we looked. */
    if (avail_idx == last_avail_idx) {
        return -EAGAIN;
    }

    /* Only get avail ring entries after they have been exposed by guest. */
    mb();

    /* Grab the next descriptor number they're advertising, and increment
     * the index we've seen. */
    head = vring->vr.avail->ring[last_avail_idx % num];

    /* If their number is silly, that's an error. */
    if (unlikely(head >= num)) {
        error_report("Guest says index %u > %u is available", head, num);
        vring->broken = true;
        return -EFAULT;
    }

    if (vdev->guest_features & (1 << VIRTIO_RING_F_EVENT_IDX)) {
        vring_avail_event(&vring->vr) = vring->vr.avail->idx;
    }

    /* When we start there are none of either input nor output. */
    *out_num = *in_num = 0;

    i = head;
    do {
        if (unlikely(i >= num)) {
            error_report("Desc index is %u > %u, head = %u", i, num, head);
            vring->broken = true;
            return -EFAULT;
        }
        if (unlikely(++found > num)) {
            error_report("Loop detected: last one at %u vq size %u head %u",
                         i, num, head);
            vring->broken = true;
            return -EFAULT;
        }
        desc = vring->vr.desc[i];

        /* Ensure descriptor is loaded before accessing fields */
        barrier();

        if (desc.flags & VRING_DESC_F_INDIRECT) {
            int ret = get_indirect(vring, iov, iov_end, out_num, in_num, &desc);
            if (ret < 0) {
                return ret;
            }
            continue;
        }

        /* If there are not enough iovecs left, stop for now.  The caller
         * should check if there are more descs available once they have dealt
         * with the current set.
         */
        if (iov >= iov_end) {
            return -ENOBUFS;
        }

        /* TODO handle non-contiguous memory across region boundaries */
        iov->iov_base = hostmem_lookup(&vring->hostmem, desc.addr, desc.len,
                                       desc.flags & VRING_DESC_F_WRITE);
        if (!iov->iov_base) {
            error_report("Failed to map vring desc addr %#" PRIx64 " len %u",
                         (uint64_t)desc.addr, desc.len);
            vring->broken = true;
            return -EFAULT;
        }
        iov->iov_len  = desc.len;
        iov++;

        if (desc.flags & VRING_DESC_F_WRITE) {
            /* If this is an input descriptor,
             * increment that count. */
            *in_num += 1;
        } else {
            /* If it's an output descriptor, they're all supposed
             * to come before any input descriptors. */
            if (unlikely(*in_num)) {
                error_report("Descriptor has out after in: idx %d", i);
                vring->broken = true;
                return -EFAULT;
            }
            *out_num += 1;
        }
        i = desc.next;
    } while (desc.flags & VRING_DESC_F_NEXT);

    /* On success, increment avail index. */
    vring->last_avail_idx++;
    return head;
}

/* After we've used one of their buffers, we tell them about it.
 *
 * Stolen from linux/drivers/vhost/vhost.c.
 */
void vring_push(Vring *vring, unsigned int head, int len)
{
    struct vring_used_elem *used;
    uint16_t new;

    /* Don't touch vring if a fatal error occurred */
    if (vring->broken) {
        return;
    }

    /* The virtqueue contains a ring of used buffers.  Get a pointer to the
     * next entry in that used ring. */
    used = &vring->vr.used->ring[vring->last_used_idx % vring->vr.num];
    used->id = head;
    used->len = len;

    /* Make sure buffer is written before we update index. */
    mb();

    new = vring->vr.used->idx = ++vring->last_used_idx;
    if (unlikely((int16_t)(new - vring->signalled_used) < (uint16_t)1)) {
        vring->signalled_used_valid = false;
    }
}
