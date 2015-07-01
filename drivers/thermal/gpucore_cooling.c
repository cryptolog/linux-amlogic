/*
 *  linux/drivers/thermal/cpu_cooling.c
 *
 *  Copyright (C) 2012	Samsung Electronics Co., Ltd(http://www.samsung.com)
 *  Copyright (C) 2012  Amit Daniel <amit.kachhap@linaro.org>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */
#include <linux/module.h>
#include <linux/thermal.h>
#include <linux/cpufreq.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/gpucore_cooling.h>

/**
 * struct gpucore_cooling_device - data for cooling device with gpucore
 * @id: unique integer value corresponding to each gpucore_cooling_device
 *	registered.
 * @cool_dev: thermal_cooling_device pointer to keep track of the
 *	registered cooling device.
 * @gpucore_state: integer value representing the current state of gpucore
 *	cooling	devices.
 * @gpucore_val: integer value representing the absolute value of the clipped
 *	frequency.
 * @allowed_cpus: all the cpus involved for this gpucore_cooling_device.
 *
 * This structure is required for keeping information of each
 * gpucore_cooling_device registered. In order to prevent corruption of this a
 * mutex lock cooling_gpucore_lock is used.
 */

static DEFINE_IDR(gpucore_idr);
static DEFINE_MUTEX(cooling_gpucore_lock);

/* notify_table passes value to the gpucore_ADJUST callback function. */
#define NOTIFY_INVALID NULL

/**
 * get_idr - function to get a unique id.
 * @idr: struct idr * handle used to create a id.
 * @id: int * value generated by this function.
 *
 * This function will populate @id with an unique
 * id, using the idr API.
 *
 * Return: 0 on success, an error code on failure.
 */
static int get_idr(struct idr *idr, int *id)
{
	int ret;

	mutex_lock(&cooling_gpucore_lock);
	ret = idr_alloc(idr, NULL, 0, 0, GFP_KERNEL);
	mutex_unlock(&cooling_gpucore_lock);
	if (unlikely(ret < 0))
		return ret;
	*id = ret;

	return 0;
}

/**
 * release_idr - function to free the unique id.
 * @idr: struct idr * handle used for creating the id.
 * @id: int value representing the unique id.
 */
static void release_idr(struct idr *idr, int id)
{
	mutex_lock(&cooling_gpucore_lock);
	idr_remove(idr, id);
	mutex_unlock(&cooling_gpucore_lock);
}

/* gpucore cooling device callback functions are defined below */

/**
 * gpucore_get_max_state - callback function to get the max cooling state.
 * @cdev: thermal cooling device pointer.
 * @state: fill this variable with the max cooling state.
 *
 * Callback for the thermal cooling device to return the gpucore
 * max cooling state.
 *
 * Return: 0 on success, an error code otherwise.
 */
static int gpucore_get_max_state(struct thermal_cooling_device *cdev,
				 unsigned long *state)
{
	struct gpucore_cooling_device *gpucore_device = cdev->devdata;
	*state=gpucore_device->max_gpu_core_num;
	pr_debug( "max Gpu core=%ld\n",*state);
	return 0;
}

/**
 * gpucore_get_cur_state - callback function to get the current cooling state.
 * @cdev: thermal cooling device pointer.
 * @state: fill this variable with the current cooling state.
 *
 * Callback for the thermal cooling device to return the gpucore
 * current cooling state.
 *
 * Return: 0 on success, an error code otherwise.
 */
static int gpucore_get_cur_state(struct thermal_cooling_device *cdev,
				 unsigned long *state)
{
	struct gpucore_cooling_device *gpucore_device = cdev->devdata;
	*state=gpucore_device->gpucore_state;
	pr_debug( "current state=%ld\n",*state);
	return 0;
}

/**
 * gpucore_set_cur_state - callback function to set the current cooling state.
 * @cdev: thermal cooling device pointer.
 * @state: set this variable to the current cooling state.
 *
 * Callback for the thermal cooling device to change the gpucore
 * current cooling state.
 *
 * Return: 0 on success, an error code otherwise.
 */
static int gpucore_set_cur_state(struct thermal_cooling_device *cdev,
				 unsigned long state)
{
	struct gpucore_cooling_device *gpucore_device = cdev->devdata;
	int set_max_num;
	mutex_lock(&cooling_gpucore_lock);
	if(gpucore_device->stop_flag){
		mutex_unlock(&cooling_gpucore_lock);
		return 0;
	}
	if((state & GPU_STOP) ==GPU_STOP){
		gpucore_device->stop_flag=1;
		state=state&(~GPU_STOP);
	}
	mutex_unlock(&cooling_gpucore_lock);
	gpucore_device->gpucore_state=state;
	set_max_num=gpucore_device->max_gpu_core_num-state;
	gpucore_device->set_max_pp_num((unsigned int)set_max_num);
	pr_debug( "need set max gpu num=%d,state=%ld\n",set_max_num,state);
	return 0;
}

/* Bind gpucore callbacks to thermal cooling device ops */
static struct thermal_cooling_device_ops const gpucore_cooling_ops = {
	.get_max_state = gpucore_get_max_state,
	.get_cur_state = gpucore_get_cur_state,
	.set_cur_state = gpucore_set_cur_state,
};

/**
 * gpucore_cooling_register - function to create gpucore cooling device.
 * @clip_cpus: cpumask of cpus where the frequency constraints will happen.
 *
 * This interface function registers the gpucore cooling device with the name
 * "thermal-gpucore-%x". This api can support multiple instances of gpucore
 * cooling devices.
 *
 * Return: a valid struct thermal_cooling_device pointer on success,
 * on failure, it returns a corresponding ERR_PTR().
 */
 struct gpucore_cooling_device * gpucore_cooling_alloc(void)
{
	struct gpucore_cooling_device *gcdev;
	gcdev=kzalloc(sizeof(struct gpucore_cooling_device), GFP_KERNEL);
	if (!gcdev)
		return ERR_PTR(-ENOMEM);
	memset(gcdev,0,sizeof(*gcdev));
	return gcdev;
}
EXPORT_SYMBOL_GPL(gpucore_cooling_alloc);

struct thermal_cooling_device *
gpucore_cooling_register(struct gpucore_cooling_device *gpucore_dev)
{
	struct thermal_cooling_device *cool_dev;
	char dev_name[THERMAL_NAME_LENGTH];
	int ret = 0;
	ret = get_idr(&gpucore_idr, &gpucore_dev->id);
	if (ret) {
		kfree(gpucore_dev);
		return ERR_PTR(-EINVAL);
	}

	snprintf(dev_name, sizeof(dev_name), "thermal-gpucore-%d",
		 gpucore_dev->id);

	cool_dev = thermal_cooling_device_register(dev_name, gpucore_dev,
						   &gpucore_cooling_ops);
	if (!cool_dev) {
		
		release_idr(&gpucore_idr, gpucore_dev->id);
		kfree(gpucore_dev);
		return ERR_PTR(-EINVAL);
	}
	gpucore_dev->cool_dev = cool_dev;
	gpucore_dev->gpucore_state = 0;
	return 0;
}
EXPORT_SYMBOL_GPL(gpucore_cooling_register);

/**
 * gpucore_cooling_unregister - function to remove gpucore cooling device.
 * @cdev: thermal cooling device pointer.
 *
 * This interface function unregisters the "thermal-gpucore-%x" cooling device.
 */
void gpucore_cooling_unregister(struct thermal_cooling_device *cdev)
{
	struct gpucore_cooling_device *gpucore_dev;

	if (!cdev)
		return;

	gpucore_dev = cdev->devdata;

	thermal_cooling_device_unregister(gpucore_dev->cool_dev);
	release_idr(&gpucore_idr, gpucore_dev->id);
	kfree(gpucore_dev);
}
EXPORT_SYMBOL_GPL(gpucore_cooling_unregister);
