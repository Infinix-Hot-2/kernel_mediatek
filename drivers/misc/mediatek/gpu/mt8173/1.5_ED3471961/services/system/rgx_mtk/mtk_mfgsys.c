/*
* Copyright (c) 2014 MediaTek Inc.
* Author: Chiawen Lee <chiawen.lee@mediatek.com>
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2 as
* published by the Free Software Foundation.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*/
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/regulator/consumer.h>
#include <linux/version.h>
#include <linux/pm_runtime.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/bug.h>
#include <linux/mtk_gpu_utility.h>
#include "mtk_mfgsys.h"
#include <dt-bindings/clock/mt8173-clk.h>
//#include <mach/mt_typedefs.h>
#include <mach/mt_gpufreq.h>
#include "rgxdevice.h"
#include "osfunc.h"
#include "pvrsrv.h"
#include "rgxhwperf.h"
#include <trace/events/mtk_events.h>


static char *top_mfg_clk_name[] = {
#if (LINUX_VERSION_CODE > KERNEL_VERSION(3, 11, 0))
	"mfg_mem_in_sel",
	"mfg_axi_in_sel",
	"top_axi",
	"top_mem",
	"top_mfg",
#else
	"MT_CG_MFG_POWER",
	"MT_CG_MFG_AXI",
	"MT_CG_MFG_MEM",
	"MT_CG_MFG_G3D",
	"MT_CG_MFG_26M",
#endif
};

#define MAX_TOP_MFG_CLK ARRAY_SIZE(top_mfg_clk_name)

#define REG_MFG_AXI BIT(0)
#define REG_MFG_MEM BIT(1)
#define REG_MFG_G3D BIT(2)
#define REG_MFG_26M BIT(3)
#define REG_MFG_ALL (REG_MFG_AXI | REG_MFG_MEM | REG_MFG_G3D | REG_MFG_26M)

#define REG_MFG_CG_STA 0x00
#define REG_MFG_CG_SET 0x04
#define REG_MFG_CG_CLR 0x08

//#define MTK_CAL_POWER_INDEX 0
#undef MTK_CAL_POWER_INDEX

#define MTK_DEFER_DVFS_WORK_MS          10000
#define MTK_DVFS_SWITCH_INTERVAL_MS     50//16//100
#define MTK_SYS_BOOST_DURATION_MS       50
#define MTK_WAIT_FW_RESPONSE_TIMEOUT_US 5000
#define MTK_GPIO_REG_OFFSET             0x30
#define MTK_GPU_FREQ_ID_INVALID         0xFFFFFFFF
#define MTK_RGX_DEVICE_INDEX_INVALID    -1

static IMG_HANDLE g_hDVFSTimer = NULL;
static POS_LOCK ghDVFSLock = NULL;

#ifdef MTK_CAL_POWER_INDEX
static IMG_PVOID g_pvRegsKM = NULL;
static IMG_PVOID g_pvRegsBaseKM = NULL;
#endif

static IMG_BOOL g_bExit = IMG_TRUE;
static IMG_INT32 g_iSkipCount;
static IMG_UINT32 g_sys_dvfs_time_ms;

static IMG_UINT32 g_bottom_freq_id;
static IMG_UINT32 gpu_bottom_freq;
static IMG_UINT32 g_cust_boost_freq_id;
static IMG_UINT32 gpu_cust_boost_freq;
static IMG_UINT32 g_cust_upbound_freq_id;
static IMG_UINT32 gpu_cust_upbound_freq;

static IMG_UINT32 gpu_power = 0;
static IMG_UINT32 gpu_current = 0;
static IMG_UINT32 gpu_dvfs_enable;
static IMG_UINT32 boost_gpu_enable;
static IMG_UINT32 gpu_debug_enable;
static IMG_UINT32 gpu_dvfs_force_idle = 0;
static IMG_UINT32 gpu_dvfs_cb_force_idle = 0;

static IMG_UINT32 gpu_pre_loading = 0;
static IMG_UINT32 gpu_loading = 0;
static IMG_UINT32 gpu_block = 0;
static IMG_UINT32 gpu_idle = 0;
static IMG_UINT32 gpu_freq = 0;


static struct platform_device *sPVRLDMDev;
#define GET_MTK_MFG_BASE(x) (struct mtk_mfg_base *)(x->dev.platform_data)

static void mtk_mfg_set_clock_gating(void __iomem *reg)
{
#if (LINUX_VERSION_CODE > KERNEL_VERSION(3, 11, 0))
	writel(REG_MFG_ALL, reg + REG_MFG_CG_SET);
#endif
}

static void mtk_mfg_clr_clock_gating(void __iomem *reg)
{
#if (LINUX_VERSION_CODE > KERNEL_VERSION(3, 11, 0))
	writel(REG_MFG_ALL, reg + REG_MFG_CG_CLR);
#endif
}

static int mtk_mfg_prepare_clock(struct mtk_mfg_base *mfg_base)
{
	int i;

	for (i = 0; i < MAX_TOP_MFG_CLK; i++)
		clk_prepare(mfg_base->top_clk[i]);

	return PVRSRV_OK;
}

static int mtk_mfg_unprepare_clock(struct mtk_mfg_base *mfg_base)
{
	int i;

	for (i = MAX_TOP_MFG_CLK - 1; i >= 0; i--)
		clk_unprepare(mfg_base->top_clk[i]);

	return PVRSRV_OK;
}

static int mtk_mfg_enable_clock(struct mtk_mfg_base *mfg_base)
{
	int i;

	pm_runtime_get_sync(&mfg_base->pdev->dev);
	for (i = 0; i < MAX_TOP_MFG_CLK; i++)
		clk_enable(mfg_base->top_clk[i]);
	mtk_mfg_clr_clock_gating(mfg_base->reg_base);

	return PVRSRV_OK;
}

static int mtk_mfg_disable_clock(struct mtk_mfg_base *mfg_base)
{
	int i;

	mtk_mfg_set_clock_gating(mfg_base->reg_base);
	for (i = MAX_TOP_MFG_CLK - 1; i >= 0; i--)
		clk_disable(mfg_base->top_clk[i]);
	pm_runtime_put_sync(&mfg_base->pdev->dev);

	return PVRSRV_OK;
}

static void MTKEnableMfgClock(void)
{
	struct mtk_mfg_base *mfg_base = GET_MTK_MFG_BASE(sPVRLDMDev);
	mtk_mfg_enable_clock(mfg_base);
}


static void MTKDisableMfgClock(void)
{
	struct mtk_mfg_base *mfg_base = GET_MTK_MFG_BASE(sPVRLDMDev);
	mtk_mfg_disable_clock(mfg_base);
}

#if defined(MTK_ENABLE_HWAPM)
static void mtk_mfg_enable_hw_apm(struct mtk_mfg_base *mfg_base)
{
	writel(0x003c3d4d, mfg_base->reg_base + 0x24);
	writel(0x4d45440b, mfg_base->reg_base + 0x28);
	writel(0x7a710184, mfg_base->reg_base + 0xe0);
	writel(0x835f6856, mfg_base->reg_base + 0xe4);
	writel(0x002b0234, mfg_base->reg_base + 0xe8);
	writel(0x80000000, mfg_base->reg_base + 0xec);
	writel(0x08000000, mfg_base->reg_base + 0xa0);
}

static void mtk_mfg_disable_hw_apm(struct mtk_mfg_base *mfg_base)
{
	writel(0x00, mfg_base->reg_base + 0x24);
	writel(0x00, mfg_base->reg_base + 0x28);
	writel(0x00, mfg_base->reg_base + 0xe0);
	writel(0x00, mfg_base->reg_base + 0xe4);
	writel(0x00, mfg_base->reg_base + 0xe8);
	writel(0x00, mfg_base->reg_base + 0xec);
	writel(0x00, mfg_base->reg_base + 0xa0);
}
#endif

static int mtk_mfg_get_opp_table(struct platform_device *pdev,
				 struct mtk_mfg_base *mfg_base)
{
	const struct property *prop;
	int i, nr;
	const __be32 *val;

	prop = of_find_property(pdev->dev.of_node, "operating-points", NULL);
	if (!prop) {
		dev_err(&pdev->dev, "failed to fail operating-points\n");
		return -ENODEV;
	}

	if (!prop->value) {
		dev_err(&pdev->dev, "failed to get fv array data\n");
		return -ENODATA;
	}

	/*
	 * Each OPP is a set of tuples consisting of frequency and
	 * voltage like <freq-kHz vol-uV>.
	 */
	nr = prop->length / sizeof(u32);
	if (nr % 2) {
		dev_err(&pdev->dev, "Invalid OPP list\n");
		return -EINVAL;
	}

	mfg_base->fv_table_length = nr / 2;
	mfg_base->fv_table = devm_kcalloc(&pdev->dev,
					  mfg_base->fv_table_length,
					  sizeof(*mfg_base->fv_table),
					  GFP_KERNEL);
	if (!mfg_base->fv_table)
		return -ENOMEM;

	val = prop->value;

	for (i = 0; i < mfg_base->fv_table_length; ++i) {
		u32 freq = be32_to_cpup(val++);
		u32 volt = be32_to_cpup(val++);

		mfg_base->fv_table[i].freq = freq;
		mfg_base->fv_table[i].volt = volt;

		dev_info(&pdev->dev, "freq:%d kHz volt:%d uV\n", freq, volt);
	}

	return 0;
}


static int mtk_mfg_bind_device_resource(struct platform_device *pdev,
				 struct mtk_mfg_base *mfg_base)
{
	int i, err;
	int len = sizeof(struct clk *) * MAX_TOP_MFG_CLK;

	mfg_base->top_clk = devm_kzalloc(&pdev->dev, len, GFP_KERNEL);
	if (!mfg_base->top_clk)
		return -ENOMEM;

	mfg_base->reg_base = of_iomap(pdev->dev.of_node, 1);
	if (!mfg_base->reg_base) {
		mtk_mfg_debug("Unable to ioremap registers pdev %p\n", pdev);
		return -ENOMEM;
	}

#ifndef MTK_MFG_DVFS
	mfg_base->mmpll = devm_clk_get(&pdev->dev, "mmpll_clk");
	if (IS_ERR(mfg_base->mmpll)) {
		err = PTR_ERR(mfg_base->mmpll);
		dev_err(&pdev->dev, "devm_clk_get mmpll_clk failed !!!\n");
		goto err_iounmap_reg_base;
	}
#endif

	for (i = 0; i < MAX_TOP_MFG_CLK; i++) {
		mfg_base->top_clk[i] = devm_clk_get(&pdev->dev,
						    top_mfg_clk_name[i]);
		if (IS_ERR(mfg_base->top_clk[i])) {
			err = PTR_ERR(mfg_base->top_clk[i]);
			dev_err(&pdev->dev, "devm_clk_get %s failed !!!\n",
				top_mfg_clk_name[i]);
			goto err_iounmap_reg_base;
		}
	}

#ifndef MTK_MFG_DVFS
	mfg_base->vgpu = devm_regulator_get(&pdev->dev, "mfgsys-power");
	if (IS_ERR(mfg_base->vgpu)) {
		err = PTR_ERR(mfg_base->vgpu);
		goto err_iounmap_reg_base;
	}

	err = regulator_enable(mfg_base->vgpu);
	if (err != 0) {
		dev_err(&pdev->dev, "failed to enable regulator vgpu\n");
		goto err_iounmap_reg_base;
	}
	mtk_mfg_get_opp_table(pdev, mfg_base);
#endif

	pm_runtime_enable(&pdev->dev);
	mfg_base->pdev = pdev;

	return 0;

err_iounmap_reg_base:
	iounmap(mfg_base->reg_base);
	return err;
}

static int mtk_mfg_unbind_device_resource(struct platform_device *pdev,
				   struct mtk_mfg_base *mfg_base)
{
	iounmap(mfg_base->reg_base);
#ifndef MTK_MFG_DVFS
	regulator_disable(mfg_base->vgpu);
#endif
	pm_runtime_disable(&pdev->dev);
	mfg_base->pdev = NULL;

	return 0;
}

void MTKSysSetInitialPowerState(void)
{
	mtk_mfg_debug("MTKSysSetInitialPowerState ---\n");
}

void MTKSysRestoreInitialPowerState(void)
{
	mtk_mfg_debug("MTKSysRestoreInitialPowerState ---\n");
}

PVRSRV_ERROR MTKDevPrePowerState(PVRSRV_DEV_POWER_STATE eNewPowerState,
				    PVRSRV_DEV_POWER_STATE eCurrentPowerState,
				    IMG_BOOL bForced)
{
	struct mtk_mfg_base *mfg_base = GET_MTK_MFG_BASE(sPVRLDMDev);

	mtk_mfg_debug("MTKSysDevPrePowerState (%d->%d), bForced = %d\n",
		      eCurrentPowerState, eNewPowerState, bForced);

	mutex_lock(&mfg_base->set_power_state);

	if ((PVRSRV_DEV_POWER_STATE_OFF == eNewPowerState) &&
	    (PVRSRV_DEV_POWER_STATE_ON == eCurrentPowerState)) {
#if !defined(PVR_DVFS)
		mtk_mfg_gpu_calc_power_off(mfg_base);
#endif
#if defined(MTK_ENABLE_HWAPM)
		mtk_mfg_disable_hw_apm(mfg_base);
#endif
		mtk_mfg_disable_clock(mfg_base);
		mfg_base->power_on = false;
	}

	mutex_unlock(&mfg_base->set_power_state);
	return PVRSRV_OK;
}

PVRSRV_ERROR MTKDevPostPowerState(PVRSRV_DEV_POWER_STATE eNewPowerState,
				     PVRSRV_DEV_POWER_STATE eCurrentPowerState,
				     IMG_BOOL bForced)
{
	struct mtk_mfg_base *mfg_base = GET_MTK_MFG_BASE(sPVRLDMDev);

	mtk_mfg_debug("MTKSysDevPostPowerState (%d->%d)\n",
		      eCurrentPowerState, eNewPowerState);

	mutex_lock(&mfg_base->set_power_state);

	if ((PVRSRV_DEV_POWER_STATE_ON == eNewPowerState) &&
	    (PVRSRV_DEV_POWER_STATE_OFF == eCurrentPowerState)) {
		mtk_mfg_enable_clock(mfg_base);
#if defined(MTK_ENABLE_HWAPM)
		mtk_mfg_enable_hw_apm(mfg_base);
#endif
#if !defined(PVR_DVFS)
		mtk_mfg_gpu_calc_power_on(mfg_base);
#endif
		mfg_base->power_on = true;
	}

	mutex_unlock(&mfg_base->set_power_state);

	return PVRSRV_OK;
}

PVRSRV_ERROR MTKSystemPrePowerState(PVRSRV_SYS_POWER_STATE eNewPowerState)
{
	int err = 0;
	struct mtk_mfg_base *mfg_base = GET_MTK_MFG_BASE(sPVRLDMDev);

	pr_err("MTKSystemPrePowerState(%d) eNewPowerState %d\n",
		      mfg_base->power_on, eNewPowerState);
#ifndef MTK_MFG_DVFS
	/* turn off regulator for power saving ~30mw */
	if (eNewPowerState == PVRSRV_SYS_POWER_STATE_OFF)
		err = regulator_disable(mfg_base->vgpu);
	else if (eNewPowerState == PVRSRV_SYS_POWER_STATE_ON)
		err = regulator_enable(mfg_base->vgpu);

	if (err != 0) {
		pr_err("failed to %s regulator vgpu\n",
		       ((eNewPowerState == PVRSRV_SYS_POWER_STATE_OFF)
		       ? "disable" : "enable"));
		return PVRSRV_ERROR_INVALID_PARAMS;
	}
#endif
	return PVRSRV_OK;
}

PVRSRV_ERROR MTKSystemPostPowerState(PVRSRV_SYS_POWER_STATE eNewPowerState)
{
	mtk_mfg_debug("MTKSystemPostPowerState eNewPowerState %d\n",
		      eNewPowerState);

	return PVRSRV_OK;
}

int MTKMFGBaseInit(struct platform_device *pdev)
{
	int err;
	struct mtk_mfg_base *mfg_base;

	mtk_mfg_debug("MTKMFGSystemInit Begin\n");

	mfg_base = devm_kzalloc(&pdev->dev, sizeof(*mfg_base), GFP_KERNEL);
	if (!mfg_base)
		return -ENOMEM;


	err = mtk_mfg_bind_device_resource(pdev, mfg_base);
	if (err != 0)
		return err;

	mutex_init(&mfg_base->set_power_state);

	mtk_mfg_prepare_clock(mfg_base);
#if !defined(PVR_DVFS) && !defined(MTK_MFG_DVFS)
	mtk_mfg_gpu_dvfs_init(mfg_base);
#endif

	/* attach mfg_base to pdev->dev.platform_data */
	pdev->dev.platform_data = mfg_base;
	sPVRLDMDev = pdev;

	mtk_mfg_debug("MTKMFGSystemInit End\n");
	return 0;
}

int MTKMFGBaseDeInit(struct platform_device *pdev)
{
	struct mtk_mfg_base *mfg_base = GET_MTK_MFG_BASE(sPVRLDMDev);

	if (pdev != sPVRLDMDev) {
		dev_err(&pdev->dev, "release %p != %p\n", pdev, sPVRLDMDev);
		return 0;
	}

	mtk_mfg_unprepare_clock(mfg_base);
#if !defined(PVR_DVFS) && !defined(MTK_MFG_DVFS)
	mtk_mfg_gpu_dvfs_deinit(mfg_base);
#endif

	mtk_mfg_unbind_device_resource(pdev, mfg_base);
	return 0;
}

static PVRSRV_DEVICE_NODE* MTKGetRGXDevNode(IMG_VOID)
{
    PVRSRV_DATA *psPVRSRVData = PVRSRVGetPVRSRVData();
    IMG_UINT32 i;
    for (i = 0; i < psPVRSRVData->ui32RegisteredDevices; i++)
    {
        PVRSRV_DEVICE_NODE* psDeviceNode = psPVRSRVData->apsRegisteredDevNodes[i];
        if (psDeviceNode && psDeviceNode->psDevConfig &&
            psDeviceNode->psDevConfig->eDeviceType == PVRSRV_DEVICE_TYPE_RGX)
        {
            return psDeviceNode;
        }
    }
    return NULL;
}

static IMG_UINT32 MTKGetRGXDevIdx(IMG_VOID)
{
    static IMG_UINT32 ms_ui32RGXDevIdx = MTK_RGX_DEVICE_INDEX_INVALID;
    if (MTK_RGX_DEVICE_INDEX_INVALID == ms_ui32RGXDevIdx)
    {
        PVRSRV_DATA *psPVRSRVData = PVRSRVGetPVRSRVData();
        IMG_UINT32 i;
        for (i = 0; i < psPVRSRVData->ui32RegisteredDevices; i++)
        {
            PVRSRV_DEVICE_NODE* psDeviceNode = psPVRSRVData->apsRegisteredDevNodes[i];
            if (psDeviceNode && psDeviceNode->psDevConfig &&
                psDeviceNode->psDevConfig->eDeviceType == PVRSRV_DEVICE_TYPE_RGX)
            {
                ms_ui32RGXDevIdx = i;
                break;
            }
        }
    }
    return ms_ui32RGXDevIdx;
}

static IMG_VOID MTKWriteBackFreqToRGX(IMG_UINT32 ui32DeviceIndex, IMG_UINT32 ui32NewFreq)
{
    PVRSRV_DATA *psPVRSRVData = PVRSRVGetPVRSRVData();
    PVRSRV_DEVICE_NODE* psDeviceNode = psPVRSRVData->apsRegisteredDevNodes[ui32DeviceIndex];
    RGX_DATA* psRGXData = (RGX_DATA*)psDeviceNode->psDevConfig->hDevData;
    psRGXData->psRGXTimingInfo->ui32CoreClockSpeed = ui32NewFreq * 1000; /* kHz to Hz write to RGX as the same unit */
}

static IMG_BOOL MTKDoGpuDVFS(IMG_UINT32 ui32NewFreqID, IMG_BOOL bIdleDevice)
{
    PVRSRV_ERROR eResult;
    IMG_UINT32 ui32RGXDevIdx;

    if (mt_gpufreq_dvfs_ready() == false)
    {
        return IMG_FALSE;
    }

    // bottom bound
    if (ui32NewFreqID > g_bottom_freq_id)
    {
        ui32NewFreqID = g_bottom_freq_id;
    }
    if (ui32NewFreqID > g_cust_boost_freq_id)
    {
        ui32NewFreqID = g_cust_boost_freq_id;
    }

    // up bound
    if (ui32NewFreqID < g_cust_upbound_freq_id)
    {
        ui32NewFreqID = g_cust_upbound_freq_id;
    }

    // thermal power limit
    if (ui32NewFreqID < mt_gpufreq_get_thermal_limit_index())
    {
        ui32NewFreqID = mt_gpufreq_get_thermal_limit_index();
    }

    // no change
    if (ui32NewFreqID == mt_gpufreq_get_cur_freq_index())
    {
        return IMG_FALSE;
    }

    ui32RGXDevIdx = MTKGetRGXDevIdx();
    if (MTK_RGX_DEVICE_INDEX_INVALID == ui32RGXDevIdx)
    {
        return IMG_FALSE;
    }

    eResult = PVRSRVDevicePreClockSpeedChange(ui32RGXDevIdx, bIdleDevice, (IMG_VOID*)NULL);
    if ((PVRSRV_OK == eResult) || (PVRSRV_ERROR_RETRY == eResult))
    {
        unsigned int ui32GPUFreq;
        unsigned int ui32CurFreqID;
        PVRSRV_DEV_POWER_STATE ePowerState;

        PVRSRVGetDevicePowerState(ui32RGXDevIdx, &ePowerState);
        if (ePowerState != PVRSRV_DEV_POWER_STATE_ON)
        {
            MTKEnableMfgClock();
        }

        mt_gpufreq_target(ui32NewFreqID);
        ui32CurFreqID = mt_gpufreq_get_cur_freq_index();
        ui32GPUFreq = mt_gpufreq_get_frequency_by_level(ui32CurFreqID);
        gpu_freq = ui32GPUFreq;
        MTKWriteBackFreqToRGX(ui32RGXDevIdx, ui32GPUFreq);

        if (ePowerState != PVRSRV_DEV_POWER_STATE_ON)
        {
            MTKDisableMfgClock();
        }

        if (PVRSRV_OK == eResult)
        {
            PVRSRVDevicePostClockSpeedChange(ui32RGXDevIdx, bIdleDevice, (IMG_VOID*)NULL);
        }

        return IMG_TRUE;
    }

    return IMG_FALSE;
}

static void MTKFreqInputBoostCB(unsigned int ui32BoostFreqID)
{
    if (0 < g_iSkipCount)
    {
        return;
    }

    if (boost_gpu_enable == 0)
    {
        return;
    }

    OSLockAcquire(ghDVFSLock);

    if (ui32BoostFreqID < mt_gpufreq_get_cur_freq_index())
    {
        if (MTKDoGpuDVFS(ui32BoostFreqID, gpu_dvfs_cb_force_idle == 0 ? IMG_FALSE : IMG_TRUE))
        {
            g_sys_dvfs_time_ms = OSClockms();
        }
    }

    OSLockRelease(ghDVFSLock);

}

static void MTKFreqPowerLimitCB(unsigned int ui32LimitFreqID)
{
    if (0 < g_iSkipCount)
    {
        return;
    }

    OSLockAcquire(ghDVFSLock);

    if (ui32LimitFreqID > mt_gpufreq_get_cur_freq_index())
    {
        if (MTKDoGpuDVFS(ui32LimitFreqID, gpu_dvfs_cb_force_idle == 0 ? IMG_FALSE : IMG_TRUE))
        {
            g_sys_dvfs_time_ms = OSClockms();
        }
    }

    OSLockRelease(ghDVFSLock);
}

#ifdef MTK_CAL_POWER_INDEX
static IMG_VOID MTKStartPowerIndex(IMG_VOID)
{

    struct mtk_mfg_base *mfg_base = GET_MTK_MFG_BASE(sPVRLDMDev);
    writel(0x1, mfg_base->reg_base + 0x6320);
}

static IMG_VOID MTKReStartPowerIndex(IMG_VOID)
{
    struct mtk_mfg_base *mfg_base = GET_MTK_MFG_BASE(sPVRLDMDev);
    writel(0x1, mfg_base->reg_base + 0x6320);
    
}

static IMG_VOID MTKStopPowerIndex(IMG_VOID)
{

    struct mtk_mfg_base *mfg_base = GET_MTK_MFG_BASE(sPVRLDMDev);
    writel(0x0, mfg_base->reg_base + 0x6320);
}

static IMG_UINT32 MTKCalPowerIndex(IMG_VOID)
{
    IMG_UINT32 ui32State, ui32Result;
    PVRSRV_DEV_POWER_STATE  ePowerState;
    IMG_BOOL bTimeout;
    IMG_UINT32 u32Deadline;
    struct mtk_mfg_base *mfg_base = GET_MTK_MFG_BASE(sPVRLDMDev);
    IMG_PVOID pvGPIO_REG = mfg_base->reg_base + (uintptr_t)MTK_GPIO_REG_OFFSET;
    IMG_PVOID pvPOWER_ESTIMATE_RESULT;

    if (!mfg_base->reg_base && PVRSRVGetInitServerState(PVRSRV_INIT_SERVER_SUCCESSFUL))
    {
        MTKStartPowerIndex();
    }

    pvPOWER_ESTIMATE_RESULT = mfg_base->reg_base + (uintptr_t)0x6328;

    if (PVRSRVPowerLock() != PVRSRV_OK)
    {
        return 0;
    }

	PVRSRVGetDevicePowerState(MTKGetRGXDevIdx(), &ePowerState);
    if (ePowerState != PVRSRV_DEV_POWER_STATE_ON)
	{
		PVRSRVPowerUnlock();
		return 0;
	}

    //writes 1 to GPIO_INPUT_REQ, bit[0]
    DRV_WriteReg32(pvGPIO_REG, DRV_Reg32(pvGPIO_REG) | 0x1);

    //wait for 1 in GPIO_OUTPUT_REQ, bit[16]
    bTimeout = IMG_TRUE;
    u32Deadline = OSClockus() + MTK_WAIT_FW_RESPONSE_TIMEOUT_US;
    while(OSClockus() < u32Deadline)
    {
        if (0x10000 & DRV_Reg32(pvGPIO_REG))
        {
            bTimeout = IMG_FALSE;
            break;
        }
    }

    //writes 0 to GPIO_INPUT_REQ, bit[0]
    DRV_WriteReg32(pvGPIO_REG, DRV_Reg32(pvGPIO_REG) & (~0x1));
    if (bTimeout)
    {
        PVRSRVPowerUnlock();
        return 0;
    }

    //read GPIO_OUTPUT_DATA, bit[24]
    ui32State = DRV_Reg32(pvGPIO_REG) >> 24;

    //read POWER_ESTIMATE_RESULT
    ui32Result = DRV_Reg32(pvPOWER_ESTIMATE_RESULT);

    //writes 1 to GPIO_OUTPUT_ACK, bit[17]
    DRV_WriteReg32(pvGPIO_REG, DRV_Reg32(pvGPIO_REG)|0x20000);

    //wait for 0 in GPIO_OUTPUT_REG, bit[16]
    bTimeout = IMG_TRUE;
    u32Deadline = OSClockus() + MTK_WAIT_FW_RESPONSE_TIMEOUT_US;
    while(OSClockus() < u32Deadline)
    {
        if (!(0x10000 & DRV_Reg32(pvGPIO_REG)))
        {
            bTimeout = IMG_FALSE;
            break;
        }
    }

    //writes 0 to GPIO_OUTPUT_ACK, bit[17]
    DRV_WriteReg32(pvGPIO_REG, DRV_Reg32(pvGPIO_REG) & (~0x20000));
    if (bTimeout)
    {
        PVRSRVPowerUnlock();
        return 0;
    }

    MTKReStartPowerIndex();

    PVRSRVPowerUnlock();

    return (1 == ui32State) ? ui32Result : 0;
}
#endif

static IMG_VOID MTKCalGpuLoading(unsigned int* pui32Loading , unsigned int* pui32Block,unsigned int* pui32Idle)
{
    PVRSRV_DEVICE_NODE* psDevNode;
    PVRSRV_RGXDEV_INFO* psDevInfo;

    psDevNode = MTKGetRGXDevNode();
    if (!psDevNode)
    {
        return;
    }
    psDevInfo = psDevNode->pvDevice;
    if (psDevInfo && psDevInfo->pfnGetGpuUtilStats)
    {
        RGXFWIF_GPU_UTIL_STATS sGpuUtilStats = {0};
        sGpuUtilStats = psDevInfo->pfnGetGpuUtilStats(psDevInfo->psDeviceNode);
        if (sGpuUtilStats.bValid)
        {
#if 0
            PVR_DPF((PVR_DBG_ERROR,"Loading: A(%d), I(%d), B(%d)",
                sGpuUtilStats.ui32GpuStatActive, sGpuUtilStats.ui32GpuStatIdle, sGpuUtilStats.ui32GpuStatBlocked));
#endif

            *pui32Loading = sGpuUtilStats.ui32GpuStatActiveHigh/100;
            *pui32Block = sGpuUtilStats.ui32GpuStatBlocked/100;
            *pui32Idle = sGpuUtilStats.ui32GpuStatIdle/100;
        }
    }
}

static IMG_BOOL MTKGpuDVFSPolicy(IMG_UINT32 ui32GPULoading, unsigned int* pui32NewFreqID)
{
    int i32MaxLevel = (int)(mt_gpufreq_get_dvfs_table_num() - 1);
    int i32CurFreqID = (int)mt_gpufreq_get_cur_freq_index();
    int i32NewFreqID = i32CurFreqID;

    if (ui32GPULoading >= 99)
    {
        i32NewFreqID = 0;
    }
    else if (ui32GPULoading <= 1)
    {
        i32NewFreqID = i32MaxLevel;
    }
    else if (ui32GPULoading >= 85)
    {
        i32NewFreqID -= 2;
    }
    else if (ui32GPULoading <= 30)
    {
        i32NewFreqID += 2;
    }
    else if (ui32GPULoading >= 70)
    {
        i32NewFreqID -= 1;
    }
    else if (ui32GPULoading <= 50)
    {
        i32NewFreqID += 1;
    }

    if (i32NewFreqID < i32CurFreqID)
    {
        if (gpu_pre_loading * 17 / 10 < ui32GPULoading)
        {
            i32NewFreqID -= 1;
        }
    }
    else if (i32NewFreqID > i32CurFreqID)
    {
        if (ui32GPULoading * 17 / 10 < gpu_pre_loading)
        {
            i32NewFreqID += 1;
        }
    }

    if (i32NewFreqID > i32MaxLevel)
    {
        i32NewFreqID = i32MaxLevel;
    }
    else if (i32NewFreqID < 0)
    {
        i32NewFreqID = 0;
    }

    if (i32NewFreqID != i32CurFreqID)
    {
        
        *pui32NewFreqID = (unsigned int)i32NewFreqID;
        return IMG_TRUE;
    }
    
    return IMG_FALSE;
}

static IMG_VOID MTKDVFSTimerFuncCB(IMG_PVOID pvData)
{
    IMG_UINT32 x1, x2, x3, y1, y2, y3;

    int i32MaxLevel = (int)(mt_gpufreq_get_dvfs_table_num() - 1);
    int i32CurFreqID = (int)mt_gpufreq_get_cur_freq_index();

    x1=10000;
    x2=30000;
    x3=50000;
    y1=50;
    y2=430;
    y3=750;

    if (gpu_debug_enable)
    {
        PVR_DPF((PVR_DBG_ERROR, "MTKDVFSTimerFuncCB"));
    }

    if (0 == gpu_dvfs_enable)
    {
        gpu_power = 0;
        gpu_current = 0;
        gpu_loading = 0;
        gpu_block= 0;
        gpu_idle = 0;
        return;
    }

    if (g_iSkipCount > 0)
    {
        gpu_power = 0;
        gpu_current = 0;
        gpu_loading = 0;
        gpu_block= 0;
        gpu_idle = 0;
        g_iSkipCount -= 1;
    }
    else if ((!g_bExit) || (i32CurFreqID < i32MaxLevel))
    {
        IMG_UINT32 ui32NewFreqID;

        // calculate power index
#ifdef MTK_CAL_POWER_INDEX
        gpu_power = MTKCalPowerIndex();
        //mapping power index to power current
        if (gpu_power < x1)
        {
            gpu_current = y1;
        }
        else if (gpu_power < x2)
        {
            gpu_current = y1+((gpu_power-x1)*(y2-y1)/(x2-x1));
        }
        else if (gpu_power < x3)
        {
            gpu_current = y2+((gpu_power-x2)*(y3-y2) / (x3-x2));
        }
        else
        {
            gpu_current =y3;
        }

#else
        gpu_power = 0;
	gpu_current = 0;
#endif

        MTKCalGpuLoading(&gpu_loading, &gpu_block, &gpu_idle);

        OSLockAcquire(ghDVFSLock);

        // check system boost duration
        if ((g_sys_dvfs_time_ms > 0) && (OSClockms() - g_sys_dvfs_time_ms < MTK_SYS_BOOST_DURATION_MS))
        {
            OSLockRelease(ghDVFSLock);
            return;
        }
        else
        {
            g_sys_dvfs_time_ms = 0;
        }

        // do gpu dvfs
        if (MTKGpuDVFSPolicy(gpu_loading, &ui32NewFreqID))
        {
            MTKDoGpuDVFS(ui32NewFreqID, gpu_dvfs_force_idle == 0 ? IMG_FALSE : IMG_TRUE);
        }

        gpu_pre_loading = gpu_loading;

        OSLockRelease(ghDVFSLock);
    }
}

void MTKMFGEnableDVFSTimer(bool bEnable)
{
	/* OSEnableTimer() and OSDisableTimer() should be called sequentially, following call will lead to assertion.
	   OSEnableTimer();
	   OSEnableTimer();
	   OSDisableTimer();
	   ...
	   bPreEnable is to avoid such scenario */
	static bool bPreEnable = false;

	if (gpu_debug_enable)
	{
		PVR_DPF((PVR_DBG_ERROR, "MTKMFGEnableDVFSTimer: %s (%s)",
			bEnable ? "yes" : "no", bPreEnable ? "yes" : "no"));
	}

	if (g_hDVFSTimer)
	{
		if (bEnable == true && bPreEnable == false)
		{
			OSEnableTimer(g_hDVFSTimer);
			bPreEnable = true;
		}
		else if (bEnable == false && bPreEnable == true)
		{
			OSDisableTimer(g_hDVFSTimer);
			bPreEnable = false;
		}
		else if (gpu_debug_enable)
		{
			BUG();
		}
	}
}


static void MTKBoostGpuFreq(void)
{
    if (gpu_debug_enable)
    {
        PVR_DPF((PVR_DBG_ERROR, "MTKBoostGpuFreq"));
    }
    MTKFreqInputBoostCB(0);
}

static void MTKSetBottomGPUFreq(unsigned int ui32FreqLevel)
{
    unsigned int ui32MaxLevel;

    if (gpu_debug_enable)
    {
        PVR_DPF((PVR_DBG_ERROR, "MTKSetBottomGPUFreq: freq = %d", ui32FreqLevel));
    }

    ui32MaxLevel = mt_gpufreq_get_dvfs_table_num() - 1;
    if (ui32MaxLevel < ui32FreqLevel)
    {
        ui32FreqLevel = ui32MaxLevel;
    }

    OSLockAcquire(ghDVFSLock);

    // 0 => The highest frequency
    // table_num - 1 => The lowest frequency
    g_bottom_freq_id = ui32MaxLevel - ui32FreqLevel;
    gpu_bottom_freq = mt_gpufreq_get_frequency_by_level(g_bottom_freq_id);

    if (g_bottom_freq_id < mt_gpufreq_get_cur_freq_index())
    {
        MTKDoGpuDVFS(g_bottom_freq_id, gpu_dvfs_cb_force_idle == 0 ? IMG_FALSE : IMG_TRUE);
    }
     
    OSLockRelease(ghDVFSLock);

}

static unsigned int MTKCustomGetGpuFreqLevelCount(void)
{
    return mt_gpufreq_get_dvfs_table_num();
}

static void MTKCustomBoostGpuFreq(unsigned int ui32FreqLevel)
{
    unsigned int ui32MaxLevel;

    if (gpu_debug_enable)
    {
        PVR_DPF((PVR_DBG_ERROR, "MTKCustomBoostGpuFreq: freq = %d", ui32FreqLevel));
    }

    ui32MaxLevel = mt_gpufreq_get_dvfs_table_num() - 1;
    if (ui32MaxLevel < ui32FreqLevel)
    {
        ui32FreqLevel = ui32MaxLevel;
    }

    OSLockAcquire(ghDVFSLock);

    // 0 => The highest frequency
    // table_num - 1 => The lowest frequency
    g_cust_boost_freq_id = ui32MaxLevel - ui32FreqLevel;
    gpu_cust_boost_freq = mt_gpufreq_get_frequency_by_level(g_cust_boost_freq_id);

    if (g_cust_boost_freq_id < mt_gpufreq_get_cur_freq_index())
    {
        MTKDoGpuDVFS(g_cust_boost_freq_id, gpu_dvfs_cb_force_idle == 0 ? IMG_FALSE : IMG_TRUE);
    }

    OSLockRelease(ghDVFSLock);
}

static void MTKCustomUpBoundGpuFreq(unsigned int ui32FreqLevel)
{
    unsigned int ui32MaxLevel;

    if (gpu_debug_enable)
    {
        PVR_DPF((PVR_DBG_ERROR, "MTKCustomUpBoundGpuFreq: freq = %d", ui32FreqLevel));
    }

    ui32MaxLevel = mt_gpufreq_get_dvfs_table_num() - 1;
    if (ui32MaxLevel < ui32FreqLevel)
    {
        ui32FreqLevel = ui32MaxLevel;
    }

    OSLockAcquire(ghDVFSLock);

    // 0 => The highest frequency
    // table_num - 1 => The lowest frequency
    g_cust_upbound_freq_id = ui32MaxLevel - ui32FreqLevel;
    gpu_cust_upbound_freq = mt_gpufreq_get_frequency_by_level(g_cust_upbound_freq_id);

    if (g_cust_upbound_freq_id > mt_gpufreq_get_cur_freq_index())
    {
        MTKDoGpuDVFS(g_cust_upbound_freq_id, gpu_dvfs_cb_force_idle == 0 ? IMG_FALSE : IMG_TRUE);
    }
     
    OSLockRelease(ghDVFSLock);
}

static unsigned int MTKGetCustomBoostGpuFreq(void)
{
    unsigned int ui32MaxLevel = mt_gpufreq_get_dvfs_table_num() - 1;
    return ui32MaxLevel - g_cust_boost_freq_id;
}

static unsigned int MTKGetCustomUpBoundGpuFreq(void)
{
    unsigned int ui32MaxLevel = mt_gpufreq_get_dvfs_table_num() - 1;
    return ui32MaxLevel - g_cust_upbound_freq_id;
}

static IMG_UINT32 MTKGetGpuLoading(IMG_VOID)
{
    return gpu_loading;
}

static IMG_UINT32 MTKGetGpuBlock(IMG_VOID)
{
    return gpu_block;
}

static IMG_UINT32 MTKGetGpuIdle(IMG_VOID)
{
    return gpu_idle;
}

static IMG_UINT32 MTKGetPowerIndex(IMG_VOID)
{
    return gpu_current;    //gpu_power;
}

int MTKMFGSystemInit(void)
{
	PVRSRV_ERROR error;

	error = OSLockCreate(&ghDVFSLock, LOCK_TYPE_PASSIVE);
	if (error != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "Create DVFS Lock Failed"));
		return error;
	}

	g_iSkipCount = MTK_DEFER_DVFS_WORK_MS / MTK_DVFS_SWITCH_INTERVAL_MS;

	g_hDVFSTimer = OSAddTimer(MTKDVFSTimerFuncCB, (IMG_VOID *)NULL, MTK_DVFS_SWITCH_INTERVAL_MS);
	if(!g_hDVFSTimer)
	{
		OSLockDestroy(ghDVFSLock);
		PVR_DPF((PVR_DBG_ERROR, "Create DVFS Timer Failed"));
		return PVRSRV_ERROR_OUT_OF_MEMORY;
	}

	MTKMFGEnableDVFSTimer(true);

	#ifdef MTK_GPU_DVFS
	gpu_dvfs_enable = 1;
	#else
	gpu_dvfs_enable = 0;
	#endif

	boost_gpu_enable = 1;

	g_sys_dvfs_time_ms = 0;

	g_bottom_freq_id = mt_gpufreq_get_dvfs_table_num() - 1;
	gpu_bottom_freq = mt_gpufreq_get_frequency_by_level(g_bottom_freq_id);

	g_cust_boost_freq_id = mt_gpufreq_get_dvfs_table_num() - 1;
	gpu_cust_boost_freq = mt_gpufreq_get_frequency_by_level(g_cust_boost_freq_id);

	g_cust_upbound_freq_id = 0;
	gpu_cust_upbound_freq = mt_gpufreq_get_frequency_by_level(g_cust_upbound_freq_id);

	gpu_debug_enable = 0;

	mt_gpufreq_mfgclock_notify_registerCB(MTKEnableMfgClock, MTKDisableMfgClock);

	mt_gpufreq_input_boost_notify_registerCB(MTKFreqInputBoostCB);
	mt_gpufreq_power_limit_notify_registerCB(MTKFreqPowerLimitCB);

	mtk_boost_gpu_freq_fp = MTKBoostGpuFreq;

	mtk_set_bottom_gpu_freq_fp = MTKSetBottomGPUFreq;

	mtk_custom_get_gpu_freq_level_count_fp = MTKCustomGetGpuFreqLevelCount;

	mtk_custom_boost_gpu_freq_fp = MTKCustomBoostGpuFreq;

	mtk_custom_upbound_gpu_freq_fp = MTKCustomUpBoundGpuFreq;

	mtk_get_custom_boost_gpu_freq_fp = MTKGetCustomBoostGpuFreq;

	mtk_get_custom_upbound_gpu_freq_fp = MTKGetCustomUpBoundGpuFreq;

	/*mtk_enable_gpu_dvfs_timer_fp = MTKMFGEnableDVFSTimer; */

	mtk_get_gpu_power_loading_fp = MTKGetPowerIndex;

	mtk_get_gpu_loading_fp = MTKGetGpuLoading;
	mtk_get_gpu_block_fp = MTKGetGpuBlock;
	mtk_get_gpu_idle_fp = MTKGetGpuIdle;


	mtk_mfg_debug("MTKMFGSystemInit\n");
	return PVRSRV_OK;
}

int MTKMFGSystemDeInit(void)
{
	mtk_mfg_debug("MTKMFGSystemDeInit\n");
	g_bExit = IMG_TRUE;

	if(g_hDVFSTimer)
	{
		MTKMFGEnableDVFSTimer(false);
		OSRemoveTimer(g_hDVFSTimer);
		g_hDVFSTimer = IMG_NULL;
	}

	if (ghDVFSLock)
	{
		OSLockDestroy(ghDVFSLock);
		ghDVFSLock = NULL;
	}


	return PVRSRV_OK;
}

module_param(gpu_loading, uint, 0644);
module_param(gpu_block, uint, 0644);
module_param(gpu_idle, uint, 0644);
module_param(gpu_power, uint, 0644);
module_param(gpu_dvfs_enable, uint, 0644);
module_param(boost_gpu_enable, uint, 0644);
module_param(gpu_debug_enable, uint, 0644);
module_param(gpu_dvfs_force_idle, uint, 0644);
module_param(gpu_dvfs_cb_force_idle, uint, 0644);
module_param(gpu_bottom_freq, uint, 0644);
module_param(gpu_cust_boost_freq, uint, 0644);
module_param(gpu_cust_upbound_freq, uint, 0644);
module_param(gpu_freq, uint, 0644);
