.. _i2c_fault_injection:

I2C fault injection
###################

Overview
********

Zephyr's existing I2C tests validate that transfers succeed. This suite
validates what happens when they fail.

It injects real bus faults against a real controller and asserts three things
of every one of them: that the driver reports the failure, that it reports it
within a time bound, and that the bus and the driver are usable afterwards.
The last of those is the point of the suite. A driver that hangs rather than
returning an error fails a conventional test only by exhausting the twister
timeout, which is indistinguishable from a build or a flash problem.

The suite is vendor neutral. No board, SoC or vendor appears in the test
logic, and a board adopts it by contributing a devicetree overlay.

Hardware tiers
**************

Each tier is a superset of the one above it. Conditions the present tier
cannot produce are skipped at run time and named in the log; they are never
silently passed.

.. list-table::
   :header-rows: 1

   * - Tier
     - Fixture
     - Wiring
     - Adds
   * - 0
     - none
     - one I2C controller
     - address phase, API misuse, timing
   * - 1
     - ``i2c_bus_fault``
     - two spare GPIOs jumpered to the bus
     - bus hang, recovery, clock hold
   * - 2
     - ``i2c_bus_short``
     - a second controller shorted to the first
     - data phase, target-driven stretch, integrity, contention

Tier 0 runs on any board that declares the ``i2c-dut`` alias.

Tier 1 is the only new hardware requirement the suite introduces: one or two
jumper wires from spare GPIO pins to the controller's own SDA and SCL nets.
Both ends stay on the board under test. It closes the largest gap, because
recovery against a bus that is genuinely hung has no coverage anywhere in the
tree today.

Tier 2 uses the existing ``i2c_bus_short`` fixture, which around sixty boards
are already wired for. It reaches the faults that need a device that answers
and then misbehaves partway through a transaction, which no absent address
can produce.

Two-controller arbitration needs tiers 1 and 2 wired at once, and runs only in
the scenario that names both fixtures. The forcing GPIOs hold the bus busy
while both controllers queue a transfer, then release them together.

Wiring
******

Every connection stays on the board under test. No second board, bench
instrument or external component is needed.

**Tier 1**, one wire per line, from a spare GPIO to the controller's own bus
net:

.. code-block:: none

   <scl-gpios pin>  ------------------  SCL of the controller under test
   <sda-gpios pin>  ------------------  SDA of the controller under test

The GPIOs sit as inputs at rest and become outputs only while a line is being
held, so the rig contributes nothing to the bus between tests and an aborted
run cannot leave it stuck. Choose pins with no other function on the board,
and check they share a voltage domain with the bus before connecting them.

**Tier 2**, shorting a second controller onto the same pair:

.. code-block:: none

   second controller SCL  ------------  SCL of the controller under test
   second controller SDA  ------------  SDA of the controller under test

Both controllers must be enabled in devicetree and the fault target node
placed under the second one. The suite detects whether the target shares a
controller with the device under test and skips the contention conditions in
that case, since there is then nothing to contend with.

Devicetree contract
*******************

A board supplies an overlay under ``boards/``. Nothing else is required.

.. code-block:: devicetree

   / {
           aliases {
                   i2c-dut = &i2c0;        /* required, controller under test */
                   i2c-other = &i2c2;      /* optional, third bus for E5 */
                   /*
                    * Optional, tiers 1 and 2 together. A device on the
                    * i2c-dut bus at an address below the fault target's.
                    */
                   i2c-fault-contender = &sensor0;
           };

           zephyr,user {
                   /* Optional, tier 1. Spare GPIOs jumpered to the bus. */
                   scl-gpios = <&gpio0 4 GPIO_ACTIVE_HIGH>;
                   sda-gpios = <&gpio0 5 GPIO_ACTIVE_HIGH>;
           };
   };

   /* Optional, tier 2. A second controller shorted to the first. */
   &i2c1 {
           status = "okay";

           fault_target: fault-target@42 {
                   compatible = "zephyr,i2c-target-fault";
                   reg = <0x42>;
                   size = <32>;
           };
   };

The forcing GPIOs must be declared ``GPIO_ACTIVE_HIGH`` so that a logical zero
is a physical low. They are configured as inputs at rest and only become
outputs while a line is being held, so an aborted run cannot leave the bus
stuck.

A devicetree property says which pins would be used, not that a jumper is
fitted, so the tier 1 conditions additionally require
``CONFIG_I2C_FAULT_TEST_BUS_FAULT_WIRED``. The twister scenario carrying the
``i2c_bus_fault`` fixture sets it; a manual ``west build`` must set it by hand
once the wires are in place. Without it those conditions skip, because running
them against disconnected pins produces failures indistinguishable from driver
defects.

Controller target-mode requirements
***********************************

Tier 2 needs more of a controller than target mode alone. The faults are
produced by the return value of a target callback, so the controller carrying
the fault target must invoke ``write_received`` and ``read_processed`` while
the transaction is still on the wire and must act on what they return.

Some drivers buffer a whole write and only then replay the bytes through
``write_received``, discarding the result. On those the target cannot NACK
anything, and the injector self-test in ``main.c`` detects it: it arms a known
fault, sees that the controller acknowledged a byte the target refused, and
skips every target-driven condition with that reason printed. Tiers 0 and 1
are unaffected.

Addresses
*********

The address-phase conditions need an address that nothing on the bus answers.
The suite discovers one rather than deriving it, because a derived address is
correct only by accident on a given board. Discovery scans 0x08 through 0x77,
skipping the ranges the I2C specification reserves, and takes the first
address that does not answer. The probe is the canonical zero-length write,
so nothing on the bus is written to.

Whichever source supplies the address, it is verified silent before it is
trusted and verified again before every test, so a device coming out of reset
partway through the run is reported as such rather than appearing to be a
driver defect.

``CONFIG_I2C_FAULT_TEST_ABSENT_ADDR`` pins the address instead, which is
useful where probing would disturb a device or where the startup time of the
scan matters. ``CONFIG_I2C_FAULT_TEST_PRESENT_ADDR`` does the same for the
address used by the known-good transfers.

Standing policies
*****************

**The injector is self-testing.** Before any target-driven suite runs, the
harness arms a known fault and confirms the controller's behaviour changed.
If it did not, the dependent conditions skip. A fault injector that silently
fails must never be reported as a passing run.

**Error codes are asserted strictly by default.** The I2C API documents
``-EIO`` for a failed transfer and the existing upstream suites assert that
exact value, so it is what this requires.
``CONFIG_I2C_FAULT_TEST_RELAXED_ERRNO`` weakens it to any negative value for
bringing up a driver that is not yet conformant. In both modes the value each
driver actually returned is printed per fault class, so the question of
whether finer-grained error reporting is wanted can be answered with data.

**Timing assertions are diagnostic.** Bounds are loose and a measurement
outside them is marked in the log rather than failed, unless
``CONFIG_I2C_FAULT_TEST_TIMING_ASSERT`` is set. A suite that fails
intermittently on timing across a dozen platforms is worse than no suite.

**Bounded failure is the through-line.** Faulted transfers run on a worker
thread that the test thread waits on with
``CONFIG_I2C_FAULT_TEST_TIMEOUT_MS``. A transfer that outruns the bound is
reported as a hang. Where the stimulus can be removed, the line is released
and the worker given a chance to unwind, so one hanging driver does not take
the rest of the run with it.

Building and running
********************

Tier 0 needs no wiring and runs on any board declaring the ``i2c-dut`` alias:

.. code-block:: console

   west build -p auto -b <board> tests/drivers/i2c/i2c_fault_injection
   west flash

Tier 1 additionally requires the wiring to be declared, not merely present,
because a devicetree property names pins rather than confirming a jumper:

.. code-block:: console

   west build -p auto -b <board> tests/drivers/i2c/i2c_fault_injection \
           -- -DCONFIG_I2C_FAULT_TEST_BUS_FAULT_WIRED=y

Or through twister, which selects the scenario matching the wiring declared
for the board and sets that symbol for you:

.. code-block:: console

   ./scripts/twister -p <board> -T tests/drivers/i2c/i2c_fault_injection \
           -X i2c_bus_fault -X i2c_bus_short

Pass only the fixtures actually wired.

Pausing for an instrument
*************************

Capturing a single event on an oscilloscope is awkward because the suite
performs hundreds of legitimate transfers before reaching any particular
condition, and an edge trigger cannot tell them apart. The suite can instead
halt and wait for a console keypress, which leaves the bus silent until the
operator is ready. The first edge after the keypress is then the event under
investigation, and an ordinary single-shot edge trigger becomes unambiguous
without a spare channel or a trigger pin.

Add the call as the first statement of the condition to be captured, before
any bus activity it performs:

.. code-block:: c

   ZTEST(i2c_fault_recovery, test_recovery_idempotent)
   {
           i2c_fault_scope_pause("D6, the first recovery attempt");
           ...
   }

Then build with the fragment, which sets the suite's own symbol together with
the two console symbols it cannot select for itself:

.. code-block:: console

   west build -p always -b <board> tests/drivers/i2c/i2c_fault_injection \
           -- -DEXTRA_CONF_FILE=scope_pause.conf

``CONFIG_CONSOLE_GETCHAR`` belongs to an optional Kconfig choice, so
``CONFIG_I2C_FAULT_TEST_SCOPE_PAUSE`` cannot select it. Enabling the pause
without it fails the build rather than producing a run that never pauses.

A terminal that can send keystrokes is required; a logger that only reads will
wait indefinitely.

**This makes the suite interactive.** The symbol is off by default and no
scenario in ``tests.yaml`` enables it. Never enable it for a twister run or
any other unattended execution.

No call sites are kept in the tree. A pause belongs to one investigation, so
add it where it is wanted and remove it once that question is answered.

This is bring-up tooling, not a feature of the suite. It is scheduled for
removal in full before upstream submission; see open item 12 in
``i2c_fault_injection_status.md``.

Reading a run
*************

The suite prints what it found before running anything. Check this block
first; the results after it are only meaningful if it is right.

.. code-block:: none

   I2C fault injection environment
     controller under test: i2c@40000000
     absent address:        0x09
     known-good address:    0x18
     line forcing:          available
     fault target:          ready

The last two lines report the tier actually reached, which may be lower than
the wiring suggests if a Kconfig was missed. Anything reported as unavailable
causes the conditions needing it to skip with that reason named.

Each condition prints the value the driver returned:

.. code-block:: none

     A1 address NACK on write: driver returned -5

Those lines are a deliverable rather than debug output. The suite exists partly
to collect what drivers actually return per fault class, so keep the full log.

A driver that fails to return is reported explicitly rather than as a twister
timeout:

.. code-block:: none

     D1 SDA held low: transfer did not return within 2000 ms; the driver hung
     rather than reporting an error

After that the remaining conditions skip, because the worker thread is parked
inside the driver and the bus state is unknown.

Troubleshooting
***************

.. list-table::
   :header-rows: 1

   * - Symptom
     - Cause
   * - ``controller under test is not ready``
     - The ``i2c-dut`` alias names a node that is disabled, or the overlay was
       not picked up. Check the build log names the expected overlay file.
   * - ``could not find an unused I2C address``
     - Every address answered. Normally this means SDA is stuck low, so every
       probe reads as an acknowledge. Check the bus before going further.
   * - ``known-good address: none found``
     - Nothing on the bus answered. The conditions needing a working transfer
       skip. Usable, but confirm the controller reaches its pins.
   * - ``SCL reads low on an idle bus``
     - A forcing jumper is on the wrong net, or the bus has no pull-ups. The
       check runs at setup precisely so this is not mistaken for a driver
       defect later.
   * - ``line forcing: not wired`` with jumpers fitted
     - ``CONFIG_I2C_FAULT_TEST_BUS_FAULT_WIRED`` was not set. See above.
   * - ``fault target: registered, cannot refuse a byte``
     - The controller carrying the target does not act on callback return
       values. See the section on target-mode requirements. Not a rig fault.
   * - An address that answers partway through the run
     - A device came out of reset. Pin a known-silent address with
       ``CONFIG_I2C_FAULT_TEST_ABSENT_ADDR``.
