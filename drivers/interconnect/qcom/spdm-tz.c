// SPDX-License-Identifier: GPL-2.0
/*
 * System Performance Dynamic Monitoring (SPDM) interconnect driver
 *
 * Copyright (C) 2021, AngeloGioacchino Del Regno
 *		       <angelogioacchino.delregno@somainline.org>
 */

/* TODO: This driver has to be cleaned up!!! */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/interconnect-provider.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/qcom_scm.h>
#include <dt-bindings/interconnect/qcom,spdm-tz.h>

#define PERF_LEVEL_MAX_FREQS	2
#define SPDM_CLIENT_CPU		0
#define SPDM_CLIENT_GPU		1
#define SPDM_TZ_MAX_LINKS	1

enum {
	TZ_APSS_SPDM = 500,
	TZ_SLAVE_SPDM
};

#define to_qcom_provider(_provider) \
	container_of(_provider, struct qcom_spdm_tz_icc_provider, provider)

struct qcom_spdm_tz_icc_provider {
	struct icc_provider provider;
	const struct qcom_spdm_tz_desc *spdm;
	struct clk *core_clk;
	struct clk *cci_clk;
	bool enabled;
};

/**
 * struct qcom_icc_node - Qualcomm specific interconnect nodes
 * @name: the node name used in debugfs
 * @links: an array of nodes where we can go next while traversing
 * @id: a unique node identifier
 * @num_links: the total number of @links
 * @buswidth: width of the interconnect between a node and the bus
 */
struct qcom_icc_node {
	const char *name;
	u16 links[SPDM_TZ_MAX_LINKS];
	u16 id;
	u16 num_links;
	u16 buswidth;
};

/**
 * struct qcom_spdm_tz_desc - SPDM-TZ SoC specific parameters
 * @alpha_up:      up value for the SPDM Filter
 * @alpha_down:    down value for the SPDM Filter
 * @bucket_size:   bucket size of the SPDM Filter
 * @port:          SPDM port used by current instance
 * @client:        SPDM client number
 * @down_interval: down-vote polling interval
 * @bw_upstep:     Initial BW up vote in MB/s per step increment
 * @bw_downstep:   Initial BW down vote in MB/s per step increment
 * @bw_max_vote:   Maximum achievable SPDM bandwidth in MB/s
 * @cci_resp_freq: CCI frequency at which response-time calculation
 *                 is started in the SPDM HW
 * @perflvl_freqs: SPDM Performance Level cut-over frequencies
 * @num_pl_freqs:  Number of entries in the perflvl_freqs array
 * @reject_rate:   Rejection Rate for internal SPDM HW calculations
 * @resp_us:       Response Time (uS) for internal SPDM HW calculations
 * @cci_resp_us:   CCI Response Time (uS) for internal SPDM HW calculations
 */
struct qcom_spdm_tz_desc {
	u8 alpha_up;
	u8 alpha_down;
	u8 bucket_size;
	u8 port;
	u8 client;
	u8 down_interval;

	u16 bw_upstep;
	u16 bw_downstep;
	u16 bw_max_vote;
	u32 cci_resp_freq;
	u32 perflvl_freqs[PERF_LEVEL_MAX_FREQS];
	u8 num_pl_freqs;

	const struct qcom_scm_spdm_level *reject_rate;
	const struct qcom_scm_spdm_level *resp_us;
	const struct qcom_scm_spdm_level *cci_resp_us;
};

/**
 * struct qcom_icc_desc - Specific interconnect descriptor
 * @nodes:         list of Qualcomm specific interconnect nodes
 * @num_nodes:     number of entries the nodes array
 * @spdm:          specific SPDM parameters
 */
struct qcom_icc_desc {
	const struct qcom_icc_node **nodes;
	size_t num_nodes;
	const struct qcom_spdm_tz_desc *spdm;
};

static const struct qcom_scm_spdm_level reject_rate_5k = {
	.low  = { 5000, 5000 },
	.med  = { 5000, 5000 },
	.high = { 5000, 5000 }
};

static const struct qcom_scm_spdm_level resp_us_10k = {
	.low  = { 10000, 10000 },
	.med  = { 10000, 10000 },
	.high = { 10000, 10000 }
};

#define DEFINE_QNODE(_name, _id, _buswidth, ...)			\
	static const struct qcom_icc_node _name = {			\
		.name = #_name,						\
		.id = _id,						\
		.buswidth = _buswidth,					\
		.num_links = ARRAY_SIZE(((int[]){ __VA_ARGS__ })),	\
		.links = { __VA_ARGS__ },				\
	}

DEFINE_QNODE(apss_spdm_mas, TZ_APSS_SPDM, 16, TZ_SLAVE_SPDM);
DEFINE_QNODE(spdm_slv, TZ_SLAVE_SPDM, 16);

static const struct qcom_icc_node *spdm_cpu_nodes[] = {
	[MASTER_APSS_SPDM_TZ] = &apss_spdm_mas,
	[SLAVE_APSS_SPDM_TZ]  = &spdm_slv,
};

static const struct qcom_spdm_tz_desc msm8998_spdm_cpu_desc = {
	.client = SPDM_CLIENT_CPU,
	.down_interval = 100,
	.port = 24,
	.alpha_up = 12,
	.alpha_down = 15,
	.bucket_size = 8,
	.bw_upstep = 1000,
	.bw_downstep = 1000,
	.bw_max_vote = 10000,
	.cci_resp_freq = 1036800,
	.perflvl_freqs = { 260000, 770000 },
	.num_pl_freqs = ARRAY_SIZE(msm8998_spdm_cpu_desc.perflvl_freqs),
	.reject_rate = &reject_rate_5k,
	.resp_us     = &resp_us_10k,
	.cci_resp_us = &resp_us_10k,
};

static const struct qcom_icc_desc msm8998_spdm = {
	.nodes = spdm_cpu_nodes,
	.num_nodes = ARRAY_SIZE(spdm_cpu_nodes),
	.spdm = &msm8998_spdm_cpu_desc,
};

static const struct qcom_spdm_tz_desc sdm630_spdm_cpu_desc = {
	.client = SPDM_CLIENT_CPU,
	.down_interval = 30,
	.port = 24,
	.alpha_up = 8,
	.alpha_down = 15,
	.bucket_size = 8,
	.bw_upstep = 450,
	.bw_downstep = 6750,
	.bw_max_vote = 6750,
	.cci_resp_freq = 1036800,
	.perflvl_freqs = { 260000, 610000 },
	.num_pl_freqs = ARRAY_SIZE(sdm630_spdm_cpu_desc.perflvl_freqs),
	.reject_rate = &reject_rate_5k,
	.resp_us     = &resp_us_10k,
	.cci_resp_us = &resp_us_10k,
};

static const struct qcom_icc_desc sdm630_spdm = {
	.nodes = spdm_cpu_nodes,
	.num_nodes = ARRAY_SIZE(spdm_cpu_nodes),
	.spdm = &sdm630_spdm_cpu_desc,
};

static const struct qcom_spdm_tz_desc sdm660_spdm_cpu_desc = {
	.client = SPDM_CLIENT_CPU,
	.down_interval = 30,
	.port = 24,
	.alpha_up = 8,
	.alpha_down = 15,
	.bucket_size = 8,
	.bw_upstep = 450,
	.bw_downstep = 8200,
	.bw_max_vote = 8200,
	.cci_resp_freq = 1036800,
	.perflvl_freqs = { 260000, 610000 },
	.num_pl_freqs = ARRAY_SIZE(sdm630_spdm_cpu_desc.perflvl_freqs),
	.reject_rate = &reject_rate_5k,
	.resp_us     = &resp_us_10k,
	.cci_resp_us = &resp_us_10k,
};

static const struct qcom_icc_desc sdm660_spdm = {
	.nodes = spdm_cpu_nodes,
	.num_nodes = ARRAY_SIZE(spdm_cpu_nodes),
	.spdm = &sdm660_spdm_cpu_desc,
};

static int qcom_spdm_tz_setup(const struct qcom_spdm_tz_desc *desc)
{
	int ret;

	ret = qcom_scm_spdm_cfg_single_port(desc->client, desc->port);
	if (ret < 0) {
		pr_err("cfg_single_port returns 0x%x", ret);
		//return ret;
	}

	/* ***** TODO: USE THE RIGHT RET CHECKS ***** */

	/* The SCM functions are not returning 0 for success and
	 * negative for error, but throwing here the TZ retval.
	 * This is most probably wrong, think about the best strategy...
	 */ 


pr_err("Single port\n");
	ret = qcom_scm_spdm_cfg_filter(desc->client, desc->alpha_up,
				       desc->alpha_down, desc->bucket_size);
//
//	if (ret < 0)
//		return ret;
pr_err("Filter ret 0x%x\n", ret);
	ret = qcom_scm_spdm_cfg_perflevel(desc->client,
					  (u32*)desc->perflvl_freqs,
					  (u32)desc->num_pl_freqs);
//	if (ret < 0)
//		return ret;
pr_err("PerfLevel ret 0x%x\n", ret);
	ret = qcom_scm_spdm_cfg_reject_rate(desc->client, desc->reject_rate);
//	if (ret < 0)
//		return ret;
pr_err("RejRate ret 0x%x\n", ret);
	ret = qcom_scm_spdm_cfg_resp_time(desc->client, desc->resp_us);
//	if (ret)
//		return ret;
pr_err("RespuS ret 0x%x\n", ret);
	ret = qcom_scm_spdm_cfg_cci_resp_time(desc->client, desc->cci_resp_us);
//	if (ret)
//		return ret;
pr_err("CCI RespuS ret 0x%x\n", ret);
	ret = qcom_scm_spdm_cfg_cci_thresh(desc->client, desc->cci_resp_freq);
//	if (ret)
//		return ret;
pr_err("CCI Threshold ret 0x%x\n", ret);
	ret = qcom_scm_spdm_cfg_bw_votes(desc->client, desc->bw_upstep,
					 desc->bw_downstep, desc->bw_max_vote);
//	if (ret)
//		return ret;
pr_err("bw votes ret 0x%x\n", ret);

	ret = qcom_scm_spdm_enable(desc->client, true);
	pr_err("spdm ena=0x%x\n", ret);
//	if (ret)
//		return ret;


	return 0;
}

/**
 * qcom_icc_spdm_set - Control enable state of SPDM based on BW request
 * @src: source icc node
 * @dst: destination icc node (unused)
 *
 * The SPDM HW controls the bandwidth setting internally, but has to be
 * disabled during suspend in order to improve power consumption in that
 * state. This function only enables the SPDM HW when there is any BW
 * request, otherwise disables it.
 */
static int qcom_icc_spdm_set(struct icc_node *src, struct icc_node *dst)
{
	struct qcom_spdm_tz_icc_provider *qp;
	struct icc_provider *provider;
	const struct qcom_icc_node *qn;
	const struct qcom_spdm_tz_desc *spdm;
	struct icc_node *n;
	u32 agg_peak = 0;
	u32 agg_avg = 0;
	u64 rate;
	int ret;

	qn = src->data;
	provider = src->provider;
	qp = to_qcom_provider(provider);
	spdm = qp->spdm;

	list_for_each_entry(n, &provider->nodes, node_list)
		provider->aggregate(n, 0, n->avg_bw, n->peak_bw,
				    &agg_avg, &agg_peak);

	rate = max(agg_avg, agg_peak);
	rate = icc_units_to_bps(rate);
	do_div(rate, qn->buswidth);

	/*
	 * Disabling the SPDM makes it to sometimes lose its configuration,
	 * so just set it up again if we're coming from a disabled state
	 * as to avoid errors.
	 */
	if (rate > 0 && !qp->enabled) {
		ret = qcom_spdm_tz_setup(spdm);
		if (ret)
			return ret;
		qp->enabled = true;
	} else if (rate == 0 && qp->enabled) {
		qp->enabled = false;
	}

	ret = qcom_scm_spdm_enable(spdm->client, qp->enabled);
	if (ret == -22)
		pr_err("Failure %d\n", ret);
	return 0;
}

static int qcom_icc_spdm_get_bw(struct icc_node *node, u32 *avg, u32 *peak)
{
	/* At boot, the SPDM HW has 0 BW vote */
	*avg = *peak = 0;
	return 0;
}

static int qcom_spdm_tz_remove(struct platform_device *pdev)
{
	struct qcom_spdm_tz_icc_provider *qp = platform_get_drvdata(pdev);

	qcom_scm_spdm_enable(qp->spdm->client, false);
	clk_disable_unprepare(qp->core_clk);
	icc_nodes_remove(&qp->provider);
	return icc_provider_del(&qp->provider);
}

static irqreturn_t spdm_irq_handler(int irq, void *dev_id)
{
	struct platform_device *pdev = dev_id;
	struct qcom_spdm_tz_icc_provider *qp = platform_get_drvdata(pdev);
	int ret;

	/* TODO: This never happens for 8998, 630, 660 */
	pr_err("SPDM IRQ FIRED!\n");

	return IRQ_HANDLED;
}

static int qcom_spdm_tz_probe(struct platform_device *pdev)
{
	struct qcom_spdm_tz_icc_provider *qp;
	const struct qcom_icc_desc *desc;
	struct icc_onecell_data *data;
	struct icc_provider *provider;
	const struct qcom_icc_node **qnodes;
	struct icc_node *node;
	size_t num_nodes;
	struct clk *core_clk, *cci_clk;
	int i, irq, ret;

	if (!qcom_scm_is_available())
		return -EPROBE_DEFER;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	core_clk = devm_clk_get(&pdev->dev, "core");
	if (IS_ERR(core_clk))
		return PTR_ERR(core_clk);

	cci_clk = devm_clk_get_optional(&pdev->dev, "cci");
	if (IS_ERR(cci_clk))
		return PTR_ERR(cci_clk);

	qp = devm_kzalloc(&pdev->dev, sizeof(*qp), GFP_KERNEL);
	if (!qp)
		return -ENOMEM;

	desc = device_get_match_data(&pdev->dev);
	if (!desc)
		return -EINVAL;

	qnodes = desc->nodes;
	num_nodes = desc->num_nodes;
	qp->spdm = desc->spdm;
	qp->core_clk = core_clk;
	qp->cci_clk = cci_clk;

	data = devm_kzalloc(&pdev->dev, struct_size(data, nodes, num_nodes),
			    GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	provider = &qp->provider;
	INIT_LIST_HEAD(&provider->nodes);
	provider->dev = &pdev->dev;
	provider->set = qcom_icc_spdm_set;
	provider->get_bw = qcom_icc_spdm_get_bw;
	provider->aggregate = icc_std_aggregate;
	provider->xlate = of_icc_xlate_onecell;
	provider->data = data;

	ret = clk_prepare_enable(qp->core_clk);
	if (ret)
		return ret;

	ret = icc_provider_add(provider);
	if (ret) {
		dev_err(&pdev->dev, "error adding interconnect provider\n");
		return ret;
	}

	for (i = 0; i < num_nodes; i++) {
		size_t j;

		node = icc_node_create(qnodes[i]->id);
		if (IS_ERR(node)) {
			ret = PTR_ERR(node);
			goto err;
		}

		node->name = qnodes[i]->name;
		/* Cast away const and add it back in qcom_icc_set() */
		node->data = (void *)qnodes[i];
		icc_node_add(node, provider);

		for (j = 0; j < qnodes[i]->num_links; j++)
			icc_link_create(node, qnodes[i]->links[j]);

		data->nodes[i] = node;
	}
	data->num_nodes = num_nodes;

	platform_set_drvdata(pdev, qp);

	return devm_request_threaded_irq(&pdev->dev, irq, NULL,
					 spdm_irq_handler, IRQF_ONESHOT,
					 pdev->name, pdev);
err:
	icc_nodes_remove(provider);
	icc_provider_del(provider);

	return ret;
}

static const struct of_device_id spdm_tz_of_match[] = {
	{ .compatible = "qcom,msm8998-spdm-cpu", .data = &msm8998_spdm },
	{ .compatible = "qcom,sdm630-spdm-cpu", .data = &sdm630_spdm },
	{ .compatible = "qcom,sdm660-spdm-cpu", .data = &sdm660_spdm },
	{ }
};
MODULE_DEVICE_TABLE(of, spdm_tz_of_match);

static struct platform_driver spdm_tz_driver = {
	.probe = qcom_spdm_tz_probe,
	.remove = qcom_spdm_tz_remove,
	.driver = {
		.name = "spdm-tz",
		.of_match_table = spdm_tz_of_match,
		.sync_state = icc_sync_state,
	},
};
module_platform_driver(spdm_tz_driver);

MODULE_DESCRIPTION("Qualcomm SPDM TZ interconnect driver");
MODULE_LICENSE("GPL v2");
