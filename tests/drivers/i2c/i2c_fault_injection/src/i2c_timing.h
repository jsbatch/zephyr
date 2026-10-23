/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Cycle-counter instrumentation and the bounded-execution harness.
 *
 * Timing uses k_cycle_get_32() and sys_clock_hw_cycles_per_sec(), the same
 * primitives the bit-banged controller uses for its own bit timing, so it is
 * available on every platform and needs no peripheral.
 *
 * The bounded harness exists because the principal defect this suite looks
 * for is a driver that never returns. Measuring elapsed time in the calling
 * thread cannot detect that, since the calling thread is the one that is
 * stuck. Transfers therefore run on a worker thread that the test thread
 * waits on with a timeout.
 */

#ifndef I2C_TIMING_H_
#define I2C_TIMING_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>

/** A measured interval, in cycle-counter ticks. */
struct i2c_timing_span {
	uint32_t start;
	uint32_t end;
};

/** Result of running a job under the time bound. */
struct i2c_fault_outcome {
	/** Value the job returned, meaningless when timed_out is set. */
	int ret;
	/** Wall time the job took, or the bound itself when it timed out. */
	uint32_t elapsed_us;
	/** Cycle counter sampled immediately before the job ran. */
	uint32_t start_cycles;
	/** Cycle counter sampled immediately after the job returned. */
	uint32_t end_cycles;
	/** Set when the job did not return within the bound. */
	bool timed_out;
};

/**
 * Report whether two measured intervals overlapped.
 *
 * Used by the conditions that need two transfers to have been in flight at
 * once. Overlap is arranged as well as it can be and then checked, rather
 * than assumed, so that a condition which failed to produce it skips instead
 * of passing on no evidence.
 *
 * @param a_start cycle count at the start of the first interval.
 * @param a_end   cycle count at the end of the first interval.
 * @param b_start cycle count at the start of the second interval.
 * @param b_end   cycle count at the end of the second interval.
 *
 * @return true if the intervals intersect.
 */
bool i2c_timing_overlapped(uint32_t a_start, uint32_t a_end, uint32_t b_start,
			   uint32_t b_end);

/** Signature of a job the bounded harness runs. */
typedef int (*i2c_fault_job_fn)(void *arg);

/** Sample the cycle counter at the start of an interval. */
void i2c_timing_begin(struct i2c_timing_span *span);

/** Sample the cycle counter at the end of an interval. */
void i2c_timing_end(struct i2c_timing_span *span);

/** Length of a measured interval in microseconds. */
uint32_t i2c_timing_us(const struct i2c_timing_span *span);

/** Print a measurement through the ztest output. */
void i2c_timing_report(const char *what, uint32_t value, const char *unit);

/**
 * Start the worker thread the bounded harness runs jobs on.
 *
 * Called once from the suite setup. Safe to call again; later calls do
 * nothing.
 */
void i2c_timing_harness_start(void);

/**
 * Run @p fn on the worker thread and wait for it under the time bound.
 *
 * The bound is CONFIG_I2C_FAULT_TEST_TIMEOUT_MS. A job that outruns it is
 * left running, because aborting a thread stuck inside a controller driver
 * would leave the peripheral in an unknown state and make every later result
 * meaningless. The suite records the hang instead and skips what follows.
 *
 * @param[in]  fn  job to run.
 * @param[in]  arg opaque argument handed to @p fn.
 * @param[out] out result and elapsed time.
 *
 * @retval 0 if the job returned within the bound.
 * @retval -ETIMEDOUT if it did not.
 */
int i2c_timing_run_bounded(i2c_fault_job_fn fn, void *arg,
			   struct i2c_fault_outcome *out);

/**
 * Hand @p fn to the worker thread and return without waiting.
 *
 * Used by the conditions that must act on the bus while a transfer is in
 * flight, such as releasing a held line partway through an attempt. Must be
 * paired with i2c_timing_collect().
 *
 * @param fn  job to run.
 * @param arg opaque argument handed to @p fn.
 *
 * @retval 0 on success.
 * @retval -ETIMEDOUT if an earlier job never returned.
 */
int i2c_timing_submit(i2c_fault_job_fn fn, void *arg);

/**
 * Wait under the time bound for the job handed over by i2c_timing_submit().
 *
 * @param[out] out result and elapsed time.
 *
 * @retval 0 if the job returned within the bound.
 * @retval -ETIMEDOUT if it did not.
 */
int i2c_timing_collect(struct i2c_fault_outcome *out);

/**
 * Give a job that outran the bound a second chance to finish.
 *
 * Called after the stimulus that caused the stall has been removed, for
 * instance once a held bus line has been released. If the worker comes back
 * the harness is usable again and the tests that follow are not skipped.
 *
 * @return true if the worker returned and the harness is usable again.
 */
bool i2c_timing_harness_recover(void);

/**
 * Report whether a job has ever outrun the bound.
 *
 * Once that has happened the worker thread is parked inside a controller
 * driver and no later measurement means anything, so the suite skips the
 * tests that follow rather than reporting them as passes.
 *
 * @return true if a job never returned.
 */
bool i2c_timing_harness_stuck(void);

/**
 * Run @p fn under the bound and fail the test if it does not return.
 *
 * @param[in]  fn        job to run.
 * @param[in]  arg       opaque argument handed to @p fn.
 * @param[in]  condition condition identifier used in the failure message.
 * @param[out] out       result and elapsed time.
 */
void i2c_timing_run_or_fail(i2c_fault_job_fn fn, void *arg,
			    const char *condition,
			    struct i2c_fault_outcome *out);

#endif /* I2C_TIMING_H_ */
