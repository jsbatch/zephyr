drivers: i2c: infineon_pdl: reset the completion semaphore per message

A stale count on transfer_sem lets the next transfer report success
before the bus has done anything.  The driver now resets the semaphore
before each submission.

Assisted-by: Copilot:claude-opus-5.5
Signed-off-by: John Batch <john.batch@infineon.com>

# Please enter the commit message for your changes. Lines starting
# with '#' will be ignored, and an empty message aborts the commit.
#
# Date:      Thu Oct 15 15:06:18 2026 -0700
#
# interactive rebase in progress; onto 03b0b5443c7
# Last commands done (2 commands done):
#    reword 195fd0a9d4a drivers: i2c: infineon_pdl: end a transfer only on its own completion
#    reword 269556682f5 drivers: i2c: infineon_pdl: reset the completion semaphore per message
# Next command to do (1 remaining command):
#    pick 096c5c09579 drivers: i2c: infineon_pdl: assert that no transfer is in progress
# You are currently editing a commit while rebasing branch 'gooey-uniformly-lankiness' on '03b0b5443c7'.
#
# Changes to be committed:
#	modified:   drivers/i2c/i2c_infineon_pdl.c
#
# Untracked files:
#	boards/infineon/kit_psc3m5_evk/arduino_r3_connector.dtsi
#	boards/infineon/kit_psc3m6_eval/arduino_r3_connector.dtsi
#	boards/infineon/kit_pse84_eval/arduino_r3_connector.dtsi
#	boards/shields/cy15framkit_001/
#	drivers/i2c/target/Kconfig.fault
#	drivers/i2c/target/fault_target.c
#	dts/bindings/i2c/zephyr,i2c-target-fault.yaml
#	include/zephyr/drivers/i2c/target/fault.h
#	tests/drivers/i2c/i2c_api/boards/kit_pse84_eval_pse846gps2dbzc4a_m33.overlay
#	tests/drivers/i2c/i2c_at24/boards/cy8cproto_041tp.conf
#	tests/drivers/i2c/i2c_at24/boards/cy8cproto_041tp.overlay
#	tests/drivers/i2c/i2c_at24/boards/kit_psc3m5_evk_psc3m5fds2afq1.conf
#	tests/drivers/i2c/i2c_fault_injection/
#	tests/drivers/i2c/i2c_ram/boards/kit_psc3m5_evk_i2c_ram.dtsi
#	tests/drivers/i2c/i2c_ram/boards/kit_psc3m5_evk_psc3m5fds2afq1.overlay
#
