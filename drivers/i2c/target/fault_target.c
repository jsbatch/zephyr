/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zephyr_i2c_target_fault

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c/target/fault.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(i2c_target_fault, CONFIG_I2C_LOG_LEVEL);

/*
 * The target presents a flat byte buffer addressed by a one-byte offset, the
 * same shape as the EEPROM target. The first byte of a write selects the
 * offset and later bytes store from there; reads stream from the offset.
 * Keeping the protocol this simple means a failed assertion in a test points
 * at the controller under test rather than at this target.
 */

struct i2c_target_fault_config_dt {
	struct i2c_dt_spec bus;
	uint8_t *buf;
	uint16_t buf_size;
};

struct i2c_target_fault_data {
	const struct device *dev;
	struct i2c_target_config target_cfg;

	struct k_spinlock lock;

	struct i2c_target_fault_config fault;
	bool armed;

	bool in_transaction;
	bool offset_pending;
	uint16_t offset;
	uint16_t write_index;
	uint16_t read_index;

	/* Transactions seen since the fault was armed, for repeat_count. */
	uint32_t repeat_seen;

	uint32_t transactions;
	uint32_t injections;

	size_t trace_count;
	bool trace_overflow;
	struct i2c_target_fault_event trace[CONFIG_I2C_TARGET_FAULT_TRACE_DEPTH];

	i2c_target_fault_hook_t hook;
	void *hook_data;
};

/*
 * What a callback decided to do, sampled while the lock was held.
 *
 * The refusal value and the hook are copied here rather than read again after
 * unlocking, so that an arm() or set_hook() call racing with a live
 * transaction cannot pair one callback's decision with another call's
 * parameters.
 */
struct fault_decision {
	bool refuse;
	int refuse_errno;
	i2c_target_fault_hook_t hook;
	void *hook_data;
};

static struct i2c_target_fault_data *fault_data(struct i2c_target_config *cfg)
{
	return CONTAINER_OF(cfg, struct i2c_target_fault_data, target_cfg);
}

/*
 * Record a callback and run the test hook. Called from every callback, which
 * on most controller drivers means interrupt context, so nothing here may
 * block. The trace saturates rather than wrapping: the interesting events are
 * the first ones after a fault is armed.
 */
static void trace_add(struct i2c_target_fault_data *data,
		      enum i2c_target_fault_event_type type, uint8_t value,
		      uint16_t index, bool injected)
{
	struct i2c_target_fault_event *ev;

	if (injected) {
		data->injections++;
	}

	if (data->trace_count >= ARRAY_SIZE(data->trace)) {
		data->trace_overflow = true;
		return;
	}

	ev = &data->trace[data->trace_count];
	ev->type = (uint8_t)type;
	ev->value = value;
	ev->index = index;
	ev->injected = injected;
	ev->cycles = k_cycle_get_32();
	data->trace_count++;
}

/*
 * Run the test hook outside the driver spinlock, so that a hook which touches
 * another peripheral cannot deadlock against this driver.
 */
static void run_hook(const struct fault_decision *decision,
		     const struct device *dev,
		     enum i2c_target_fault_event_type type, uint16_t index)
{
	if (decision->hook != NULL) {
		decision->hook(dev, type, index, decision->hook_data);
	}
}

/* Mark the start of a transaction, which a repeated START does not repeat. */
static void transaction_begin(struct i2c_target_fault_data *data)
{
	if (!data->in_transaction) {
		data->in_transaction = true;
		data->transactions++;
		data->repeat_seen++;
	}
}

/* True when the armed fault is of kind @p kind and this transaction is in range. */
static bool fault_active(struct i2c_target_fault_data *data,
			 enum i2c_target_fault_kind kind)
{
	if (!data->armed || data->fault.kind != kind) {
		return false;
	}

	if (data->fault.repeat_count == I2C_TARGET_FAULT_REPEAT_FOREVER) {
		return true;
	}

	return data->repeat_seen <= (uint32_t)data->fault.repeat_count;
}

/* True when @p index is the byte the armed fault acts on. */
static bool trigger_matches(const struct i2c_target_fault_data *data,
			    uint16_t index)
{
	return (data->fault.trigger_index == I2C_TARGET_FAULT_TRIGGER_EVERY) ||
	       (data->fault.trigger_index == index);
}

/* Capture the parameters a callback will act on while the lock is held. */
static void decision_capture(const struct i2c_target_fault_data *data,
			     struct fault_decision *decision, bool refuse)
{
	decision->refuse = refuse;
	decision->refuse_errno = (data->fault.refuse_errno < 0)
					 ? data->fault.refuse_errno
					 : -EIO;
	decision->hook = data->hook;
	decision->hook_data = data->hook_data;
}

/* Value the callback returns, which is zero when nothing was refused. */
static int decision_result(const struct fault_decision *decision)
{
	return decision->refuse ? decision->refuse_errno : 0;
}

static int fault_write_requested(struct i2c_target_config *cfg)
{
	struct i2c_target_fault_data *data = fault_data(cfg);
	struct fault_decision decision;
	k_spinlock_key_t key;
	bool refuse;

	key = k_spin_lock(&data->lock);
	transaction_begin(data);
	data->write_index = 0U;
	data->offset_pending = true;

	refuse = fault_active(data, I2C_TARGET_FAULT_NACK_WRITE_ADDR);
	decision_capture(data, &decision, refuse);
	trace_add(data, I2C_TARGET_FAULT_EV_WRITE_REQUESTED, 0U, 0U, refuse);
	k_spin_unlock(&data->lock, key);

	run_hook(&decision, data->dev, I2C_TARGET_FAULT_EV_WRITE_REQUESTED,
		 0U);

	return decision_result(&decision);
} /* fault_write_requested() */

static int fault_write_received(struct i2c_target_config *cfg, uint8_t val)
{
	struct i2c_target_fault_data *data = fault_data(cfg);
	const struct i2c_target_fault_config_dt *dt_cfg = data->dev->config;
	struct fault_decision decision;
	k_spinlock_key_t key;
	uint16_t index;
	bool refuse;

	key = k_spin_lock(&data->lock);
	index = data->write_index;
	data->write_index++;

	refuse = fault_active(data, I2C_TARGET_FAULT_NACK_WRITE_DATA) &&
		 trigger_matches(data, index);
	decision_capture(data, &decision, refuse);
	trace_add(data, I2C_TARGET_FAULT_EV_WRITE_RECEIVED, val, index,
		  refuse);

	/* A refused byte is not stored, so a NACK leaves no side effect. */
	if (!refuse) {
		if (data->offset_pending) {
			data->offset = (uint16_t)(val % dt_cfg->buf_size);
			data->offset_pending = false;
		} else {
			dt_cfg->buf[data->offset] = val;
			data->offset = (uint16_t)((data->offset + 1U) %
						  dt_cfg->buf_size);
		}
	}
	k_spin_unlock(&data->lock, key);

	run_hook(&decision, data->dev, I2C_TARGET_FAULT_EV_WRITE_RECEIVED,
		 index);

	return decision_result(&decision);
} /* fault_write_received() */

static int fault_read_requested(struct i2c_target_config *cfg, uint8_t *val)
{
	struct i2c_target_fault_data *data = fault_data(cfg);
	const struct i2c_target_fault_config_dt *dt_cfg = data->dev->config;
	struct fault_decision decision;
	k_spinlock_key_t key;
	bool refuse;

	key = k_spin_lock(&data->lock);
	transaction_begin(data);

	refuse = fault_active(data, I2C_TARGET_FAULT_NACK_READ_ADDR) ||
		 (fault_active(data, I2C_TARGET_FAULT_FAIL_READ_DATA) &&
		  trigger_matches(data, 0U));
	decision_capture(data, &decision, refuse);

	*val = dt_cfg->buf[data->offset];
	if (refuse) {
		/*
		 * A refused byte must not be mistaken for real data, so the
		 * idle bus level is presented instead and the offset is left
		 * where it was.
		 */
		*val = 0xFFU;
	} else {
		data->offset = (uint16_t)((data->offset + 1U) %
					  dt_cfg->buf_size);
	}
	data->read_index = 1U;

	trace_add(data, I2C_TARGET_FAULT_EV_READ_REQUESTED, *val, 0U, refuse);
	k_spin_unlock(&data->lock, key);

	run_hook(&decision, data->dev, I2C_TARGET_FAULT_EV_READ_REQUESTED, 0U);

	return decision_result(&decision);
} /* fault_read_requested() */

static int fault_read_processed(struct i2c_target_config *cfg, uint8_t *val)
{
	struct i2c_target_fault_data *data = fault_data(cfg);
	const struct i2c_target_fault_config_dt *dt_cfg = data->dev->config;
	struct fault_decision decision;
	k_spinlock_key_t key;
	uint16_t index;
	bool refuse;

	key = k_spin_lock(&data->lock);
	index = data->read_index;
	data->read_index++;

	refuse = fault_active(data, I2C_TARGET_FAULT_FAIL_READ_DATA) &&
		 trigger_matches(data, index);
	decision_capture(data, &decision, refuse);

	*val = dt_cfg->buf[data->offset];
	if (refuse) {
		*val = 0xFFU;
	} else {
		data->offset = (uint16_t)((data->offset + 1U) %
					  dt_cfg->buf_size);
	}

	trace_add(data, I2C_TARGET_FAULT_EV_READ_PROCESSED, *val, index,
		  refuse);
	k_spin_unlock(&data->lock, key);

	run_hook(&decision, data->dev, I2C_TARGET_FAULT_EV_READ_PROCESSED,
		 index);

	return decision_result(&decision);
} /* fault_read_processed() */

static int fault_stop(struct i2c_target_config *cfg)
{
	struct i2c_target_fault_data *data = fault_data(cfg);
	struct fault_decision decision;
	k_spinlock_key_t key;

	key = k_spin_lock(&data->lock);
	data->in_transaction = false;
	data->offset_pending = false;
	decision_capture(data, &decision, false);
	trace_add(data, I2C_TARGET_FAULT_EV_STOP, 0U, 0U, false);
	k_spin_unlock(&data->lock, key);

	run_hook(&decision, data->dev, I2C_TARGET_FAULT_EV_STOP, 0U);

	return 0;
} /* fault_stop() */

static void fault_error(struct i2c_target_config *cfg,
			enum i2c_error_reason reason)
{
	struct i2c_target_fault_data *data = fault_data(cfg);
	struct fault_decision decision;
	k_spinlock_key_t key;

	key = k_spin_lock(&data->lock);
	data->in_transaction = false;
	decision_capture(data, &decision, false);
	trace_add(data, I2C_TARGET_FAULT_EV_ERROR, (uint8_t)reason, 0U, false);
	k_spin_unlock(&data->lock, key);

	run_hook(&decision, data->dev, I2C_TARGET_FAULT_EV_ERROR, 0U);
} /* fault_error() */

static const struct i2c_target_callbacks fault_callbacks = {
	.write_requested = fault_write_requested,
	.read_requested = fault_read_requested,
	.write_received = fault_write_received,
	.read_processed = fault_read_processed,
	.stop = fault_stop,
	.error = fault_error,
};

int i2c_target_fault_arm(const struct device *dev,
			 const struct i2c_target_fault_config *cfg)
{
	struct i2c_target_fault_data *data = dev->data;
	k_spinlock_key_t key;

	if ((cfg != NULL) &&
	    ((cfg->kind < I2C_TARGET_FAULT_NONE) ||
	     (cfg->kind > I2C_TARGET_FAULT_FAIL_READ_DATA))) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	if ((cfg == NULL) || (cfg->kind == I2C_TARGET_FAULT_NONE)) {
		data->armed = false;
		(void)memset(&data->fault, 0, sizeof(data->fault));
	} else {
		data->fault = *cfg;
		data->armed = true;
	}

	/*
	 * repeat_count is measured from this call, so only the repeat window
	 * is rewound here. The public counters belong to
	 * i2c_target_fault_reset().
	 */
	data->repeat_seen = 0U;
	k_spin_unlock(&data->lock, key);

	return 0;
} /* i2c_target_fault_arm() */

void i2c_target_fault_disarm(const struct device *dev)
{
	(void)i2c_target_fault_arm(dev, NULL);
} /* i2c_target_fault_disarm() */

void i2c_target_fault_reset(const struct device *dev)
{
	struct i2c_target_fault_data *data = dev->data;
	k_spinlock_key_t key;

	key = k_spin_lock(&data->lock);
	data->trace_count = 0U;
	data->trace_overflow = false;
	data->transactions = 0U;
	data->injections = 0U;
	data->in_transaction = false;
	data->offset_pending = false;
	data->write_index = 0U;
	data->read_index = 0U;
	k_spin_unlock(&data->lock, key);
} /* i2c_target_fault_reset() */

size_t i2c_target_fault_event_count(const struct device *dev)
{
	struct i2c_target_fault_data *data = dev->data;
	k_spinlock_key_t key;
	size_t count;

	key = k_spin_lock(&data->lock);
	count = data->trace_count;
	k_spin_unlock(&data->lock, key);

	return count;
} /* i2c_target_fault_event_count() */

int i2c_target_fault_event(const struct device *dev, size_t idx,
			   struct i2c_target_fault_event *out)
{
	struct i2c_target_fault_data *data = dev->data;
	k_spinlock_key_t key;
	int ret = 0;

	key = k_spin_lock(&data->lock);
	if (idx >= data->trace_count) {
		ret = -ENOENT;
	} else {
		*out = data->trace[idx];
	}
	k_spin_unlock(&data->lock, key);

	return ret;
}

bool i2c_target_fault_overflowed(const struct device *dev)
{
	struct i2c_target_fault_data *data = dev->data;
	k_spinlock_key_t key;
	bool overflowed;

	key = k_spin_lock(&data->lock);
	overflowed = data->trace_overflow;
	k_spin_unlock(&data->lock, key);

	return overflowed;
} /* i2c_target_fault_overflowed() */

size_t i2c_target_fault_injections(const struct device *dev)
{
	struct i2c_target_fault_data *data = dev->data;
	k_spinlock_key_t key;
	uint32_t count;

	key = k_spin_lock(&data->lock);
	count = data->injections;
	k_spin_unlock(&data->lock, key);

	return (size_t)count;
} /* i2c_target_fault_injections() */

size_t i2c_target_fault_transactions(const struct device *dev)
{
	struct i2c_target_fault_data *data = dev->data;
	k_spinlock_key_t key;
	uint32_t count;

	key = k_spin_lock(&data->lock);
	count = data->transactions;
	k_spin_unlock(&data->lock, key);

	return (size_t)count;
} /* i2c_target_fault_transactions() */

int i2c_target_fault_set_data(const struct device *dev, const uint8_t *data_in,
			      size_t len)
{
	const struct i2c_target_fault_config_dt *dt_cfg = dev->config;
	struct i2c_target_fault_data *data = dev->data;
	k_spinlock_key_t key;

	if (len > dt_cfg->buf_size) {
		return -ENOSPC;
	}

	key = k_spin_lock(&data->lock);
	(void)memcpy(dt_cfg->buf, data_in, len);
	k_spin_unlock(&data->lock, key);

	return 0;
}

size_t i2c_target_fault_get_data(const struct device *dev, uint8_t *buf,
				 size_t len)
{
	const struct i2c_target_fault_config_dt *dt_cfg = dev->config;
	struct i2c_target_fault_data *data = dev->data;
	size_t copied = MIN(len, (size_t)dt_cfg->buf_size);
	k_spinlock_key_t key;

	key = k_spin_lock(&data->lock);
	(void)memcpy(buf, dt_cfg->buf, copied);
	k_spin_unlock(&data->lock, key);

	return copied;
}

size_t i2c_target_fault_size(const struct device *dev)
{
	const struct i2c_target_fault_config_dt *dt_cfg = dev->config;

	return (size_t)dt_cfg->buf_size;
}

void i2c_target_fault_set_hook(const struct device *dev,
			       i2c_target_fault_hook_t hook, void *user_data)
{
	struct i2c_target_fault_data *data = dev->data;
	k_spinlock_key_t key;

	key = k_spin_lock(&data->lock);
	data->hook_data = user_data;
	data->hook = hook;
	k_spin_unlock(&data->lock, key);
}

static int fault_driver_register(const struct device *dev)
{
	const struct i2c_target_fault_config_dt *dt_cfg = dev->config;
	struct i2c_target_fault_data *data = dev->data;

	return i2c_target_register(dt_cfg->bus.bus, &data->target_cfg);
}

static int fault_driver_unregister(const struct device *dev)
{
	const struct i2c_target_fault_config_dt *dt_cfg = dev->config;
	struct i2c_target_fault_data *data = dev->data;

	return i2c_target_unregister(dt_cfg->bus.bus, &data->target_cfg);
}

static DEVICE_API(i2c_target, fault_target_api) = {
	.driver_register = fault_driver_register,
	.driver_unregister = fault_driver_unregister,
};

static int i2c_target_fault_init(const struct device *dev)
{
	const struct i2c_target_fault_config_dt *dt_cfg = dev->config;
	struct i2c_target_fault_data *data = dev->data;

	if (!device_is_ready(dt_cfg->bus.bus)) {
		LOG_ERR("I2C controller %s is not ready", dt_cfg->bus.bus->name);
		return -ENODEV;
	}

	data->dev = dev;
	data->target_cfg.address = dt_cfg->bus.addr;
	data->target_cfg.callbacks = &fault_callbacks;

	return 0;
}

#define I2C_TARGET_FAULT_INIT(inst)                                                        \
	BUILD_ASSERT(DT_INST_PROP(inst, size) > 0,                                         \
		     "zephyr,i2c-target-fault size must be greater than zero");            \
	BUILD_ASSERT(DT_INST_PROP(inst, size) <= UINT8_MAX + 1,                            \
		     "zephyr,i2c-target-fault size must be addressable by one byte");      \
                                                                                           \
	static uint8_t i2c_target_fault_buf_##inst[DT_INST_PROP(inst, size)];              \
                                                                                           \
	static struct i2c_target_fault_data i2c_target_fault_data_##inst;                  \
                                                                                           \
	static const struct i2c_target_fault_config_dt i2c_target_fault_cfg_##inst = {     \
		.bus = I2C_DT_SPEC_INST_GET(inst),                                         \
		.buf = i2c_target_fault_buf_##inst,                                        \
		.buf_size = (uint16_t)DT_INST_PROP(inst, size),                            \
	};                                                                                 \
                                                                                           \
	DEVICE_DT_INST_DEFINE(inst, i2c_target_fault_init, NULL,                           \
			      &i2c_target_fault_data_##inst,                               \
			      &i2c_target_fault_cfg_##inst, POST_KERNEL,                   \
			      CONFIG_I2C_TARGET_INIT_PRIORITY, &fault_target_api);

DT_INST_FOREACH_STATUS_OKAY(I2C_TARGET_FAULT_INIT)
