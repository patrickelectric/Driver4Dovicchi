/*
 * USB Skeleton driver - 2.0
 *
 * Copyright (C) 2001-2004 Greg Kroah-Hartman (greg@kroah.com)
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License as
 *	published by the Free Software Foundation, version 2.
 *
 * This driver is based on the 2.6.3 version of drivers/usb/usb-skeleton.c 
 * but has been rewritten to be easy to read and use, as no locks are now
 * needed anymore.
 *
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kref.h>
#include <linux/usb.h>
#include <linux/timer.h>
#include <asm/uaccess.h>


struct timer_list timer1;

/* Define these values to match your devices */
#define USB_SKEL_VENDOR_ID	0x0403
#define USB_SKEL_PRODUCT_ID	0x6001

/* table of devices that work with this driver */
static struct usb_device_id skel_table [] = {
	{ USB_DEVICE(USB_SKEL_VENDOR_ID, USB_SKEL_PRODUCT_ID) },
	{ }					/* Terminating entry */
};

struct patrick_vai 
{
	struct usb_device	*udev;
	__u8			bulk_out_endpointAddr;	/* the address of the bulk out endpoint */
};

MODULE_DEVICE_TABLE (usb, skel_table);


/* Get a minor range for your devices from the usb maintainer */
#define USB_SKEL_MINOR_BASE	192

/* Structure to hold all of our device specific stuff */
struct usb_device *	udev2;
struct usb_skel {
	struct usb_device *	udev;			/* the usb device for this device */
	struct usb_interface *	interface;		/* the interface for this device */
	unsigned char *		bulk_in_buffer;		/* the buffer to receive data */
	size_t			bulk_in_size;		/* the size of the receive buffer */
	__u8			bulk_in_endpointAddr;	/* the address of the bulk in endpoint */
	__u8			bulk_out_endpointAddr;	/* the address of the bulk out endpoint */
	struct kref		kref;

};

#define to_skel_dev(d) container_of(d, struct usb_skel, kref)

static struct usb_driver skel_driver;

void timer1_routine(unsigned long data_pass);

static void skel_delete(struct kref *kref)
{	
	struct usb_skel *dev = to_skel_dev(kref);

	usb_put_dev(dev->udev);
	kfree (dev->bulk_in_buffer);
	kfree (dev);
}

static int skel_open(struct inode *inode, struct file *file)
{
	struct usb_skel *dev;
	struct usb_interface *interface;
	int subminor;
	int retval = 0;

	subminor = iminor(inode);

	interface = usb_find_interface(&skel_driver, subminor);
	if (!interface) {
		pr_err("%s - error, can't find device for minor %d",
		     __FUNCTION__, subminor);
		retval = -ENODEV;
		goto exit;
	}

	dev = usb_get_intfdata(interface);
	if (!dev) {
		retval = -ENODEV;
		goto exit;
	}
	
	/* increment our usage count for the device */
	kref_get(&dev->kref);

	/* save our object in the file's private structure */
	file->private_data = dev;

exit:
	return retval;
}

static int skel_release(struct inode *inode, struct file *file)
{
	struct usb_skel *dev;

	dev = (struct usb_skel *)file->private_data;
	if (dev == NULL)
		return -ENODEV;

	/* decrement the count on our device */
	kref_put(&dev->kref, skel_delete);
	return 0;
}

static ssize_t skel_read(struct file *file, char __user *buffer, size_t count, loff_t *ppos)
{
	struct usb_skel *dev;
	int retval = 0;

	dev = (struct usb_skel *)file->private_data;
	
	/* do a blocking bulk read to get data from the device */
	retval = usb_bulk_msg(dev->udev,
			      usb_rcvbulkpipe(dev->udev, dev->bulk_in_endpointAddr),
			      dev->bulk_in_buffer,
			      min(dev->bulk_in_size, count),
			      &count, HZ*10);

	/* if the read was successful, copy the data to userspace */
	if (!retval) {
		if (copy_to_user(buffer, dev->bulk_in_buffer, count))
			retval = -EFAULT;
		else
			retval = count;
	}

	return retval;
}

static void skel_write_bulk_callback(struct urb *urb)
{
	struct usb_skel *dev = urb->context;

	/* sync/async unlink faults aren't errors */
	if (urb->status && 
	    !(urb->status == -ENOENT || 
	      urb->status == -ECONNRESET ||
	      urb->status == -ESHUTDOWN)) {
		dev_dbg(&dev->interface->dev,
			"%s - nonzero write bulk status received: %d",
			__FUNCTION__, urb->status);
	}

	/* free up our allocated buffer */
	usb_free_coherent(urb->dev, urb->transfer_buffer_length,
			urb->transfer_buffer, urb->transfer_dma);
}

static ssize_t skel_write(struct file *file, const char __user *user_buffer, size_t count, loff_t *ppos)
{
	struct usb_skel *dev;
	int retval = 0;
	struct urb *urb = NULL;
	char *buf = NULL;

	dev = (struct usb_skel *)file->private_data;

	/* verify that we actually have some data to write */
	if (count == 0)
		goto exit;

	/* create a urb, and a buffer for it, and copy the data to the urb */
	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb) {
		retval = -ENOMEM;
		goto error;
	}

	buf = usb_alloc_coherent(dev->udev, count, GFP_KERNEL, &urb->transfer_dma);
	if (!buf) {
		retval = -ENOMEM;
		goto error;
	}
	if (copy_from_user(buf, user_buffer, count)) {
		retval = -EFAULT;
		goto error;
	}

	/* initialize the urb properly */
	usb_fill_bulk_urb(urb, dev->udev,
			  usb_sndbulkpipe(dev->udev, dev->bulk_out_endpointAddr),
			  buf, count, skel_write_bulk_callback, dev);
	urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	/* send the data out the bulk port */
	retval = usb_submit_urb(urb, GFP_KERNEL);
	if (retval) {
		pr_err("%s - failed submitting write urb, error %d", __FUNCTION__, retval);
		goto error;
	}

	/* release our reference to this urb, the USB core will eventually free it entirely */
	usb_free_urb(urb);

exit:
	return count;

error:
	usb_free_coherent(dev->udev, count, buf, urb->transfer_dma);
	usb_free_urb(urb);
	kfree(buf);
	return retval;
}

static struct file_operations skel_fops = {
	.owner =	THIS_MODULE,
	.read =		skel_read,
	.write =	skel_write,
	.open =		skel_open,
	.release =	skel_release,
};

/* 
 * usb class driver info in order to get a minor number from the usb core,
 * and to have the device registered with devfs and the driver core
 */
static struct usb_class_driver skel_class = {
	.name = "usb/skel%d",
	.fops = &skel_fops,
	.minor_base = USB_SKEL_MINOR_BASE,
};

static int skel_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
	struct usb_skel *dev = NULL;
	struct usb_host_interface *iface_desc;
	struct usb_endpoint_descriptor *endpoint;
	size_t buffer_size;
	int i;
	int retval = -ENOMEM;

	/* allocate memory for our device state and initialize it */
	dev = kmalloc(sizeof(struct usb_skel), GFP_KERNEL);
	if (dev == NULL) {
		pr_err("Out of memory");
		goto error;
	}
	memset(dev, 0x00, sizeof (*dev));
	kref_init(&dev->kref);

	dev->udev = usb_get_dev(interface_to_usbdev(interface));
	udev2 = dev->udev;
	dev->interface = interface;

	/* set up the endpoint information */
	/* use only the first bulk-in and bulk-out endpoints */
	iface_desc = interface->cur_altsetting;
	for (i = 0; i < iface_desc->desc.bNumEndpoints; ++i) {
		endpoint = &iface_desc->endpoint[i].desc;

		if (!dev->bulk_in_endpointAddr &&
		    (endpoint->bEndpointAddress & USB_DIR_IN) &&
		    ((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK)
					== USB_ENDPOINT_XFER_BULK)) {
			/* we found a bulk in endpoint */
			buffer_size = endpoint->wMaxPacketSize;
			dev->bulk_in_size = buffer_size;
			dev->bulk_in_endpointAddr = endpoint->bEndpointAddress;
			dev->bulk_in_buffer = kmalloc(buffer_size, GFP_KERNEL);
			if (!dev->bulk_in_buffer) {
				pr_err("Could not allocate bulk_in_buffer");
				goto error;
			}
		}

		if (!dev->bulk_out_endpointAddr &&
		    !(endpoint->bEndpointAddress & USB_DIR_IN) &&
		    ((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK)
					== USB_ENDPOINT_XFER_BULK)) {
			/* we found a bulk out endpoint */
			dev->bulk_out_endpointAddr = endpoint->bEndpointAddress;
		}
	}
	if (!(dev->bulk_in_endpointAddr && dev->bulk_out_endpointAddr)) {
		pr_err("Could not find both bulk-in and bulk-out endpoints");
		goto error;
	}

	/* save our data pointer in this interface device */
	usb_set_intfdata(interface, dev);

	/* we can register the device now, as it is ready */
	retval = usb_register_dev(interface, &skel_class);
	if (retval) {
		/* something prevented us from registering this driver */
		pr_err("Not able to get a minor for this device.");
		usb_set_intfdata(interface, NULL);
		goto error;
	}

	// create struct
	struct patrick_vai *data;
	// alocs struct
	data = kmalloc(sizeof(*data), GFP_KERNEL);

	if (!data)
	{
		printk("Can't allocate memory for data structure");
		return -99;
	}

	// clear all
	memset(data, 0, sizeof(*data));
	// pass udev
	data->udev = udev2;

	/* let the user know what node this device is now attached to */
	dev_info(&interface->dev, "USB Skeleton device now attached to USBSkel-%d", interface->minor);

	init_timer(&timer1);
	timer1.function = timer1_routine;
	timer1.data = (unsigned long) data;
	timer1.expires = jiffies + HZ; /* 1 second */
	printk(KERN_ALERT"Timer Module loaded\n");
	add_timer(&timer1); /* Starting the timer */

	return 0;

error:
	if (dev)
		kref_put(&dev->kref, skel_delete);
	return retval;
}

static void skel_disconnect(struct usb_interface *interface)
{
	struct usb_skel *dev;
	int minor = interface->minor;

	dev = usb_get_intfdata(interface);
	usb_set_intfdata(interface, NULL);

	/* give back our minor */
	usb_deregister_dev(interface, &skel_class);

	/* decrement our usage count */
	kref_put(&dev->kref, skel_delete);

	dev_info(&interface->dev, "USB Skeleton #%d now disconnected", minor);
	printk(KERN_ALERT"Timer Module killed\n");
	del_timer(&timer1);
}

static struct usb_driver skel_driver = {
	.name = "skeleton",
	.id_table = skel_table,
	.probe = skel_probe,
	.disconnect = skel_disconnect,
};

int i;

void timer1_routine(unsigned long data_pass)
{
	struct patrick_vai *data = (struct patrick_vai *) data_pass;

	//printk(KERN_ALERT"Inside Timer Routine count-> %d data passed %ld\n",i++,data);
	printk("Inside Timer Routine count-> %d data passed %02X\n",i++,data);
	printk("Inside Timer idProduct %02X, idVendor %02X\n",data->udev->descriptor.idProduct,data->udev->descriptor.idVendor);
	mod_timer(&timer1, jiffies + HZ); /* restarting timer */

	char buffer[3];
	memset(&buffer,0,sizeof(buffer));
	buffer[0]='a';
	buffer[1]='\n';

	//usb_sndbulkpipe(data->udev, USB_TYPE_VENDOR | USB_RECIP_DEVICE,0 , 0, 0, NULL, buffer, 1, 1000);
	//usb_bulk_msg(data->udev, usb_sndctrlpipe(data->udev, 0), buffer, 1, 1, 1);
	//(data->udev, usb_sndctrlpipe(data->udev, 0), USB_TYPE_VENDOR | USB_RECIP_DEVICE, 0x21, 0x0, 0x0, &buffer, 1, HZ*5);

	struct urb *urb = NULL;
	char *buf = NULL;

	buf=usb_alloc_coherent(data->udev, 2, GFP_KERNEL, &urb->transfer_dma);

	urb = usb_alloc_urb(2, GFP_KERNEL);

	buf[0]=buffer[0];
	buf[1]=buffer[1];

	/* initialize the urb properly */
	usb_fill_bulk_urb(urb, data->udev,
			  usb_sndbulkpipe(data->udev, data->bulk_out_endpointAddr),
			  buf, 2, skel_write_bulk_callback, data);
	urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	int retval;
	/* send the data out the bulk port */
	retval=usb_submit_urb(urb, GFP_KERNEL);          

	if (retval) {
		pr_err("%s - failed submitting write urb, error %d", __FUNCTION__, retval);
	}

	usb_free_urb(urb);
}

static int __init usb_skel_init(void)
{
	int result;

	/* register this driver with the USB subsystem */
	result = usb_register(&skel_driver);
	if (result)
		pr_err("usb_register failed. Error number %d", result);

	return result;
}

static void __exit usb_skel_exit(void)
{
	/* deregister this driver with the USB subsystem */
	usb_deregister(&skel_driver);
}

module_init (usb_skel_init);
module_exit (usb_skel_exit);

MODULE_LICENSE("GPL");
