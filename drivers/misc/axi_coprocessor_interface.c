#include <linux/platform_device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include <asm/io.h>
#include <linux/misc/axi_coprocessor_interface.h>

#define DEVICE_NAME "axi_coprocessor_interface"

struct interface_priv{
	void *mmio;
	unsigned int buf_in_offs;
	unsigned int buf_out_offs;
};

static struct interface_priv interface_private_data;

static u32 interface_get_register(struct interface_priv *dev, u32 register_offset)
{
	volatile u32 *reg;
	reg = (u32*)( (u32)(dev->mmio) + register_offset);
	return *reg;
}

static void interface_set_register(struct interface_priv *dev, u32 register_offset, u32 register_value)
{
	volatile u32 *reg;
	reg = (u32*)( (u32)(dev->mmio) + register_offset);
	*reg = register_value;
}

/* char driver api */

static int interface_open(struct inode *inode, struct file *file)
{
	file->private_data = &interface_private_data;
	return 0;
}

static int interface_close(struct inode *inode, struct file *file)
{
	file->private_data = NULL;
	return 0;
}

static ssize_t interface_read(struct file *file, char __user *buffer, size_t length, loff_t *offset)
{
	u32 reg;
	u32 *buf = (u32*)kmalloc(length + (4 - (length % 4)), GFP_KERNEL);
	u32 copied_data = 0;
	struct interface_priv *priv;
	
	if(!buf)
		return -ENOMEM;
	/* get driver private data */
	priv = (struct interface_priv*)file->private_data;
	
	/* copy data from FIFO to out buf */
	reg = interface_get_register(priv, INTERFACE_CONTROL_REG);
	reg &= ~(INTERFACE_COPY_DATA_FROM_FIFO);
	interface_set_register(priv, INTERFACE_CONTROL_REG, reg);
	reg |= INTERFACE_COPY_DATA_FROM_FIFO;
	interface_set_register(priv, INTERFACE_CONTROL_REG, reg);
	
	/* wait till data is copied  */
	reg = interface_get_register(priv, INTERFACE_STATUS_REG);

	while(reg & INTERFACE_FIFO_OUT_COPYING_MASK)
	{
		reg = interface_get_register(priv, INTERFACE_STATUS_REG);
	}

	/* get data amount */
	copied_data = interface_get_register(priv, INTERFACE_FIFO_TRANSFER_STATUS_REG);
	copied_data &= INTERFACE_OUTPUT_DATA_COPIED_MASK;
	copied_data >>= INTERFACE_OUTPUT_DATA_COPIED_SHIFT;
	copied_data *= 4;

	memcpy(buf, (void*)(priv->mmio + priv->buf_out_offs), copied_data);
	
	if( copy_to_user((void*)buffer, buf, copied_data) != 0 )
		copied_data =  -EFAULT;	

	kfree(buf);
	return copied_data;
}

static ssize_t interface_write(struct file *file, const char __user *buffer, size_t length, loff_t *offset)
{
	u32 reg;
	
	u32 *buf = (u32*)kmalloc(length + (4 - (length % 4)), GFP_KERNEL); //data must be aligned to 4
	struct interface_priv *priv;
	
	if(!buf)
	{
			printk(KERN_ERR"alloc error \n");
			return -ENOMEM;
	}
	
	/* get driver private data */
	priv = (struct interface_priv*)file->private_data; 

	if( copy_from_user(buf, buffer, length) != 0)
	{
		kfree(buf);
		return -EFAULT;
	}

	/* copy data to buffer */
	memcpy( (void*)(priv->mmio + priv->buf_in_offs), buf, length);

	/* push data from buffer into fifo */

	/* set data length */
	interface_set_register(priv, INTERFACE_FIFO_IN_DATA_AMOUNT_REG, ((length/4) & INTERFACE_IN_DATA_AMOUNT_MASK));
	/* trigger copy process */
	reg = interface_get_register(priv, INTERFACE_CONTROL_REG);
	reg &= ~(INTERFACE_COPY_DATA_TO_FIFO);
	interface_set_register(priv, INTERFACE_CONTROL_REG, reg);
	reg |= INTERFACE_COPY_DATA_TO_FIFO;
	interface_set_register(priv, INTERFACE_CONTROL_REG, reg);

	/* wait until data is copied */
	reg = interface_get_register(priv, INTERFACE_STATUS_REG);
	while( reg & INTERFACE_FIFO_IN_COPYING_MASK)
	{
		reg = interface_get_register(priv, INTERFACE_STATUS_REG);
	}
	kfree(buf);

	/* get real amount of copied data */
	reg = interface_get_register(priv, INTERFACE_FIFO_TRANSFER_STATUS_REG);
	reg &= INTERFACE_INPUT_DATA_COPIED_MASK;
	reg >>=INTERFACE_INPUT_DATA_COPIED_SHIFT; 
	reg *= 4;
	return reg;
}

static long interface_ioctl(struct file *file, unsigned int ioctl_num, unsigned long ioctl_param)
{
	struct interface_ioctl_data ioctl_arg; 	//this is used for setting and getting regs
	unsigned int ioctl_data;		//this is used for getting FIFO status
	struct interface_priv *priv;

	priv = (struct interface_priv*) file->private_data;
	
	/* get data from user space */
	if(ioctl_num == INTERFACE_SET_USER_REGISTER || ioctl_num == INTERFACE_GET_USER_REGISTER)
	{
		if(copy_from_user(&ioctl_arg, (struct interface_ioctl_data*) ioctl_param, sizeof(struct interface_ioctl_data)))
	                return -EACCES;
	}
	else 
	{	
		if(copy_from_user(&ioctl_data, (unsigned int*)ioctl_param, sizeof(unsigned int)))
			return -EACCES;
	}

	switch(ioctl_num)
	{
		case INTERFACE_SET_USER_REGISTER:
			
			if( ioctl_arg.register_offset < INTERFACE_USER_REG_0 || ioctl_arg.register_offset > INTERFACE_USER_REG_5 )
				return -EACCES;

			interface_set_register(priv, (u32)ioctl_arg.register_offset, (u32)ioctl_arg.register_value);
			break;

		case INTERFACE_GET_USER_REGISTER:

			if( ioctl_arg.register_offset < INTERFACE_USER_REG_0 || ioctl_arg.register_offset > INTERFACE_USER_REG_5 )
				return -EACCES;

			ioctl_arg.register_value = interface_get_register(priv, ioctl_arg.register_offset);

			if( copy_to_user((struct interface_ioctl_data*)ioctl_param, &ioctl_arg,sizeof(struct interface_ioctl_data)) )
			{
				return -EACCES;
			}
			break;
		case INTERFACE_GET_FIFO_IN_EMPTY:

			ioctl_data = interface_get_register(priv, INTERFACE_STATUS_REG);
			ioctl_data &= INTERFACE_FIFO_IN_EMPTY_MASK;
			ioctl_data >>= INTERFACE_FIFO_IN_EMPTY_SHIFT;
			break;

		case INTERFACE_GET_FIFO_IN_FULL:

			ioctl_data = interface_get_register(priv, INTERFACE_STATUS_REG);
			ioctl_data &= INTERFACE_FIFO_IN_FULL_MASK;
			ioctl_data >>= INTERFACE_FIFO_IN_FULL_SHIFT;
			break;

		case INTERFACE_GET_FIFO_IN_COPY_STATUS:

			ioctl_data = interface_get_register(priv, INTERFACE_STATUS_REG);
			ioctl_data &= INTERFACE_FIFO_IN_COPYING_MASK;
			ioctl_data >>= INTERFACE_FIFO_IN_COPYING_SHIFT;
			break;

		case INTERFACE_GET_FIFO_IN_COPIED_DATA_AMOUNT:

			ioctl_data = interface_get_register(priv, INTERFACE_FIFO_TRANSFER_STATUS_REG);
			ioctl_data &= INTERFACE_INPUT_DATA_COPIED_MASK;
			ioctl_data >>= INTERFACE_INPUT_DATA_COPIED_SHIFT;
			break;

		case INTERFACE_GET_FIFO_OUT_EMPTY:

			ioctl_data = interface_get_register(priv, INTERFACE_STATUS_REG);
			ioctl_data &= INTERFACE_FIFO_OUT_EMPTY_MASK;
			ioctl_data >>= INTERFACE_FIFO_OUT_EMPTY_SHIFT;
			break;

		case INTERFACE_GET_FIFO_OUT_FULL:

			ioctl_data = interface_get_register(priv, INTERFACE_STATUS_REG);
			ioctl_data &= INTERFACE_FIFO_OUT_FULL_MASK;
			ioctl_data >>= INTERFACE_FIFO_OUT_FULL_SHIFT;
			break;

		case INTERFACE_GET_FIFO_OUT_COPY_STATUS:

			ioctl_data = interface_get_register(priv, INTERFACE_STATUS_REG);
			ioctl_data &= INTERFACE_FIFO_OUT_COPYING_MASK;
			ioctl_data >>= INTERFACE_FIFO_OUT_COPYING_SHIFT;
			break;

		case INTERFACE_GET_FIFO_OUT_COPIED_DATA_AMOUNT:

			ioctl_data = interface_get_register(priv, INTERFACE_FIFO_TRANSFER_STATUS_REG);
			ioctl_data &= INTERFACE_OUTPUT_DATA_COPIED_MASK;
			ioctl_data >>= INTERFACE_OUTPUT_DATA_COPIED_SHIFT;
			break;

		default:
			return -EINVAL;
	}
	if(ioctl_num != INTERFACE_SET_USER_REGISTER && ioctl_num != INTERFACE_GET_USER_REGISTER)
	{
		if(copy_to_user((unsigned int*)ioctl_param, &ioctl_data,sizeof(unsigned int)))
		{
			return -EACCES;
		}
	}
	return 0;
}

struct file_operations fops = {
				.read = interface_read,
				.write = interface_write,
				.unlocked_ioctl = interface_ioctl,
				.open = interface_open,
				.release = interface_close,};

/* platform driver api */

static int interface_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct resource *res;
	unsigned int dev_space = 0;
        struct device_node *np = pdev->dev.of_node;
	unsigned int in_buf_offset;
	unsigned int out_buf_offset;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
	{
		printk(KERN_ERR"%s: Failed to get device resource\n", DEVICE_NAME);
		return -ENODEV;
	}

	dev_space = res->end - res->start;

	if( dev_space < INTERFACE_MINIMAL_REGISTER_SPACE )
	{
		printk(KERN_ERR"%s: Insufficient memory space\n", DEVICE_NAME);
		return -ENODEV;
	}

	interface_private_data.mmio = ioremap(res->start, dev_space);
	
	if( of_property_read_u32(np, "kik,input_buffer_offset", &in_buf_offset))
	{
		printk(KERN_ERR"%s: Cannot get input buffer offset\n", DEVICE_NAME);
		return -ENODEV;
	}

	if( of_property_read_u32(np, "kik,output_buffer_offset", &out_buf_offset))
	{
		printk(KERN_ERR"%s: Cannot get output buffer offset\n", DEVICE_NAME);
		return -ENODEV;
	}

	interface_private_data.buf_in_offs = in_buf_offset;
	interface_private_data.buf_out_offs = out_buf_offset;

	ret = register_chrdev(INTERFACE_MAJOR_NUMBER, DEVICE_NAME, &fops);
	if (ret < 0)
	{
		printk(KERN_ERR"%s: Char device registration failed\n", DEVICE_NAME);
		return -ENODEV;
	}
	return 0;
}

static int interface_remove(struct platform_device *pdev)
{
	unregister_chrdev(INTERFACE_MAJOR_NUMBER, DEVICE_NAME);
	return 0;
}

/* Match table for of_platform binding */
static struct of_device_id interface_of_match[] = {
         { .compatible = "kik,axi_coprocessor_interface", },
         {}
};
MODULE_DEVICE_TABLE(of, interface_of_match);
 
static struct platform_driver interface_platform_driver = {
         .probe   = interface_probe,               /* Probe method */
         .remove  = interface_remove,              /* Detach method */
         .driver  = {
                 .owner = THIS_MODULE,
                 .name = DEVICE_NAME,           /* Driver name */
                 .of_match_table = interface_of_match,
                 },
};

/* module api */

static int __init interface_init(void)
{
	return platform_driver_register(&interface_platform_driver);
}

static void __exit interface_exit(void)
{
	platform_driver_unregister(&interface_platform_driver);
}

module_init(interface_init);
module_exit(interface_exit);

MODULE_AUTHOR("Karol Gugala <karol.gugala@put.poznan.pl>");
MODULE_DESCRIPTION("AXI coprocessor interface");
MODULE_LICENSE("GPL v2");
