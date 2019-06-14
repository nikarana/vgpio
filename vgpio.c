#include <linux/kobject.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/ctype.h>

#define VGPIO_NAME		"vgpio"

#define vgpio_dbg(...)	pr_debug(VGPIO_NAME ": " __VA_ARGS__)
#define vgpio_info(...)	pr_info(VGPIO_NAME ": " __VA_ARGS__)
#define vgpio_warn(...)	pr_warn(VGPIO_NAME ": " __VA_ARGS__)
#define vgpio_err(...)	pr_err(VGPIO_NAME ": " __VA_ARGS__)

#define GPIO_IRQF_TRIGGER_FALLING	BIT(0)
#define GPIO_IRQF_TRIGGER_RISING	BIT(1)
#define GPIO_IRQF_TRIGGER_BOTH		(GPIO_IRQF_TRIGGER_FALLING | \
					 GPIO_IRQF_TRIGGER_RISING)

struct vgpiod_data {
	struct kobject kobj;
	int gpio;
	struct mutex mutex;

	struct kernfs_node *value_kn;

	int direction;		/* 0:in 1:out */
	int value;			/* 0 or 1 */
	int edge;			/* 0:none 1:falling 2:rising 3:both */
	int active_low;		/* 0:active_high 1:active_low */
};
#define to_vgpiod_data(x) container_of(x, struct vgpiod_data, kobj)

struct vgpiod_attribute {
	struct attribute attr;
	ssize_t (*show)(struct vgpiod_data *data, struct vgpiod_attribute *attr, char *buf);
	ssize_t (*store)(struct vgpiod_data *data, struct vgpiod_attribute *attr, const char *buf, size_t size);
};
#define to_vgpiod_attr(x) container_of(x, struct vgpiod_attribute, attr)

static int vgpiod_export(int gpio);
static void vgpiod_unexport(int gpio);

/*
 * Lock to serialise gpiod export and unexport, and prevent re-export of
 * gpiod whose chip is being unregistered.
 */
static DEFINE_MUTEX(sysfs_lock);

/*
 * /sys/class/gpio/gpioN... only for GPIOs that are exported
 *   /direction
 *      * MAY BE OMITTED if kernel won't allow direction changes
 *      * is read/write as "in" or "out"
 *      * may also be written as "high" or "low", initializing
 *        output value as specified ("out" implies "low")
 *   /value
 *      * always readable, subject to hardware behavior
 *      * may be writable, as zero/nonzero
 *   /edge
 *      * configures behavior of poll(2) on /value
 *      * available only if pin can generate IRQs on input
 *      * is read/write as "none", "falling", "rising", or "both"
 *   /active_low
 *      * configures polarity of /value
 *      * is read/write as zero/nonzero
 *      * also affects existing and subsequent "falling" and "rising"
 *        /edge configuration
 */

static ssize_t vgpiod_attr_show(struct kobject *kobj,
								struct attribute *attr,
								char *buf)
{
	struct vgpiod_attribute		*attribute;
	struct vgpiod_data			*data;

	attribute = to_vgpiod_attr(attr);
	data = to_vgpiod_data(kobj);

	if (!attribute->show)
		return -EIO;

	return attribute->show(data, attribute, buf);
}

static ssize_t vgpiod_attr_store(struct kobject *kobj,
								struct attribute *attr,
								const char *buf, size_t size)
{
	struct vgpiod_attribute		*attribute;
	struct vgpiod_data			*data;

	attribute = to_vgpiod_attr(attr);
	data = to_vgpiod_data(kobj);

	if (!attribute->store)
		return -EIO;

	return attribute->store(data, attribute, buf, size);
}

static const struct sysfs_ops vgpiod_sysfs_ops = {
	.show = vgpiod_attr_show,
	.store = vgpiod_attr_store,
};

static void vgpiod_release(struct kobject *kobj)
{
	struct vgpiod_data	*data;

	data = to_vgpiod_data(kobj);
	kfree(data);
}

static ssize_t direction_show(struct vgpiod_data *data,
								struct vgpiod_attribute *attr,
								char *buf)
{
	ssize_t				status;

	mutex_lock(&data->mutex);

	status = sprintf(buf, "%s\n", (data->direction) ? "out" : "in");

	mutex_unlock(&data->mutex);

	return status;
}

static ssize_t direction_store(struct vgpiod_data *data,
								struct vgpiod_attribute *attr,
								const char *buf, size_t size)
{
	ssize_t				status;

	mutex_lock(&data->mutex);

	if (sysfs_streq(buf, "high")) {
		status = 0;
		data->direction = 1;
		data->value = 1;
	} else if (sysfs_streq(buf, "low")) {
		status = 0;
		data->direction = 1;
			data->value = 0;
	} else if (sysfs_streq(buf, "out")) {
		status = 0;
		data->direction = 1;
	} else if (sysfs_streq(buf, "in")) {
		status = 0;
		data->direction = 0;
	} else {
		status = -EINVAL;
	}

	mutex_unlock(&data->mutex);

	return status ? : size;
}

static ssize_t value_show(struct vgpiod_data *data,
								struct vgpiod_attribute *attr,
								char *buf)
{
	ssize_t				status;
	long				value;

	mutex_lock(&data->mutex);

	value = data->value;

	if (data->active_low)
		value = !value;

	buf[0] = '0' + value;
	buf[1] = '\n';
	status = 2;

	mutex_unlock(&data->mutex);

	return status;
}

static ssize_t value_store(struct vgpiod_data *data,
								struct vgpiod_attribute *attr,
								const char *buf, size_t size)
{
	ssize_t				status = 0;

	mutex_lock(&data->mutex);

#if 0
	/* Normally, this check is necessary.
	 * However, this check is removed to change
	 *  the value of "GPIO in IN direction"
	 */
	if (! data->direction) {
		status = -EPERM;
	} else {
#else
	{
#endif
		int				value;

		if (size <= 2 && isdigit(buf[0]) &&
			(size == 1 || buf[1] == '\n'))
			value = buf[0] - '0';
		else
			status = kstrtoint(buf, 0, &value);

		if (status == 0) {
			if (data->active_low)
				value = !value;
			data->value = value;
			status = size;
		}
	}

	mutex_unlock(&data->mutex);

	return status;
}

static const struct {
	const char *name;
	unsigned char flags;
} trigger_types[] = {
	{ "none",    0 },
	{ "falling", GPIO_IRQF_TRIGGER_FALLING },
	{ "rising",  GPIO_IRQF_TRIGGER_RISING },
	{ "both",    GPIO_IRQF_TRIGGER_BOTH },
};

static ssize_t edge_show(struct vgpiod_data *data,
								struct vgpiod_attribute *attr,
								char *buf)
{
	ssize_t				status = 0;
	int i;

	mutex_lock(&data->mutex);

	for (i = 0; i < ARRAY_SIZE(trigger_types); i++) {
		if (data->edge == trigger_types[i].flags) {
			status = sprintf(buf, "%s\n", trigger_types[i].name);
			break;
		}
	}

	mutex_unlock(&data->mutex);

	return status;
}

static ssize_t edge_store(struct vgpiod_data *data,
							struct vgpiod_attribute *attr,
							const char *buf, size_t size)
{
	unsigned char		flags;
	ssize_t				status = size;
	int i;

	for (i = 0; i < ARRAY_SIZE(trigger_types); i++) {
		if (sysfs_streq(trigger_types[i].name, buf))
			break;
	}

	if (i == ARRAY_SIZE(trigger_types))
		return -EINVAL;

	flags = trigger_types[i].flags;

	mutex_lock(&data->mutex);

	data->edge = flags;
	status = size;

	mutex_unlock(&data->mutex);

	return status;
}

/* Caller holds vgpiod-data mutex. */
static int vgpio_sysfs_set_active_low(struct vgpiod_data *data, int value)
{
	data->active_low = !!value;

	return 0;
}

static ssize_t active_low_show(struct vgpiod_data *data,
								struct vgpiod_attribute *attr,
								char *buf)
{
	ssize_t				status;

	mutex_lock(&data->mutex);

	status = sprintf(buf, "%d\n", data->active_low);

	mutex_unlock(&data->mutex);

	return status;
}

static ssize_t active_low_store(struct vgpiod_data *data,
								struct vgpiod_attribute *attr,
								const char *buf, size_t size)
{
	ssize_t				status;
	int					value;

	mutex_lock(&data->mutex);

	status = kstrtoint(buf, 0, &value);
	if (status == 0)
		status = vgpio_sysfs_set_active_low(data, value);

	mutex_unlock(&data->mutex);

	return status ? : size;
}

static struct vgpiod_attribute vgpiod_direction_attr = {
	.attr = {.name = "direction", .mode = 0666 },
	.show = direction_show,
	.store = direction_store
};
//	__ATTR(direction, 0666, direction_show, direction_store);
static struct vgpiod_attribute vgpiod_edge_attr = {
	.attr = {.name = "edge", .mode = 0666 },
	.show = edge_show,
	.store = edge_store
};
//	__ATTR(edge, 0666, edge_show, edge_store);
static struct vgpiod_attribute vgpiod_value_attr = {
	.attr = {.name = "value", .mode = 0666 },
	.show = value_show,
	.store = value_store
};
//	__ATTR(value, 0666, value_show, value_store);
static struct vgpiod_attribute vgpiod_active_low_attr = {
	.attr = {.name = "active_low", .mode = 0666 },
	.show = active_low_show,
	.store = active_low_store
};
//	__ATTR(active_low, 0666, active_low_show, active_low_store);

static struct attribute *vgpiod_default_attrs[] = {
	&vgpiod_direction_attr.attr,
	&vgpiod_edge_attr.attr,
	&vgpiod_value_attr.attr,
	&vgpiod_active_low_attr.attr,
	NULL,	/* need to NULL terminate the list of attributes */
};

static struct kobj_type vgpiod_ktype = {
	.sysfs_ops = &vgpiod_sysfs_ops,
	.release = vgpiod_release,
	.default_attrs = vgpiod_default_attrs,
};

#if 0 /* Because "export" and "unexport" cannot be change mode to "-w--w--w-". */
/*
 * vgpio/export ... write-only
 *	integer N ... number of vGPIO to export (full access)
 * vgpio/unexport ... write-only
 *	integer N ... number of vGPIO to unexport
 */
static ssize_t export_store(struct kobject *kobj, struct kobj_attribute *attr,
								const char *buf, size_t size)
{
	int					gpio;
	int					status;

	status = kstrtoint(buf, 0, &gpio);
	if (status < 0)
		goto done;

	/* reject invalid GPIOs */
	if (gpio < 0) {
		vgpio_warn("%s: invalid GPIO %d\n", __func__, gpio);
		return -EINVAL;
	}

	status = vgpiod_export(gpio);

done:
	if (status)
		vgpio_dbg("%s: status %d\n", __func__, status);
	return status ? : size;
}

static ssize_t unexport_store(struct kobject *kobj, struct kobj_attribute *attr,
								const char *buf, size_t size)
{
	int					gpio;
	int					status;

	status = kstrtoint(buf, 0, &gpio);
	if (status < 0)
		goto done;

	/* reject invalid GPIOs */
	if (gpio < 0) {
		vgpio_warn("%s: invalid GPIO %d\n", __func__, gpio);
		return -EINVAL;
	}

	vgpiod_unexport(gpio);
	status = 0;

done:
	if (status)
		vgpio_dbg("%s: status %d\n", __func__, status);
	return status ? : size;
}

static struct kobj_attribute vgpio_export_attr =
	__ATTR(export, 0222, NULL, export_store);
static struct kobj_attribute vgpio_unexport_attr =
	__ATTR(unexport, 0222, NULL, unexport_store);

static struct attribute *vgpio_attrs[] = {
	&vgpio_export_attr.attr,
	&vgpio_unexport_attr.attr,
	NULL,
};

static struct attribute_group vgpio_group = {
	.attrs = vgpio_attrs,
};

#else /* Because "export" and "unexport" cannot be change mode to "-w--w--w-". */
struct vgpio_attribute {
	struct attribute attr;
	ssize_t (*show)(struct kobject *kobj, struct vgpio_attribute *attr, char *buf);
	ssize_t (*store)(struct kobject *kobj, struct vgpio_attribute *attr, const char *buf, size_t size);
};
#define to_vgpio_attr(x) container_of(x, struct vgpio_attribute, attr)

static ssize_t vgpio_attr_store(struct kobject *kobj,
								struct attribute *attr,
								const char *buf, size_t size)
{
	struct vgpio_attribute *attribute;

	attribute = to_vgpio_attr(attr);

	if (!attribute->store)
		return -EIO;

	return attribute->store(kobj, attribute, buf, size);
}

static const struct sysfs_ops vgpio_sysfs_ops = {
	.show = NULL,
	.store = vgpio_attr_store,
};

static void vgpio_release(struct kobject *kobj)
{
	kfree(to_kset(kobj));
}

static ssize_t export_store(struct kobject *kobj,
							struct vgpio_attribute *attr,
							const char *buf, size_t size)
{
	int					gpio;
	int					status;

	status = kstrtoint(buf, 0, &gpio);
	if (status < 0)
		goto done;

	/* reject invalid GPIOs */
	if (gpio < 0) {
		vgpio_warn("%s: invalid GPIO %d\n", __func__, gpio);
		return -EINVAL;
	}

	status = vgpiod_export(gpio);

done:
	if (status)
		vgpio_dbg("%s: status %d\n", __func__, status);
	return status ? : size;
}

static ssize_t unexport_store(struct kobject *kobj,
							struct vgpio_attribute *attr,
							const char *buf, size_t size)
{
	int					gpio;
	int					status;

	status = kstrtoint(buf, 0, &gpio);
	if (status < 0)
		goto done;

	/* reject invalid GPIOs */
	if (gpio < 0) {
		vgpio_warn("%s: invalid GPIO %d\n", __func__, gpio);
		return -EINVAL;
	}

	vgpiod_unexport(gpio);
	status = 0;

done:
	if (status)
		vgpio_dbg("%s: status %d\n", __func__, status);
	return status ? : size;
}

static struct vgpio_attribute vgpio_export_attr = {
	.attr = { .name = "export", .mode = 0222 },
	.show = NULL,
	.store = export_store,
};
//	__ATTR(export, 0222, NULL, export_store);
static struct vgpio_attribute vgpio_unexport_attr = {
	.attr = { .name = "unexport", .mode = 0222 },
	.show = NULL,
	.store = unexport_store,
};
//	__ATTR(unexport, 0222, NULL, unexport_store);

static struct attribute *vgpio_default_attrs[] = {
	&vgpio_export_attr.attr,
	&vgpio_unexport_attr.attr,
	NULL,	/* need to NULL terminate the list of attributes */
};

static struct kobj_type vgpio_ktype = {
	.sysfs_ops = &vgpio_sysfs_ops,
	.release = vgpio_release,
	.default_attrs = vgpio_default_attrs,
};

static struct kset *vgpio_kset_create_and_add(const char *name,
		const struct kset_uevent_ops *uevent_ops,
		struct kobject *parent_kobj,
		struct kobj_type *ktype)
{
	struct kset *kset;
	int error;

	kset = kzalloc(sizeof(*kset), GFP_KERNEL);
	if (!kset)
		return NULL;
	error = kobject_set_name(&kset->kobj, "%s", name);
	if (error) {
		kfree(kset);
		return NULL;
	}
	kset->uevent_ops = uevent_ops;
	kset->kobj.parent = parent_kobj;
	kset->kobj.ktype = ktype;
	kset->kobj.kset = NULL;

	error = kset_register(kset);
	if (error) {
		kfree(kset);
		return NULL;
	}

	return kset;
}
#endif /* Because "export" and "unexport" cannot be change mode to "-w--w--w-". */

static struct kset *vgpio_kset;

/**
 * vgpiod_export - export a vGPIO through sysfs
 * 
 * Returns zero on success, else an error.
 */
static int vgpiod_export(int gpio)
{
	struct vgpiod_data	*data;
	int					status;

	mutex_lock(&sysfs_lock);

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data) {
		status = -ENOMEM;
		goto err_unlock;
	}

	data->kobj.kset = vgpio_kset;

	data->gpio = gpio;
	mutex_init(&data->mutex);

	status = kobject_init_and_add(&data->kobj, &vgpiod_ktype, NULL,
								"gpio%d", gpio);
	if (status) {
		kobject_put(&data->kobj);
		goto err_unlock;
	}

	kobject_uevent(&data->kobj, KOBJ_ADD);

	mutex_unlock(&sysfs_lock);
	return 0;

err_unlock:
	mutex_unlock(&sysfs_lock);
	vgpio_dbg("%s: gpio%d status %d\n", __func__, gpio, status);
	return status;
}

/**
 * vgpiod_unexport - reverse effect of vgpiod_export()
 */
static void vgpiod_unexport(int gpio)
{
	struct kobject		*kobj;
	char				gpio_name[32];

	mutex_lock(&sysfs_lock);

	sprintf(gpio_name, "gpio%d", gpio);
	kobj = kset_find_obj(vgpio_kset, gpio_name);
	if (kobj) {
		/*
		 * Because the kobj ref-counter is increment by "kset_find_obj()". 
		 * I can not help but call kobject_put() twice.
		 */
		kobject_put(kobj);
		kobject_put(kobj);
	}

	mutex_unlock(&sysfs_lock);
}

static void unexport_all(void)
{
	mutex_lock(&sysfs_lock);

	while(1)
	{
		struct kobject		*k;
		struct kobject		*kobj = NULL;

		spin_lock(&vgpio_kset->list_lock);

		list_for_each_entry(k, &vgpio_kset->list, entry) {
			if (kobject_name(k)
				&& !!strcmp(kobject_name(k), "export")
				&& !!strcmp(kobject_name(k), "unexport")) {
				kobj = k;
				break;
			}
		}

		spin_unlock(&vgpio_kset->list_lock);

		if (kobj)
			kobject_put(kobj);
		else
			break;
	}

	mutex_unlock(&sysfs_lock);
}

static int __init vgpio_init(void)
{
	int					status = 0;

	vgpio_info("vGPIO start.\n");

#if 0 /* Because "export" and "unexport" cannot be change mode to "-w--w--w-". */
	/*
	 * Create a kset with the name of "vgpio",
	 * located under /sys/kernel/
	 */
	vgpio_kset = kset_create_and_add("vgpio", NULL, kernel_kobj);
	if (!vgpio_kset)
		return -ENOMEM;

	/*
	 * Create kobject-group("export" and "unexport").
	 */
	status = sysfs_create_group(&vgpio_kset->kobj, &vgpio_group);
	if (status)
		kset_unregister(vgpio_kset);

	return status;
#else /* Because "export" and "unexport" cannot be change mode to "-w--w--w-". */
	vgpio_kset = vgpio_kset_create_and_add("vgpio", NULL, kernel_kobj,
											&vgpio_ktype);
	if (!vgpio_kset)
		status = -ENOMEM;

	return status;
#endif /* Because "export" and "unexport" cannot be change mode to "-w--w--w-". */
}

static void __exit vgpio_exit(void)
{
	vgpio_info("vGPIO stop.\n");

	unexport_all();
	kset_unregister(vgpio_kset);
}

module_init(vgpio_init);
module_exit(vgpio_exit);

MODULE_AUTHOR("Nikarana <nikarana@gmail.com>");
MODULE_DESCRIPTION("Virtual GPIO driver");
MODULE_LICENSE("GPL v2");
