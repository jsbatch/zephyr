/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Infineon Technologies AG,
 * or an affiliate of Infineon Technologies AG. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief PSC3M5 full-shape clock-management consumer demonstration.
 *
 * Reports the PSC3M5 high-frequency clock tree through the clock-management
 * API. The tree itself is modelled at SoC scope in system_clocks_cm.dtsi; this
 * consumer holds only opaque clock-output handles and reads rates through the
 * framework, so it never includes a vendor clock header or touches a register.
 * That is the point of the demonstration: everything printed below is obtained
 * without any Infineon-specific clock code in the application.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_management.h>
#include <zephyr/sys/printk.h>

#define CONSUMER_NODE DT_NODELABEL(cm_demo)
#define CPU_NODE      DT_NODELABEL(cpu0)
#define CONSOLE_DIV   DT_NODELABEL(peri0_group4_8bit_0)

CLOCK_MANAGEMENT_DT_DEFINE_OUTPUT_BY_NAME(CONSUMER_NODE, cpu);
CLOCK_MANAGEMENT_DT_DEFINE_OUTPUT_BY_NAME(CONSUMER_NODE, hf1);
CLOCK_MANAGEMENT_DT_DEFINE_OUTPUT_BY_NAME(CONSUMER_NODE, periclk);
CLOCK_MANAGEMENT_DT_DEFINE_OUTPUT_BY_NAME(CONSUMER_NODE, hf3);
CLOCK_MANAGEMENT_DT_DEFINE_OUTPUT_BY_NAME(CONSUMER_NODE, hf4);

/* One row of the clk_hf report. */
struct clk_hf_report {
	const char *name;
	const char *users;
	const struct clock_output *out;
};

/*
 * Consumers per root follow the PSC3 clock architecture (Architecture RM
 * Ch.19, Table 112): clk_hf0 is the system clock, and the remaining roots feed
 * the fixed peripheral clock groups.
 */
static const struct clk_hf_report clk_hf_reports[] = {
	{
		.name = "clk_hf0",
		.users = "CPU, PERI, SRSS, groups 0/2",
		.out = CLOCK_MANAGEMENT_DT_GET_OUTPUT_BY_NAME(CONSUMER_NODE, cpu),
	},
	{
		.name = "clk_hf1",
		.users = "LPCOMP, groups 1/3",
		.out = CLOCK_MANAGEMENT_DT_GET_OUTPUT_BY_NAME(CONSUMER_NODE, hf1),
	},
	{
		.name = "clk_hf2",
		.users = "group 4: console UART, CAN",
		.out = CLOCK_MANAGEMENT_DT_GET_OUTPUT_BY_NAME(CONSUMER_NODE, periclk),
	},
	{
		.name = "clk_hf3",
		.users = "HPPASS, group 5: TCPWM, CSG",
		.out = CLOCK_MANAGEMENT_DT_GET_OUTPUT_BY_NAME(CONSUMER_NODE, hf3),
	},
	{
		.name = "clk_hf4",
		.users = "group 6: SCB5 SPI",
		.out = CLOCK_MANAGEMENT_DT_GET_OUTPUT_BY_NAME(CONSUMER_NODE, hf4),
	},
};

static const clock_management_state_t periclk_default =
	CLOCK_MANAGEMENT_DT_GET_STATE(CONSUMER_NODE, periclk, default);

/* The console source clock, reported on its own as well as in the table. */
static const struct clock_output *const console_clk =
	CLOCK_MANAGEMENT_DT_GET_OUTPUT_BY_NAME(CONSUMER_NODE, periclk);

/* Report every clk_hf root the framework exposes an output for. */
static void report_clock_tree(void)
{
	printk("\nHigh-frequency clock roots (read through clock management)\n");

	for (size_t i = 0; i < ARRAY_SIZE(clk_hf_reports); i++) {
		const struct clk_hf_report *row = &clk_hf_reports[i];
		int rate = clock_management_get_rate(row->out);

		if (rate < 0) {
			printk("  %-8s  <get_rate failed: %d>  %s\n", row->name, rate,
			       row->users);
			continue;
		}

		printk("  %-8s  %9d Hz  %s\n", row->name, rate, row->users);
	}
}

/*
 * Show the ownership boundary this port is built around: clock management owns
 * the shared source clock, while the peripheral driver owns the leaf divider
 * that turns it into a bit rate. The divider value is read from devicetree
 * because it belongs to the driver, not to the clock-management tree.
 */
static void report_ownership_boundary(void)
{
	uint32_t div = DT_PROP(CONSOLE_DIV, clock_div);
	int src_rate = clock_management_get_rate(console_clk);

	printk("\nOwnership boundary for the console UART\n");

	if (src_rate < 0) {
		printk("  source rate unavailable (%d)\n", src_rate);
		return;
	}

	if (div == 0U) {
		printk("  leaf divider is zero, cannot derive the bit-rate clock\n");
		return;
	}

	printk("  clk_hf2 source rate (clock management)  %9d Hz\n", src_rate);
	printk("  leaf divider /%u (UART driver)          %9d Hz\n", div,
	       src_rate / (int)div);
	printk("  configured baud rate                    %9d\n",
	       DT_PROP(DT_NODELABEL(uart3), current_speed));
}

/*
 * Compare the rate a state declares with the rate the producers compute. In
 * static mode apply_state() returns the clock-frequency written in devicetree,
 * while get_rate() walks the tree and recomputes it, so a mismatch means the
 * declared value does not match the divider math.
 */
static int report_state_application(void)
{
	int declared;
	int computed;

	printk("\nApplying the clk_hf2 default state\n");

	declared = clock_management_apply_state(console_clk, periclk_default);
	if (declared < 0) {
		printk("  apply_state failed (%d)\n", declared);
		return declared;
	}

	computed = clock_management_get_rate(console_clk);
	if (computed < 0) {
		printk("  get_rate failed (%d)\n", computed);
		return computed;
	}

	printk("  declared rate (apply_state)  %9d Hz\n", declared);
	printk("  computed rate (get_rate)     %9d Hz\n", computed);
	printk("  declared %s computed\n", (declared == computed) ? "==" : "!=");

	return 0;
}

#if defined(CONFIG_CLOCK_MANAGEMENT_RUNTIME)
/*
 * Runtime mode adds frequency negotiation: instead of naming a state, a
 * consumer asks for a range and the producers pick a configuration that fits.
 * The request below brackets the console rate, so it should resolve without
 * changing the clock.
 */
static void report_rate_request(void)
{
	const struct clock_management_rate_req req = {
		.min_freq = 70000000,
		.max_freq = 90000000,
		.max_rank = CLOCK_MANAGEMENT_ANY_RANK,
	};
	int rate = clock_management_req_rate(console_clk, &req);

	printk("\nRuntime rate request for clk_hf2 (70-90 MHz)\n");

	if (rate < 0) {
		printk("  req_rate failed (%d)\n", rate);
		return;
	}

	printk("  granted rate  %9d Hz\n", rate);
}
#endif /* CONFIG_CLOCK_MANAGEMENT_RUNTIME */

int main(void)
{
	int ret;

	printk("PSC3M5 full-shape clock-management demo\n");
	printk("CPU clock declared in devicetree: %d Hz\n",
	       DT_PROP(CPU_NODE, clock_frequency));
	printk("Kernel timer frequency: %u Hz\n", sys_clock_hw_cycles_per_sec());

	report_clock_tree();
	report_ownership_boundary();

	ret = report_state_application();
	if (ret < 0) {
		return 0;
	}

#if defined(CONFIG_CLOCK_MANAGEMENT_RUNTIME)
	report_rate_request();
#endif

	printk("\nclock-management demo complete\n");

	return 0;
}
