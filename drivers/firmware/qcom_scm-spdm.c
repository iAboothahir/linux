// SPDX-License-Identifier: GPL-2.0
/*
 * Qualcomm System Performance Dynamic Monitoring (SPDM)
 * secure world communication
 *
 * Copyright (C) 2021, AngeloGioacchino Del Regno
 *		       <angelogioacchino.delregno@somainline.org>
 */

#include <linux/io.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/qcom_scm.h>
#include <linux/arm-smccc.h>
#include <linux/dma-mapping.h>

#include "qcom_scm.h"

#define QCOM_SPDM_MAX_VERSION	0x20000
#define QCOM_SPDM_NARGS		6
#define QCOM_SPDM_VOTE_MULTI	2

struct qcom_scm_spdm {
	struct device *dev;
	bool is_smc;
} *__spdm;

/**
 * qcom_scm_spdm_call() - Invoke a SPDM syscall in the secure world
 * @dev:	device
 * @desc:	Descriptor structure containing arguments and return values
 * @res:	Structure containing results from SMC/HVC call
 *
 * Sends a Security Protocol and Data Model compliant command to the SCM
 * and waits for the command to finish processing.
 * This should *only* be called in pre-emptible context.
 */
static int qcom_scm_spdm_call(u64 *args, int num_args)
{
	int i, ret;
	struct qcom_scm_desc desc = {
		.svc = QCOM_SCM_SVC_SPDM,
		.cmd = QCOM_SCM_SPDM_CMD,
		.arginfo = QCOM_SCM_ARGS(QCOM_SPDM_NARGS),
		.owner = ARM_SMCCC_OWNER_SIP,
	};
	struct qcom_scm_res res;
	struct device *dev = NULL;

	if (!__spdm)
		return -ENOTSUPP;

	if (__spdm->is_smc && !__spdm->dev)
		return -ENODEV;
	else
		dev = __spdm->dev;

	/* Number of args has to always be 6 for SPDM */
	for (i = 0; i < QCOM_SPDM_NARGS; i++) {
		desc.args[i] = (i < num_args) ? args[i] : 0;

	//	pr_err("args[%d] = 0x%x (%llu)", i, desc.args[i], desc.args[i]);
	}

	ret = qcom_scm_call(dev, &desc, &res);

	return ret ? : res.result[0];
}

/**
 * qcom_scm_spdm_cfg_bw_votes - Configure bandwidth votes
 * @spdm_client: SPDM client number
 * @up:          Vote in MB/s per step increment
 * @down:        Vote in MB/s per step decrement
 * @max:	 Maximum achievable bandwidth in MB/s
 */
int qcom_scm_spdm_cfg_bw_votes(u32 spdm_client, u32 up, u32 down, u32 max)
{
	u64 args[] = {
		QCOM_SCM_SPDM_CFG_BW_VOTES, spdm_client, up,
		down, max, QCOM_SPDM_VOTE_MULTI
	};

	return qcom_scm_spdm_call(args, ARRAY_SIZE(args));
}

/**
 * qcom_scm_spdm_cfg_cci_thresh - Configure CCI frequency threshold
 * @spdm_client: SPDM client number
 * @freq:        CCI frequency at which CCI response-time calculation
 *               is started in HW
 */
int qcom_scm_spdm_cfg_cci_thresh(u32 spdm_client, u32 freq)
{
	u64 args[] = { QCOM_SCM_SPDM_CFG_MAXCCIFREQ, spdm_client, freq };

	return qcom_scm_spdm_call(args, ARRAY_SIZE(args));
}

/**
 * qcom_scm_spdm_get_client_bw - Get client's current bandwidth vote
 * @spdm_client: SPDM client number
 *
 * Returns: Bandwidth vote or negative number for failure
 */
int qcom_scm_spdm_get_client_bw(u32 spdm_client)
{
	u64 args[] = { QCOM_SCM_SPDM_GET_BW_SPECIFIC, spdm_client };

	return qcom_scm_spdm_call(args, ARRAY_SIZE(args));
}

/**
 * qcom_scm_spdm_cfg_ports - Configure SPDM Ports
 * @spdm_client: SPDM client number
 * @ports:       Array of ports used by the SPDM client
 * @num_ports:   Number of ports in the passed array
 */
int qcom_scm_spdm_cfg_ports(u32 spdm_client, u32 *ports, u32 num_ports)
{
	u64 args[] = { QCOM_SCM_SPDM_CFG_PORTS, spdm_client, num_ports };
	int i;

	if (num_ports > 3)
		return -EINVAL;

	for (i = 0; i < num_ports; i++)
		args[i + 3] = ports[i];

	return qcom_scm_spdm_call(args, ARRAY_SIZE(args));
}

/**
 * qcom_scm_spdm_cfg_single_port - Configure SPDM for single port
 * @spdm_client: SPDM client number
 * @port:        SPDM port to configure
 *
 * Simplified function to configure just a single SPDM port.
 */
int qcom_scm_spdm_cfg_single_port(u32 spdm_client, u32 port)
{
	u64 args[] = { QCOM_SCM_SPDM_CFG_PORTS, spdm_client, 1, port};
	return qcom_scm_spdm_call(args, ARRAY_SIZE(args));
}

/**
 * qcom_scm_spdm_cfg_filter - Configure SPDM Filter
 * @spdm_client: SPDM client number
 * @aup:         SPDM Filter up alpha value
 * @adn:         SPDM Filter down alpha value
 * @bucket_sz:   SPDM Filter bucket size
 */
int qcom_scm_spdm_cfg_filter(u32 spdm_client, u32 aup, u32 adn, u32 bucket_sz)
{
	u64 args[] = { QCOM_SCM_SPDM_CFG_FILTER, spdm_client,
		       aup, adn, bucket_sz };

	return qcom_scm_spdm_call(args, ARRAY_SIZE(args));
}

/**
 * qcom_scm_spdm_cfg_perflevel - Configure SPDM Performance Level
 * @spdm_client: SPDM client number
 * @pl_freqs:    Cut-Over frequencies array
 * @num_freqs:   Number of cut-over frequencies in the array
 */
int qcom_scm_spdm_cfg_perflevel(u32 spdm_client, u32 *pl_freqs, u32 num_freqs)
{
	u64 args[5] = { QCOM_SCM_SPDM_CFG_PERFLEVEL, spdm_client, 0, 0, 0 };
	int i;

	if (num_freqs > 3)
		return -EINVAL;

	for (i = 0; i < num_freqs; i++)
		args[i + 2] = pl_freqs[i];

	return qcom_scm_spdm_call(args, ARRAY_SIZE(args));
}

static int qcom_scm_spdm_cfg_lvl_cmd(u32 cfg, u32 spdm_client, u32 lo, u32 hi)
{
	u64 args[4] = { cfg, spdm_client, lo, hi };

	return qcom_scm_spdm_call(args, ARRAY_SIZE(args));
}

static int qcom_scm_spdm_cfg_trilevel(u32 cfg_low, u32 cfg_med, u32 cfg_high,
				      u32 spdm_client,
				      const struct qcom_scm_spdm_level *r)
{
	int ret;

	ret = qcom_scm_spdm_cfg_lvl_cmd(cfg_low, spdm_client,
					r->low[0], r->low[1]);
//	if (ret)
//		return ret;

	ret = qcom_scm_spdm_cfg_lvl_cmd(cfg_med, spdm_client,
					r->med[0], r->med[1]);
//	if (ret)
//		return ret;

	ret = qcom_scm_spdm_cfg_lvl_cmd(cfg_high, spdm_client,
					r->high[0], r->high[1]);
//	if (ret)
//		return ret;

	return 0;
}

/**
 * qcom_scm_spdm_cfg_cci_resp_time - Configure perf-level CCI Response Time
 * @spdm_client: SPDM client number
 * @r:           Response Time levels in uS
 *
 * This function configures the CCI performance level response time used
 * from the HW to calculate the frequency threshold when the CCI is under
 * heavy load; the parameter values (levels) are in microseconds.
 */
int qcom_scm_spdm_cfg_cci_resp_time(u32 spdm_client,
				    const struct qcom_scm_spdm_level *r)
{
	/* Low, medium, high performance level response times of CCI */
	return qcom_scm_spdm_cfg_trilevel(QCOM_SCM_SPDM_CFG_CCIRESPT_LOW,
					  QCOM_SCM_SPDM_CFG_CCIRESPT_MED,
					  QCOM_SCM_SPDM_CFG_CCIRESPT_HIGH,
					  spdm_client, r);
}

/**
 * qcom_scm_spdm_cfg_reject_rate - Configure SPDM perf-level Rejection Rate
 * @spdm_client: SPDM client number
 * @r:           Rejection rate levels
 */
int qcom_scm_spdm_cfg_reject_rate(u32 spdm_client,
				  const struct qcom_scm_spdm_level *r)
{
	/* Low, medium, high performance level rejection rates */
	return qcom_scm_spdm_cfg_trilevel(QCOM_SCM_SPDM_CFG_REJR_LOW,
					  QCOM_SCM_SPDM_CFG_REJR_MED,
					  QCOM_SCM_SPDM_CFG_REJR_HIGH,
					  spdm_client, r);
}

/**
 * qcom_scm_spdm_cfg_resp_time - Configure SPDM perf-level Response Time
 * @spdm_client: SPDM client number
 * @r:           Response Time levels in uS
 *
 * This function configures the performance level response time; the
 * parameter values (levels) are in microseconds.
 */
int qcom_scm_spdm_cfg_resp_time(u32 spdm_client,
				const struct qcom_scm_spdm_level *r)
{
	/* Low, medium, high performance level response times */
	return qcom_scm_spdm_cfg_trilevel(QCOM_SCM_SPDM_CFG_RESPT_LOW,
					  QCOM_SCM_SPDM_CFG_RESPT_MED,
					  QCOM_SCM_SPDM_CFG_RESPT_HIGH,
					  spdm_client, r);
}

/**
 * qcom_scm_spdm_enable - Enable or disable the SPDM HW
 * @spdm_client: SPDM client number
 * @enable:      Whether to enable or disable
 */
int qcom_scm_spdm_enable(u32 spdm_client, bool enable)
{
	u64 args[3] = { 0, spdm_client, 0 }; /* last zero is "if cci_clk then put its freq in there, otherwise if no cci-clk, that's zero */

	args[0] = enable ? QCOM_SCM_SPDM_ENABLE : QCOM_SCM_SPDM_DISABLE;

	return qcom_scm_spdm_call(args, ARRAY_SIZE(args));
}

/**
 * qcom_scm_spdm_get_version - Get SPDM version
 * @max_ver:    Maximum supported version
 *
 * Sends a request to get the SPDM version and checks it against
 * the maximum version that we support.
 *
 * Returns: SPDM version or negative error number.
 */
static int qcom_scm_spdm_get_version(u64 max_version)
{
	u64 args[] = { QCOM_SCM_SPDM_GET_VERSION };
	int ret;

	ret = qcom_scm_spdm_call(args, ARRAY_SIZE(args));
	if (ret > max_version) {
		dev_err(__spdm->dev,
			"SPDM version 0x%x (%d) is not supported.\n", ret, ret);
		return -EINVAL;
	}
	dev_info(__spdm->dev, "SPDM Version 0x%x\n", ret);

	return ret;
}

int __qcom_scm_spdm_init(struct device *dev, unsigned long flags)
{
	struct qcom_scm_spdm *spdm;
	int ret;

	spdm = devm_kzalloc(dev, sizeof(*spdm), GFP_KERNEL);
	if (!spdm)
		return -ENOMEM;

	spdm->dev = dev;
	spdm->is_smc = !!(flags & SCM_HAS_SPDM_SMC);
	if (!spdm->is_smc) {
		dev_err(dev, "HVC SPDM is not supported\n");
		return -ENOTSUPP;
	}

	__spdm = spdm;

	ret = qcom_scm_spdm_get_version(QCOM_SPDM_MAX_VERSION);
	if (ret < 0) {
		__spdm = NULL;
		devm_kfree(dev, spdm);
		return ret;
	}

	return 0;
}
