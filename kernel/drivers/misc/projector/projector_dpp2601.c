/*
 * projector.c  --  projector module driver
 *
 * Copyright (C) 2010 Samsung Electronics Co.Ltd
 * Author: Inbum Choi <inbum.choi@samsung.com>
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/earlysuspend.h>
#include <linux/mutex.h>
#include <linux/projector.h>
#include <mach/gpio.h>
#include <mach/board-sec-u8500.h>
#include <linux/device.h>
#include "PRJ_Data.h"

#ifndef	GPIO_LEVEL_LOW
#define GPIO_LEVEL_LOW		0
#define GPIO_LEVEL_HIGH		1
#endif

#define LED_COMPENSATION
#define R_compensation	0
#define G_compensation	0
#define B_compensation	0

#define DPPDATAFLASH(ARRAY) (dpp_flash(ARRAY, sizeof(ARRAY)/sizeof(ARRAY[0])))
#define GENRGBSETTING(array, data) (array[6] = (data))

/* MOTOR_DRV should be set before use it */
static unsigned int motor_drv1;
static unsigned int motor_drv2;
static unsigned int motor_drv3;
static unsigned int motor_drv4;

static int motor_step;
static int motor_abs_step;
static int once_motor_verified;

static int verify_value;

static char step_motor_cw[] = {0x0A, 0x06, 0x05, 0x09};

#define MOTOR_SLEEP 100
#define MOTOR_SLEEP_IMPROVED 10
#define MAX_MOTOR_STEP 60
#define MOTOR_MAX_PHASE ((sizeof(step_motor_cw)/sizeof(char)) - 1)
#define MOTOR_PHASE_CW_OUT(x)	do { \
	gpio_direction_output(motor_drv1, (step_motor_cw[(x)]&0x08)>>3);\
	gpio_direction_output(motor_drv2, (step_motor_cw[(x)]&0x04)>>2);\
	gpio_direction_output(motor_drv3, (step_motor_cw[(x)]&0x02)>>1);\
	gpio_direction_output(motor_drv4, step_motor_cw[(x)]&0x01);\
	if (system_rev <= GAVINI_R0_0_D)\
		msleep(MOTOR_SLEEP); else msleep(MOTOR_SLEEP_IMPROVED);\
	gpio_direction_output(motor_drv1, GPIO_LEVEL_LOW);\
	gpio_direction_output(motor_drv2, GPIO_LEVEL_LOW);\
	gpio_direction_output(motor_drv3, GPIO_LEVEL_LOW);\
	gpio_direction_output(motor_drv4, GPIO_LEVEL_LOW);\
} while (0);

static int brightness = BRIGHT_HIGH;

struct workqueue_struct *projector_work_queue;
struct work_struct projector_work_power_on;
struct work_struct projector_work_power_off;
struct work_struct projector_work_motor_cw;
struct work_struct projector_work_motor_ccw;
struct work_struct projector_work_testmode_on;

struct device *sec_projector;
extern struct class *sec_class;

extern u32 sec_lpm_bootmode;
static DECLARE_MUTEX(proj_mutex);

int screen_direction = PRJ_ROTATE_0;
EXPORT_SYMBOL(screen_direction);

static struct proj_val proj_values;

static int status;
static int priv_status;

static unsigned int not_calibrated = 2;
unsigned char RGB_BUF[MAX_LENGTH];
unsigned int max_dac[3];
static unsigned char seq_number;

volatile unsigned char flash_rgb_level_data[3][3][MAX_LENGTH] = {0,};

static unsigned char SequnceNumberForInit;

struct projector_dpp2601_info {
	struct i2c_client			*client;
	struct projector_dpp2601_platform_data	*pdata;
};

struct projector_dpp2601_info *info = NULL;

/* #ifdef CONFIG_HAS_EARLYSUSPEND
static struct early_suspend early_suspend = {
	.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN,
	.suspend = projector_module_early_suspend,
	.resume = projector_module_late_resume
};
#endif */

int dpp_flash(unsigned char *DataSetArray, int iNumArray)
{
	int i = 0;
	int CurrentDataIndex = 0;
	int Bytes = 0;
	
	for(i = 0 ; i < iNumArray ; i =CurrentDataIndex + Bytes)
	{
		msleep(1);		//temp 091201 , to prevent from abnormal operation like nosie screen
		Bytes =  DataSetArray[i + OFFSET_I2C_NUM_BYTES];
		CurrentDataIndex = i + OFFSET_I2C_DATA_START;

		if(DataSetArray[i + OFFSET_I2C_DIRECTION] == PRJ_WRITE){
			i2c_master_send(info->client, &DataSetArray[CurrentDataIndex], Bytes);
			printk("[%s] WRITE addr:", __func__);
			for(i=0;i<Bytes;i++)
				printk("%x ", DataSetArray[CurrentDataIndex+i]);
			printk("\n");
		}
		else if(DataSetArray[i + OFFSET_I2C_DIRECTION] == PRJ_READ){
			memset(RGB_BUF, 0x0, sizeof(RGB_BUF));
			i2c_master_recv(info->client, RGB_BUF, Bytes);
			printk(KERN_INFO "[%s] READ value:%x %x %x %x\n", __func__,
				RGB_BUF[0], RGB_BUF[1], RGB_BUF[2], RGB_BUF[3]);
		}
		else{
			printk(KERN_INFO "[%s] data is invalid !!\n", __func__);
			return -EINVAL;
		}
	}
	return 0;
}

void set_proj_status(int enProjectorStatus)
{
	priv_status = status;
	status = enProjectorStatus;
	printk(KERN_INFO "[%s] projector status : %d\n", __func__, status);
}

int get_proj_status(void)
{
	return status;
}
EXPORT_SYMBOL(get_proj_status);

static void projector_motor_cw_work(struct work_struct *work)
{
	motor_step++;
	if (motor_step > MOTOR_MAX_PHASE)
		motor_step = 0;
 
	printk(KERN_INFO "[%s] CW:%d\n", __func__, motor_step);
	MOTOR_PHASE_CW_OUT(motor_step);
	motor_abs_step++;
}

void projector_motor_cw(void)
{
	queue_work(projector_work_queue, &projector_work_motor_cw);
}

static void projector_motor_ccw_work(struct work_struct *work)
{
	motor_step--;
	if (motor_step < 0)
		motor_step = MOTOR_MAX_PHASE;

	printk(KERN_INFO "[%s] CCW:%d\n", __func__, motor_step);
	MOTOR_PHASE_CW_OUT(motor_step);
	motor_abs_step--;
}

void projector_motor_ccw(void)
{
	queue_work(projector_work_queue, &projector_work_motor_ccw);
}

void set_led_current(int level)
{
	printk(KERN_ERR "[%s] level:%d\n", __func__, level);
	#ifdef LED_COMPENSATION
	unsigned short temp_buffer = 0;
	#endif
	
	//RED_MSB
	#ifdef LED_COMPENSATION
	temp_buffer = (flash_rgb_level_data[level-1][0][2] << 8) | flash_rgb_level_data[level-1][0][3];
	printk("[%s] R temp_buffer : %x\n", __func__, temp_buffer);
	temp_buffer += R_compensation;
	printk("[%s] R+R_compensation temp_buffer : %x\n", __func__, temp_buffer);
	flash_rgb_level_data[level-1][0][2] = (temp_buffer & 0xff00) >> 8;
	flash_rgb_level_data[level-1][0][3] = temp_buffer & 0xff;
	#endif
	dpp_flash(red_msb_register, sizeof(red_msb_register)/sizeof(red_msb_register[0]));
	GENRGBSETTING(rgb_msb_lsb, flash_rgb_level_data[level-1][0][2]);
	dpp_flash(rgb_msb_lsb, sizeof(rgb_msb_lsb)/sizeof(rgb_msb_lsb[0]));
	dpp_flash(rgb_register_writing, sizeof(rgb_register_writing)/sizeof(rgb_register_writing[0]));

	//RED_LSB
	dpp_flash(red_lsb_register, sizeof(red_lsb_register)/sizeof(red_lsb_register[0]));
	GENRGBSETTING(rgb_msb_lsb, flash_rgb_level_data[level-1][0][3]);
	dpp_flash(rgb_msb_lsb, sizeof(rgb_msb_lsb)/sizeof(rgb_msb_lsb[0]));
	dpp_flash(rgb_register_writing, sizeof(rgb_register_writing)/sizeof(rgb_register_writing[0]));

	//GREEN_MSB
	#ifdef	LED_COMPENSATION
	temp_buffer = (flash_rgb_level_data[level-1][1][2] << 8) | flash_rgb_level_data[level-1][1][3];
	printk("[%s] G temp_buffer : %x\n", __func__, temp_buffer);
	temp_buffer += G_compensation;
	printk("[%s] G+G_compensation temp_buffer : %x\n", __func__, temp_buffer);
	flash_rgb_level_data[level-1][1][2] = (temp_buffer & 0xff00) >> 8;
	flash_rgb_level_data[level-1][1][3] = temp_buffer & 0xff;
	#endif
	dpp_flash(green_msb_register, sizeof(green_msb_register)/sizeof(green_msb_register[0]));
	GENRGBSETTING(rgb_msb_lsb, flash_rgb_level_data[level-1][1][2]);
	dpp_flash(rgb_msb_lsb, sizeof(rgb_msb_lsb)/sizeof(rgb_msb_lsb[0]));
	dpp_flash(rgb_register_writing, sizeof(rgb_register_writing)/sizeof(rgb_register_writing[0]));

	//GREEN_LSB
	dpp_flash(green_lsb_register, sizeof(green_lsb_register)/sizeof(green_lsb_register[0]));
	GENRGBSETTING(rgb_msb_lsb, flash_rgb_level_data[level-1][1][3]);
	dpp_flash(rgb_msb_lsb, sizeof(rgb_msb_lsb)/sizeof(rgb_msb_lsb[0]));
	dpp_flash(rgb_register_writing, sizeof(rgb_register_writing)/sizeof(rgb_register_writing[0]));

	//BLUE_MSB
	#ifdef LED_COMPENSATION
	temp_buffer = (flash_rgb_level_data[level-1][2][2] << 8) | flash_rgb_level_data[level-1][2][3];
	printk("[%s] B temp_buffer : %x\n", __func__, temp_buffer);
	temp_buffer += B_compensation;
	printk("[%s] B+B_compensation temp_buffer : %x\n", __func__, temp_buffer);
	flash_rgb_level_data[level-1][2][2] = (temp_buffer & 0xff00) >> 8;
	flash_rgb_level_data[level-1][2][3] = temp_buffer & 0xff;
	#endif
	dpp_flash(blue_msb_register, sizeof(blue_msb_register)/sizeof(blue_msb_register[0]));
	GENRGBSETTING(rgb_msb_lsb, flash_rgb_level_data[level-1][2][2]);
	dpp_flash(rgb_msb_lsb, sizeof(rgb_msb_lsb)/sizeof(rgb_msb_lsb[0]));
	dpp_flash(rgb_register_writing, sizeof(rgb_register_writing)/sizeof(rgb_register_writing[0]));

	//BLUE_LSB
	dpp_flash(blue_lsb_register, sizeof(blue_lsb_register)/sizeof(blue_lsb_register[0]));
	GENRGBSETTING(rgb_msb_lsb, flash_rgb_level_data[level-1][2][3]);
	dpp_flash(rgb_msb_lsb, sizeof(rgb_msb_lsb)/sizeof(rgb_msb_lsb[0]));
	dpp_flash(rgb_register_writing, sizeof(rgb_register_writing)/sizeof(rgb_register_writing[0]));
}

void pwron_seq_gpio(void)
{
	gpio_direction_output(info->pdata->gpio_mp_on, GPIO_LEVEL_LOW);
	msleep(100);
	
	gpio_direction_output(info->pdata->gpio_mp_on, GPIO_LEVEL_HIGH);
	msleep(200);

	gpio_direction_output(info->pdata->gpio_parkz, GPIO_LEVEL_HIGH);
	msleep(50);

	gpio_direction_output(info->pdata->gpio_prj_en, GPIO_LEVEL_HIGH);
	msleep(300);
}

void pwron_seq_direction(void)
{
	switch (get_proj_rotation()) {
	case PRJ_ROTATE_0:
		DPPDATAFLASH(Output_Rotate_0);
		break;
	case PRJ_ROTATE_90:
		DPPDATAFLASH(Output_Rotate_90);
		break;
	case PRJ_ROTATE_180:
		DPPDATAFLASH(Output_Rotate_180);
		break;
	case PRJ_ROTATE_270:
		DPPDATAFLASH(Output_Rotate_270);
		break;
	default:
		break;
	};
}

void pwron_seq_source_res(int value)
{
	if (value == LCD_VIEW) {
		DPPDATAFLASH(WVGA_RGB888);
	} else if (value == INTERNAL_PATTERN) {
		DPPDATAFLASH(nHD_RGB888);
	}
}

void pwron_seq_fdr(void)
{
	int i, cnt;
	unsigned int dac;

	DPPDATAFLASH(InitData_FlashDataLoading);
	msleep(3);

	for (cnt = 0; cnt < 10; cnt++) {
		DPPDATAFLASH(InitData_ReadFlashData);
		if (cnt < 9) {
			dac = 0;
			dac |= RGB_BUF[0] << 24 | RGB_BUF[1] << 16
				| RGB_BUF[2] << 8 |  RGB_BUF[3];

			not_calibrated = (dac < 2 || dac > 999) ? 1 : 0;

			if (dac >= max_dac[cnt / 3])
				max_dac[cnt / 3] = dac;
			memcpy(flash_rgb_level_data[cnt/3][cnt%3], RGB_BUF, MAX_LENGTH);
		}
	}
	seq_number = RGB_BUF[MAX_LENGTH-1];
	printk(KERN_ERR "[%s] seq_number %x\n", __func__, seq_number);
	printk(KERN_INFO "[%s] max_dac : %d, %d, %d\n", __func__,
					max_dac[0], max_dac[1], max_dac[2]);


	DPPDATAFLASH(InitData_TransferCtrlToI2C);
}

void pwron_seq_current_limit(void)
{
	unsigned int limit;
	unsigned char ireg = 0x07;
	unsigned char current_limit[] = {
		PRJ_WRITE, 2, 0x02, 0xFF
	};

	limit = (max_dac[brightness - 1] * 13) / 10;

	if (limit < 260)
		ireg |= 0x0 << 3;
	else if (limit >= 260 && limit < 300)
		ireg |= 0x1 << 3;
	else if (limit >= 300 && limit < 345)
		ireg |= 0x2 << 3;
	else if (limit >= 345 && limit < 385)
		ireg |= 0x3 << 3;
	else if (limit >= 385 && limit < 440)
		ireg |= 0x4 << 3;
	else if (limit >= 440 && limit < 660)
		ireg |= 0x5 << 3;
	else if (limit >= 660 && limit < 880)
		ireg |= 0x6 << 3;
	else if (limit >= 880)
		ireg |= 0x7 << 3;

	current_limit[3] = ireg;

	DPPDATAFLASH(current_limit);
	printk(KERN_INFO "[%s] Current Limit : %u, %#x\n", __func__,
			limit, (ireg & (~0x7)) >> 3);
}

static void proj_pwron_seq_work(struct work_struct *work)
{
	if (get_proj_status() == PRJ_ON_INTERNAL) {
		pwron_seq_direction();
		pwron_seq_source_res(LCD_VIEW);
		DPPDATAFLASH(External_source);
	} else {
		pwron_seq_gpio();

		pwron_seq_direction();
		pwron_seq_source_res(LCD_VIEW);

		pwron_seq_fdr();
		pwron_seq_current_limit();
		set_led_current(brightness);

		GENRGBSETTING(Dmd_seq, seq_number);
		DPPDATAFLASH(Dmd_seq);

		DPPDATAFLASH(External_source);
		DPPDATAFLASH(Free_run);
		DPPDATAFLASH(AGC_OFF);

		if (system_rev >= GAVINI_R0_1)
			gpio_direction_output(info->pdata->gpio_prj_led_en,
					GPIO_LEVEL_HIGH);
	}
	set_proj_status(PRJ_ON_RGB_LCD);
}

static void proj_testmode_pwron_seq_work(struct work_struct *work)
{
	pwron_seq_gpio();

	DPPDATAFLASH(Internal_pattern_direction);
	pwron_seq_source_res(INTERNAL_PATTERN);

	pwron_seq_fdr();
	pwron_seq_current_limit();
	set_led_current(brightness);

	GENRGBSETTING(Dmd_seq, seq_number);
	DPPDATAFLASH(Dmd_seq);
}

void proj_testmode_pwron_seq(void)
{
	queue_work(projector_work_queue, &projector_work_testmode_on);
}

void ProjectorPowerOnSequence(void)
{
	if (!sec_lpm_bootmode)
		queue_work(projector_work_queue, &projector_work_power_on);
}
EXPORT_SYMBOL(ProjectorPowerOnSequence);

static void proj_pwroff_seq_work(struct work_struct *work)
{
	if (system_rev >= GAVINI_R0_1)
		gpio_direction_output(info->pdata->gpio_prj_led_en,
				GPIO_LEVEL_LOW);

	gpio_direction_output(info->pdata->gpio_parkz, GPIO_LEVEL_LOW);
	msleep(50);

	gpio_direction_output(info->pdata->gpio_prj_en, GPIO_LEVEL_LOW);
	msleep(200);

	set_proj_status(PRJ_OFF);
}

void ProjectorPowerOffSequence(void)
{
	queue_work(projector_work_queue, &projector_work_power_off);
}
EXPORT_SYMBOL(ProjectorPowerOffSequence);

void turn_on_proj(unsigned int enProjectorStatus)
{
	if (priv_status == PRJ_OFF) {
		ProjectorPowerOnSequence();
	} else {
		DPPDATAFLASH(Output_Curtain_Enable);
	}
}

void turn_off_proj(void)
{
	ProjectorPowerOffSequence();
}

void rotate_proj_screen(int bLandscape)
{
	if (bLandscape != get_proj_rotation() && get_proj_status() == PRJ_ON_RGB_LCD) {
		switch (bLandscape) {
		case PRJ_ROTATE_0:
			DPPDATAFLASH(Output_Curtain_Enable);
			DPPDATAFLASH(Output_Rotate_0);
			msleep(50);
			DPPDATAFLASH(Output_Curtain_Disable);
			break;
		case PRJ_ROTATE_90:
			DPPDATAFLASH(Output_Curtain_Enable);
			DPPDATAFLASH(Output_Rotate_90);
			msleep(50);
			DPPDATAFLASH(Output_Curtain_Disable);
			break;
		case PRJ_ROTATE_180:
			DPPDATAFLASH(Output_Curtain_Enable);
			DPPDATAFLASH(Output_Rotate_180);
			msleep(50);
			DPPDATAFLASH(Output_Curtain_Disable);
			break;
		case PRJ_ROTATE_270:
			DPPDATAFLASH(Output_Curtain_Enable);
			DPPDATAFLASH(Output_Rotate_270);
			msleep(50);
			DPPDATAFLASH(Output_Curtain_Disable);
			break;
		default:
			break;
		};
		screen_direction = bLandscape;
	}
}

int get_proj_rotation(void)
{
	return screen_direction;
}

int get_proj_brightness(void)
{
	return brightness;
}

void proj_set_rotation_lock(int bRotationLock)
{
	if (proj_get_rotation_lock() != bRotationLock) {
		proj_values.bRotationLock = bRotationLock;
	}
}

int proj_get_rotation_lock(void)
{
	return proj_values.bRotationLock;
}

void PRJ_SetCurtainEnable(int bCurtainEnable)
{
	if (PRJ_GetCurtainEnable() != bCurtainEnable) {
		if (get_proj_status() != PRJ_OFF) {
			if (bCurtainEnable) {
				DPPDATAFLASH(Output_Curtain_Enable);
			} else {
				DPPDATAFLASH(Output_Curtain_Disable);
			}
		}
		proj_values.bCurtainEnable = bCurtainEnable;
	}
}

int PRJ_GetCurtainEnable(void)
{
	return proj_values.bCurtainEnable;
}

void PRJ_SetHighAmbientLight(int bHighAmbientLight)
{
	if (PRJ_GetHighAmbientLight() != bHighAmbientLight) {
		if (get_proj_status() == PRJ_ON_RGB_LCD) {
			DPPDATAFLASH(Output_Curtain_Enable);
			msleep(100);
			DPPDATAFLASH(Output_Curtain_Disable);
		}
		proj_values.bHighAmbientLight = bHighAmbientLight;
	}
}

int PRJ_GetHighAmbientLight(void)
{
	return proj_values.bHighAmbientLight;
}

int __devinit dpp2601_i2c_probe(struct i2c_client *client,
                const struct i2c_device_id *id)
{
	int ret = 0;

	if (system_rev < GAVINI_R0_0_B) {
		motor_drv1 = MOTDRV_IN1_GAVINI_R0_0;
		motor_drv2 = MOTDRV_IN2_GAVINI_R0_0;
		motor_drv3 = MOTDRV_IN3_GAVINI_R0_0;
		motor_drv4 = MOTDRV_IN4_GAVINI_R0_0;
	} else {
		motor_drv1 = MOTDRV_IN1_GAVINI_R0_0_B;
		motor_drv2 = MOTDRV_IN2_GAVINI_R0_0_B;
		motor_drv3 = MOTDRV_IN3_GAVINI_R0_0_B;
		motor_drv4 = MOTDRV_IN4_GAVINI_R0_0_B;
	}

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
	{
		printk(KERN_ERR "[%s] need I2C_FUNC_I2C\n", __func__);
		ret = -ENODEV;
		return ret;
	}
	
	info = kzalloc(sizeof(struct projector_dpp2601_info), GFP_KERNEL);
	if (!info) {
		printk(KERN_ERR "[%s] fail to memory allocation.\n", __func__);
		return -1;
	}

	info->client = client;
	info->pdata = client->dev.platform_data;

	i2c_set_clientdata(client, info);

	gpio_request(info->pdata->gpio_parkz, "PARKZ");
	gpio_request(info->pdata->gpio_mp_on, "MP_ON");
	gpio_request(info->pdata->gpio_prj_en, "PRJ_EN");
	gpio_request(info->pdata->gpio_prj_led_en, "PRJ_LED_EN");

	set_proj_status(PRJ_OFF);
	proj_set_rotation_lock(false);
	PRJ_SetCurtainEnable(false);
	PRJ_SetHighAmbientLight(false);

	projector_work_queue = create_singlethread_workqueue("projector_work_queue");
   if (!projector_work_queue){
		printk(KERN_ERR "[%s] i2c_probe fail.\n", __func__);
		return -ENOMEM;
   }

	INIT_WORK(&projector_work_power_on, proj_pwron_seq_work);
	INIT_WORK(&projector_work_power_off, proj_pwroff_seq_work);
	INIT_WORK(&projector_work_motor_cw, projector_motor_cw_work);
	INIT_WORK(&projector_work_motor_ccw, projector_motor_ccw_work);
	INIT_WORK(&projector_work_testmode_on, proj_testmode_pwron_seq_work);
	
	printk(KERN_ERR "[%s] dpp2601_i2c_probe.\n", __func__);

	return 0;
}

__devexit int dpp2601_i2c_remove(struct i2c_client *client)
{
	return 0;
}

static const struct i2c_device_id dpp2601_i2c_id[] = {
	{ "dpp2601", 0 },
	{ }
};
MODULE_DEVICE_TABLE(dpp2601_i2c, dpp2601_i2c_id);

static struct i2c_driver dpp2601_i2c_driver = {
	.driver = {
		.name = "dpp2601",
		.owner = THIS_MODULE,
	},
	.probe = dpp2601_i2c_probe,
	.remove = __devexit_p(dpp2601_i2c_remove),
	.id_table = dpp2601_i2c_id,
};

int projector_module_open(struct inode *inode, struct file *file)
{
		return 0;
}

int projector_module_release(struct inode *inode, struct file *file)
{
		return 0;
}

int projector_module_ioctl(struct inode *inode, struct file *file,
		       unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	if (down_interruptible(&proj_mutex))
		return -ERESTARTSYS;
	
	switch (cmd) {
	case IOCTL_PROJECTOR_SET_STATE:
		if (arg != get_proj_status() && arg < PRJ_MAX_STATE) {
			PRJ_SetCurtainEnable(false);
			set_proj_status(arg);
			if (arg == PRJ_OFF) {
				turn_off_proj();
			} else {
				turn_on_proj(arg);
				if (!not_calibrated) {
					ret = -1;
				}
			}
		}
		break;
	case IOCTL_PROJECTOR_GET_STATE:
		ret = get_proj_status();
		break;
	case IOCTL_PROJECTOR_SET_ROTATION:
		if (!proj_values.bRotationLock && arg < PRJ_MAX_ROTATE) {
			rotate_proj_screen(arg);
		}
		break;
	case IOCTL_PROJECTOR_GET_ROTATION:
		ret = get_proj_rotation();
		break;
	case IOCTL_PROJECTOR_GET_BRIGHTNESS:
		ret = get_proj_brightness();
		break;
	case IOCTL_PROJECTOR_SET_FUNCTION:
		if (arg & (0x1 << ROTATION_LOCK)) {
			proj_set_rotation_lock(true);
		} else {
			proj_set_rotation_lock(false);
		}
		if (arg& (0x1 << CURTAIN_ENABLE)) {
			PRJ_SetCurtainEnable(true);
		} else {
			PRJ_SetCurtainEnable(false);
		}
		if (arg & (0x1 << HIGH_AMBIENT_LIGHT)) {
			PRJ_SetHighAmbientLight(true);
		} else {
			PRJ_SetHighAmbientLight(false);
		}
		break;
	case IOCTL_PROJECTOR_GET_FUNCTION:
		if (proj_get_rotation_lock()) {
			ret |= (0x1 << ROTATION_LOCK);
		}
		if (PRJ_GetCurtainEnable()) {
			ret |= (0x1 << CURTAIN_ENABLE);
		}
		if (PRJ_GetHighAmbientLight()) {
			ret |= (0x1 << HIGH_AMBIENT_LIGHT);
		}
		break;
	default:
		printk("unknown cmd = %x\n", cmd);
		break;
	}
	up(&proj_mutex);
	return ret;
}

/* #ifdef CONFIG_HAS_EARLYSUSPEND
void projector_module_early_suspend(struct early_suspend *h)
{
	turn_off_proj();
}

void projector_module_late_resume(struct early_suspend *h)
{
	turn_on_proj(PRJ_ON_RGB_LCD);
}
#endif */

static struct file_operations projector_module_fops = {
	.owner = THIS_MODULE,
	.open = projector_module_open,
	.ioctl = projector_module_ioctl,
	.release = projector_module_release,
};

static struct miscdevice projector_module_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "secProjector",
	.fops = &projector_module_fops,
};

void project_internal(int pattern)
{
	if (get_proj_status() == PRJ_OFF) {
		proj_testmode_pwron_seq();
		msleep(2000);
	}

	if (get_proj_status() == PRJ_ON_RGB_LCD) {
		DPPDATAFLASH(Internal_pattern_direction);
		pwron_seq_source_res(INTERNAL_PATTERN);
		set_proj_status(PRJ_ON_INTERNAL);
	}

	if (get_proj_status() == RGB_LED_OFF) {
		DPPDATAFLASH(RGB_led_on);
		set_proj_status(PRJ_ON_INTERNAL);
	}

	switch (pattern) {
	case CHECKER:
		DPPDATAFLASH(I_4x4checker);
		DPPDATAFLASH(Free_run);
		verify_value = 20;
		break;
	case WHITE:
		DPPDATAFLASH(I_white);
		DPPDATAFLASH(Free_run);
		verify_value = 21;
		break;
	case BLACK:
		DPPDATAFLASH(I_black);
		DPPDATAFLASH(Free_run);
		verify_value = 22;
		break;
	case LEDOFF:
		DPPDATAFLASH(RGB_led_off);
		set_proj_status(RGB_LED_OFF);
		verify_value = 23;
		break;
	case RED:
		DPPDATAFLASH(I_red);
		DPPDATAFLASH(Free_run);
		verify_value = 24;
		break;
	case GREEN:
		DPPDATAFLASH(I_green);
		DPPDATAFLASH(Free_run);
		verify_value = 25;
		break;
	case BLUE:
		DPPDATAFLASH(I_blue);
		DPPDATAFLASH(Free_run);
		verify_value = 26;
		break;
	case BEAUTY:
		verify_value = 27;
		break;
	case STRIPE:
		DPPDATAFLASH(I_stripe);
		DPPDATAFLASH(Free_run);
		verify_value = 28;
		break;
	case CURTAIN_ON:
		DPPDATAFLASH(Output_Curtain_Enable);
		break;
	case CURTAIN_OFF:
		DPPDATAFLASH(Output_Curtain_Disable);
		break;
	default:
		break;
	}

	if (get_proj_status() == PRJ_OFF) {
		DPPDATAFLASH(AGC_OFF);
		if (system_rev >= GAVINI_R0_1)
			gpio_direction_output(info->pdata->gpio_prj_led_en,
					GPIO_LEVEL_HIGH);
		set_proj_status(PRJ_ON_INTERNAL);
	}
}

void move_motor_step(int value)
{
	int i, difference;

	difference = value - motor_abs_step;

	if (!once_motor_verified) {
		for (i = 0; i < MAX_MOTOR_STEP; i++) {
			motor_step--;
			if (motor_step < 0)
				motor_step = MOTOR_MAX_PHASE;

			printk(KERN_INFO "[%s] CCW:%d\n", __func__, motor_step);
			MOTOR_PHASE_CW_OUT(motor_step);
		}

		motor_abs_step = 0;

		for (i = 0; i < value; i++) {
			motor_step++;
			if (motor_step > MOTOR_MAX_PHASE)
				motor_step = 0;

			printk(KERN_INFO "[%s] CW:%d\n", __func__, motor_step);
			MOTOR_PHASE_CW_OUT(motor_step);
			motor_abs_step++;
		}
		once_motor_verified = 1;
	} else {
		if (difference > 0) {
			for (i = 0; i < difference; i++) {
				motor_step++;
				if (motor_step > MOTOR_MAX_PHASE)
					motor_step = 0;

				printk(KERN_INFO "[%s] CW:%d\n",
						__func__, motor_step);
				MOTOR_PHASE_CW_OUT(motor_step);
				motor_abs_step++;
			}
		} else if (difference < 0) {
			for (i = 0; i < -1 * difference; i++) {
				motor_step--;
				if (motor_step < 0)
					motor_step = MOTOR_MAX_PHASE;

				printk(KERN_INFO "[%s] CCW:%d\n",
						__func__, motor_step);
				MOTOR_PHASE_CW_OUT(motor_step);
				motor_abs_step--;
			}
		}
	}

	printk(KERN_INFO "[%s] Projector Motor ABS Step : %d\n",
			__func__, motor_abs_step);
	verify_value = 300 + value;
}

static ssize_t store_motor_action(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int direction;

	sscanf(buf, "%d\n", &direction);

	if (get_proj_status()) {
		switch (direction) {
		case MOTOR_CW:
			projector_motor_cw();
			printk(KERN_INFO "[%s] StepMotor CW rotated\n", __func__);
			break;
		case MOTOR_CCW:
			projector_motor_ccw();
			printk(KERN_INFO "[%s] StepMotor CCW rotated\n", __func__);
			break;
		default:
			break;
		}
	}

	return count;
}

static ssize_t store_brightness(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int value;

	sscanf(buf, "%d\n", &value);

	if (get_proj_status() == PRJ_OFF) {
		switch (value) {
		case 1:
			brightness = BRIGHT_HIGH;
			printk(KERN_INFO "[%s] Proj Brightness HIGH Set\n",
					__func__);
			break;
		case 2:
			brightness = BRIGHT_MID;
			printk(KERN_INFO "[%s] Proj Brightness MID Set\n",
					__func__);
			break;
		case 3:
			brightness = BRIGHT_LOW;
			printk(KERN_INFO "[%s] Proj Brightness LOW Set\n",
					__func__);
			break;
		default:
			break;
		}
	} else {
		DPPDATAFLASH(Output_Curtain_Enable);
		brightness = value;
		pwron_seq_current_limit();
		set_led_current(value);
		DPPDATAFLASH(Output_Curtain_Disable);
		printk(KERN_INFO "[%s] Proj Brightness Changed : %d\n",
					__func__, value);
	}
	return count;
}

static ssize_t store_proj_key(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int value;

	sscanf(buf, "%d\n", &value);

	switch (value) {
	case 0:
		if (get_proj_status()) {
			ProjectorPowerOffSequence();
			verify_value = 0;
		}
		break;
	case 1:
		if (get_proj_status() != PRJ_ON_RGB_LCD) {
			ProjectorPowerOnSequence();
			verify_value = 10;
		}
		break;
	default:
		break;
	};

	return count;
}

static ssize_t show_cal_history(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int size;

	size = sprintf(buf, "%d\n", not_calibrated);

	return size;
}

static ssize_t show_projection_verify(struct device *dev,
		struct device_attribute *attr, char *buf)
{

}

static ssize_t show_motor_verify(struct device *dev,
		struct device_attribute *attr, char *buf)
{

}

static ssize_t show_screen_direction(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int size;

	size = sprintf(buf, "%d\n", screen_direction);

	return size;
}

static ssize_t show_retval(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int size;

	size = sprintf(buf, "%d\n", verify_value);

	return size;
}

static ssize_t store_screen_direction(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int  value;

	sscanf(buf, "%d\n", &value);

	if (value >= 0 && value <= 3) {
		screen_direction = value;
	}

	return count;
}

static ssize_t store_rotate_screen(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int value;

	sscanf(buf, "%d\n", &value);

	if (value >= 0 && value <= 3) {
		printk(KERN_INFO "[%s] inputed rotate : %d\n", __func__, value);
		rotate_proj_screen(value);
	}

	return count;
}

static ssize_t store_projection_verify(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int value;

	sscanf(buf, "%d\n", &value);

	printk(KERN_INFO "[%s] selected internal pattern : %d\n",
			__func__, value);
	project_internal(value);

	return count;
}


static ssize_t store_motor_verify(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int value;

	sscanf(buf, "%d\n", &value);

	if (value >= 0 && value <= 60) {
		printk(KERN_INFO "[%s] selected motor abs step : %d\n",
				__func__, value);
		move_motor_step(value);
	}

	return count;
}

static DEVICE_ATTR(proj_motor, S_IRUGO | S_IWUSR, NULL, store_motor_action);
static DEVICE_ATTR(brightness, S_IRUGO | S_IWUSR, NULL, store_brightness);
static DEVICE_ATTR(proj_key, S_IRUGO | S_IWUSR, NULL, store_proj_key);
static DEVICE_ATTR(cal_history, S_IRUGO, show_cal_history, NULL);
static DEVICE_ATTR(rotate_screen, S_IRUGO | S_IWUSR,
				NULL, store_rotate_screen);
static DEVICE_ATTR(screen_direction, S_IRUGO | S_IWUSR,
				show_screen_direction, store_screen_direction);
static DEVICE_ATTR(projection_verify, S_IRUGO | S_IWUSR,
			show_projection_verify, store_projection_verify);
static DEVICE_ATTR(motor_verify, S_IRUGO | S_IWUSR,
				show_motor_verify, store_motor_verify);
static DEVICE_ATTR(retval, S_IRUGO, show_retval, NULL);

int __init projector_module_init(void)
{
	int ret;

	sec_projector = device_create(sec_class, NULL, 0, NULL, "sec_projector");
	if (IS_ERR(sec_projector)) {
		printk(KERN_ERR "Failed to create device(sec_projector)!\n");
	}

	if (device_create_file(sec_projector, &dev_attr_proj_motor) < 0)
		printk(KERN_ERR "Failed to create device file(%s)!\n",
				dev_attr_proj_motor.attr.name);

	if (device_create_file(sec_projector, &dev_attr_brightness) < 0)
		printk(KERN_ERR "Failed to create device file(%s)!\n",
				dev_attr_brightness.attr.name);

	if (device_create_file(sec_projector, &dev_attr_proj_key) < 0)
		printk(KERN_ERR "Failed to create device file(%s)!\n",
				dev_attr_proj_key.attr.name);

	if (device_create_file(sec_projector, &dev_attr_cal_history) < 0)
		printk(KERN_ERR "Failed to create device file(%s)!\n",
				dev_attr_cal_history.attr.name);

	if (device_create_file(sec_projector, &dev_attr_rotate_screen) < 0)
		printk(KERN_ERR "Failed to create device file(%s)!\n",
				dev_attr_rotate_screen.attr.name);

	if (device_create_file(sec_projector, &dev_attr_screen_direction) < 0)
		printk(KERN_ERR "Failed to create device file(%s)!\n",
				dev_attr_screen_direction.attr.name);

	if (device_create_file(sec_projector, &dev_attr_projection_verify) < 0)
		printk(KERN_ERR "Failed to create device file(%s)!\n",
				dev_attr_projection_verify.attr.name);

	if (device_create_file(sec_projector, &dev_attr_motor_verify) < 0)
		printk(KERN_ERR "Failed to create device file(%s)!\n",
				dev_attr_motor_verify.attr.name);

	if (device_create_file(sec_projector, &dev_attr_retval) < 0)
		printk(KERN_ERR "Failed to create device file(%s)!\n",
				dev_attr_retval.attr.name);

	ret = i2c_add_driver(&dpp2601_i2c_driver);
	ret |= misc_register(&projector_module_device);
	if (ret) {
		printk(KERN_ERR "Projector driver registration failed!\n");
	}
	return ret;
}

void __exit projector_module_exit(void)
{
	i2c_del_driver(&dpp2601_i2c_driver);
	misc_deregister(&projector_module_device);
}

late_initcall(projector_module_init);
module_exit(projector_module_exit);

MODULE_DESCRIPTION("Samsung projector module driver");
MODULE_AUTHOR("Inbum Choi <inbum.choi@samsung.com>");
MODULE_LICENSE("GPL");
