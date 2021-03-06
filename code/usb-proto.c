#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kref.h>
#include <linux/usb.h>
#include <linux/timer.h>
#include <linux/syscalls.h>
#include <linux/mm.h>      // read information about memory
#include <linux/fs.h>      // Needed by filp
#include <asm/uaccess.h>
#include <uapi/linux/sysinfo.h>
#include <linux/unistd.h>       /* for _syscallX macros/related stuff */
#include <linux/kernel.h>       /* for struct sysinfo */
#include <linux/sched.h>
#include <linux/stat.h>


#define con(x) ((x) << (PAGE_SHIFT -10))

struct timer_list timer_write_periodic;


static struct usb_device_id proto_table [] = {
        //{ USB_DEVICE(0x1A86, 0x7523) },  // ARDUINO NANO DOUGLAs
		{ USB_DEVICE(0x0403, 0x6001) },  // ARDUINO NANO Patrick
        { }				 // Last entry
};

MODULE_DEVICE_TABLE (usb, proto_table);


/* Get a minor range for your devices from the usb maintainer */
#define USB_SKEL_MINOR_BASE	192

/* Structure to hold all of our device specific stuff */
struct usb_device *	udev2;
struct usb_skel {
    struct usb_device *         udev;                   /* the usb device for this device */
    struct usb_interface *	interface;		/* the interface for this device */
    unsigned char *             bulk_in_buffer;         /* the buffer to receive data */
    size_t			bulk_in_size;           /* the size of the receive buffer */
    __u8			bulk_in_endpointAddr;	/* the address of the bulk in endpoint */
    __u8			bulk_out_endpointAddr;	/* the address of the bulk out endpoint */
    struct kref                 kref;
};

#define to_proto_dev(d) container_of(d, struct usb_skel, kref)

static struct usb_driver proto_driver;

void timer1_routine(unsigned long data_pass);

int step;




/************************************************************************************
*  DELETE
************************************************************************************/

static void proto_delete(struct kref *kref)
{	
    struct usb_skel *dev = to_proto_dev(kref);
    usb_put_dev(dev->udev);
    kfree (dev->bulk_in_buffer);
    kfree (dev);
}

/************************************************************************************
*  OPEN
************************************************************************************/

static int proto_open(struct inode *inode, struct file *file)
{
    struct usb_skel *dev;
    struct usb_interface *interface;
    int subminor;
    int retval = 0;

    subminor = iminor(inode);

    interface = usb_find_interface(&proto_driver, subminor);
    if (!interface) {
        pr_err("%s - error, can't find device for minor %d", __FUNCTION__, subminor);
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


/************************************************************************************
*  RELEASE
************************************************************************************/

static int proto_release(struct inode *inode, struct file *file)
{
    struct usb_skel *dev;

    dev = (struct usb_skel *)file->private_data;
    if (dev == NULL)
        return -ENODEV;

    /* decrement the count on our device */
    kref_put(&dev->kref, proto_delete);
    return 0;
}


/************************************************************************************
*  READ
************************************************************************************/

static ssize_t proto_read(struct file *file, char __user *buffer, size_t count, loff_t *ppos)
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

/************************************************************************************
*  WRITE BULK CALLBACK
************************************************************************************/

static void proto_write_bulk_callback(struct urb *urb)
{
    struct usb_skel *dev = urb->context;

    /* sync/async unlink faults aren't errors */
    if (urb->status &&
        !(urb->status == -ENOENT ||
          urb->status == -ECONNRESET ||
          urb->status == -ESHUTDOWN)) {
            dev_dbg(&dev->interface->dev, "%s - nonzero write bulk status received: %d", __FUNCTION__, urb->status);
    } else{
        printk("WRITE CALLBACK: block sent with success!\n");
    }

    /* free up our allocated buffer */
    usb_free_coherent(urb->dev, urb->transfer_buffer_length, urb->transfer_buffer, urb->transfer_dma);
}


/************************************************************************************
*  WRITE
************************************************************************************/

static ssize_t proto_write(struct file *file, const char __user *user_buffer, size_t count, loff_t *ppos)
{
    struct usb_skel *dev;
    int retval = 0;
    struct urb *urb = NULL;
    char *buf = NULL;

    dev = (struct usb_skel *) file->private_data;

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
                      buf, count, proto_write_bulk_callback, dev);
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


/************************************************************************************
*  WRITE PERIODIC
************************************************************************************/
#define LOAD_INT(x) ((x) >> FSHIFT)
#define LOAD_FRAC(x) LOAD_INT(((x) & (FIXED_1-1)) * 100)

static ssize_t proto_write_periodic(unsigned long data)
{
    //Size of buffer for dummy data
    int count = 5;

    struct usb_skel *dev;
    int retval = 0;
    struct urb *urb = NULL;
    char *buf = NULL;

    dev = (struct usb_skel *) data;

    printk("Inside Timer Routine count-> %d\n",step++);
    printk("Inside Timer idProduct %02X, idVendor %02X\n",dev->udev->descriptor.idProduct, dev->udev->descriptor.idVendor);
    struct sysinfo kk;
    si_meminfo(&kk);
    //sys_sysinfo(&kk);
    
    kk.uptime=jiffies/HZ;
    int days=kk.uptime/86400;
    int hours=kk.uptime/3600 - days*24;
    int mins=(kk.uptime/60) - (days*1440) - (hours*60);

	printk(KERN_INFO "Days since boot: %d\n", (days));
    printk(KERN_INFO "Hours since boot: %d\n", (hours));
    printk(KERN_INFO "Mins since boot: %d\n", (mins));
    printk(KERN_INFO "Seconds since boot: %d\n", (kk.uptime));
    printk(KERN_INFO "CPU load: %d\n", (kk.loads[0]));
    printk(KERN_INFO "CPU load: %d\n", (avenrun[0] << (SI_LOAD_SHIFT - FSHIFT)));
    printk(KERN_INFO "CPU load: %d\n", (avenrun[1] << (SI_LOAD_SHIFT - FSHIFT)));
    printk(KERN_INFO "CPU load: %d\n", (avenrun[2] << (SI_LOAD_SHIFT - FSHIFT)));
    int a, b, c;
    a = avenrun[0] + (FIXED_1/200);
    b = avenrun[1] + (FIXED_1/200);
    c = avenrun[2] + (FIXED_1/200);
    printk(KERN_INFO "%d.%02d %d.%02d %d.%02d\n", LOAD_INT(a), LOAD_FRAC(a), LOAD_INT(b), LOAD_FRAC(b), LOAD_INT(c), LOAD_FRAC(c));
    printk(KERN_INFO "Total usable main memory size: %lu\n", (kk.totalram)*(unsigned long long)kk.mem_unit/1048576);
    printk(KERN_INFO "Available memory size: %lu\n", (kk.freeram)*(unsigned long long)kk.mem_unit/1048576);
    printk(KERN_INFO "Number of current processes: %u\n", (kk.procs));
	
	



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

    //Dummy data
    printk("load %d\n",LOAD_INT(a)*100+LOAD_FRAC(a));
    //(kk.totalram)*((unsigned long long)kk.mem_unit/1048576)
    printk("mem %d\n",((kk.totalram)*(unsigned long long)kk.mem_unit/1048576));
    printk("mem %d\n",(100-((kk.freeram)*(unsigned long long)kk.mem_unit/1048576)*100/((kk.totalram)*(unsigned long long)kk.mem_unit/1048576)));
    buf[0]='R';
    buf[1]='a';//(int16_t)(avenrun[0] << (SI_LOAD_SHIFT - FSHIFT)*100);
    buf[2]=(char)(LOAD_INT(a)*100+LOAD_FRAC(a));
    buf[3]=(char)(100-((kk.freeram)*(unsigned long long)kk.mem_unit/1048576)*100/((kk.totalram)*(unsigned long long)kk.mem_unit/1048576));//hours;
    buf[4]='l';
    printk("WRITE LOKA: buffer:\n");
    int i;
    for (i= 0; i < 5; ++i)
    {
    	printk("%d - %c (%d)\n",buf[i],buf[i],i);
    }

    /* initialize the urb properly */
    usb_fill_bulk_urb(urb, dev->udev,
                      usb_sndbulkpipe(dev->udev, dev->bulk_out_endpointAddr),
                      buf, count, proto_write_bulk_callback, dev);
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
    mod_timer(&timer_write_periodic, jiffies + HZ); /* restarting timer */
    return count;

error:
    usb_free_coherent(dev->udev, count, buf, urb->transfer_dma);
    usb_free_urb(urb);
    kfree(buf);
    mod_timer(&timer_write_periodic, jiffies + HZ); /* restarting timer */
    return retval;
}

/************************************************************************************
*  FILE OPERATIONS
************************************************************************************/

static struct file_operations proto_fops = {
    .owner =	THIS_MODULE,
    .read =     proto_read,
    .write =	proto_write,
    .open =	proto_open,
    .release =	proto_release,
};

/* 
 * usb class driver info in order to get a minor number from the usb core,
 * and to have the device registered with devfs and the driver core
 */
static struct usb_class_driver proto_class = {
    .name = "usb/skel%d",
    .fops = &proto_fops,
    .minor_base = USB_SKEL_MINOR_BASE,
};


/************************************************************************************
*  PROBE
************************************************************************************/

static int proto_probe(struct usb_interface *interface, const struct usb_device_id *id)
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
            ((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_BULK)) {
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
            ((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_BULK)) {
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
    retval = usb_register_dev(interface, &proto_class);
    if (retval) {
        /* something prevented us from registering this driver */
        pr_err("Not able to get a minor for this device.");
        usb_set_intfdata(interface, NULL);
        goto error;
    }

    // create struct
    struct usb_skel * data;
    // alocs struct
    data = kmalloc(sizeof(*data), GFP_KERNEL);

    if (!data)
    {
        printk("Can't allocate memory for data structure\n");
        return -99;
    }

    // clear all
    memset(data, 0, sizeof(*data));
    // pass udev
    data->udev = udev2;

    /* let the user know what node this device is now attached to */
    dev_info(&interface->dev, "USB Skeleton device now attached to USBSkel-%d", interface->minor);
    data->interface = interface;
    data->bulk_out_endpointAddr = dev->bulk_out_endpointAddr;

    init_timer(&timer_write_periodic);
    timer_write_periodic.function = proto_write_periodic;
    timer_write_periodic.data = (unsigned long) data;
    timer_write_periodic.expires = jiffies + HZ; /* 1 second */
    printk(KERN_ALERT"Timer Module loaded\n");
    add_timer(&timer_write_periodic); /* Starting the timer */



    // seta baudrate ara 9600
    //https://github.com/damg/arnie-robot-controller/blob/f036c7066c2ca894911f8286df5219d6c42a5b2a/src/libftdi-0.14/ftdi.c
    //https://github.com/cyphunk/sectk/blob/50f7b5f552647fe073d25684dfddc9b8d59d17b4/often/bitbang_in_python/ftdibb-0.5/ftdibb.c
    //http://www.ftdichip.com/Support/Knowledgebase/index.html?whatbaudratesareachieveabl.htm
    //http://lxr.free-electrons.com/source/drivers/usb/serial/ftdi_sio.c
    //http://lxr.free-electrons.com/source/drivers/usb/serial/ftdi_sio.h
    //http://sourceforge.net/p/ftdi-usb-sio/mailman/message/4079784/
    //http://www.virtsync.com/c-error-codes-include-errno
    //https://www.kumari.net/index.php/random/37-determing-unknown-baud-rate
    //http://arduino.stackexchange.com/questions/296/how-high-of-a-baud-rate-can-i-go-without-errors
    //ftp://ftp.u-aizu.ac.jp/pub/os/Linux/kernel/v2.3/patch-html/patch-2.3.48/linux_drivers_usb_usb-serial.c.html
    //http://lxr.free-electrons.com/source/drivers/usb/serial/ftdi_sio.h?v=2.2.26
    //dev->udev, dev->bulk_out_endpointAddr
    int re = usb_control_msg(dev->udev, usb_sndctrlpipe(dev->udev, 0), 3, 0x40, 0x4138, 0, NULL, 0, HZ*5);
    //int re=0;
    if (re != 0)
    	printk(KERN_ALERT" >> Setting new baudrate failed %d",re);
    	else
    	printk(KERN_ALERT" >> Setting new baudrate sucess");

    return 0;

error:
    if (dev)
        kref_put(&dev->kref, proto_delete);
    return retval;
}


/************************************************************************************
*  DISCONNECT
************************************************************************************/

static void proto_disconnect(struct usb_interface *interface)
{
    struct usb_skel *dev;
	int minor = interface->minor;

	dev = usb_get_intfdata(interface);
	usb_set_intfdata(interface, NULL);

	/* give back our minor */
	usb_deregister_dev(interface, &proto_class);

	/* decrement our usage count */
	kref_put(&dev->kref, proto_delete);

	dev_info(&interface->dev, "USB Skeleton #%d now disconnected", minor);
	printk(KERN_ALERT"Timer Module killed\n");
        del_timer(&timer_write_periodic);
}


/************************************************************************************
*  USB_DRIVER
************************************************************************************/

static struct usb_driver proto_driver = {
	.name = "usb-proto",
	.id_table = proto_table,
	.probe = proto_probe,
	.disconnect = proto_disconnect,
};


/************************************************************************************
*  INIT
************************************************************************************/

static int __init usb_proto_init(void)
{
    int result;

    /* register this driver with the USB subsystem */
    result = usb_register(&proto_driver);
    if (result)
        pr_err("usb_register failed. Error number %d", result);

    return result;
}


/************************************************************************************
*  EXIT
************************************************************************************/

static void __exit usb_proto_exit(void)
{
    /* deregister this driver with the USB subsystem */
    usb_deregister(&proto_driver);
}


/************************************************************************************
*  WHERE IT ALL BEGINS
************************************************************************************/

module_init (usb_proto_init);
module_exit (usb_proto_exit);

MODULE_LICENSE("GPL");
