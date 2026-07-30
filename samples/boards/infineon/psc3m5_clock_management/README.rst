.. _psc3m5_clock_management:

PSC3M5 Clock Management RFC Prototype
#####################################

Overview
********

Evaluation prototype of the Zephyr clock-management RFC (PR #89124,
``rfc/clock-mgmt-drivers``) on the low-resource Infineon PSC3M5 CAT1B target.

The clock tree is modelled at SoC scope in
:zephyr_file:`dts/arm/infineon/cat1b/psc3/system_clocks_cm.dtsi`, which the
board includes: six root sources, seven clock paths (FLL on path 0, DPLL0 on
path 1, DPLL1 on path 2), and the seven ``clk_hf`` roots. This sample adds only
a consumer node, and the application reports what the framework tells it.

The application includes no vendor clock header and touches no register. Every
value it prints is obtained through ``clock_management_get_rate()``,
``clock_management_apply_state()``, and devicetree properties, which is the
producer/consumer split the RFC is being evaluated for.

Ownership model
***************

The port draws the ownership boundary at ``clk_hf``:

* **Clock management owns the shared upstream** - sources, path multiplexers,
  the FLL and DPLLs, and the ``clk_hf`` roots.
* **Peripheral drivers own their leaf divider** - the peripheral divider that
  turns a ``clk_hf`` into a bit rate stays on the legacy ``infineon,peri-div``
  clock-control path, because baud and bit timing are the driver's business.

The sample prints both halves for the console UART so the boundary is visible
at run time: the ``clk_hf2`` rate comes from clock management, while the
divider value comes from the UART's own devicetree node.

What the sample reports
***********************

#. The CPU clock frequency declared in devicetree and the kernel timer
   frequency.
#. Every ``clk_hf`` root that the tree exposes a ``clock-output`` for, with the
   peripherals each root feeds.
#. The clock-management / driver ownership boundary for the console UART.
#. The rate a state *declares* against the rate the producers *compute*. In
   static mode ``apply_state()`` returns the ``clock-frequency`` written in
   devicetree while ``get_rate()`` recomputes it from the divider chain, so a
   mismatch means the declared value disagrees with the producer math.
#. With ``CONFIG_CLOCK_MANAGEMENT_RUNTIME=y``, a frequency-range request
   negotiated through ``clock_management_req_rate()``.

Building and running
********************

Static configuration (``CONFIG_CLOCK_MANAGEMENT_RUNTIME`` disabled)::

   west build -p auto -b kit_psc3m5_evk \
       samples/boards/infineon/psc3m5_clock_management

Runtime configuration (adds ``CONFIG_CLOCK_MANAGEMENT_RUNTIME=y``)::

   west build -p auto -b kit_psc3m5_evk \
       samples/boards/infineon/psc3m5_clock_management \
       -- -DEXTRA_CONF_FILE=runtime.conf

Producer backend A/B (same devicetree, same sample)::

   # Track A: PDL runtime backend (default)
   west build -p auto -b kit_psc3m5_evk \
       samples/boards/infineon/psc3m5_clock_management -d build_pdl

   # Track B: register-direct backend (family-agnostic, addresses from DT)
   west build -p auto -b kit_psc3m5_evk \
       samples/boards/infineon/psc3m5_clock_management -d build_reg \
       -- -DEXTRA_CONF_FILE=regdirect.conf

   arm-zephyr-eabi-size build_pdl/zephyr/zephyr.elf build_reg/zephyr/zephyr.elf

Expected output
***************

::

   PSC3M5 full-shape clock-management demo
   CPU clock declared in devicetree: 180000000 Hz
   Kernel timer frequency: 32768 Hz

   High-frequency clock roots (read through clock management)
     clk_hf0   180000000 Hz  CPU, PERI, SRSS, groups 0/2
     clk_hf1   180000000 Hz  LPCOMP, groups 1/3
     clk_hf2    80000000 Hz  group 4: console UART, CAN
     clk_hf3   240000000 Hz  HPPASS, group 5: TCPWM, CSG
     clk_hf4    96000000 Hz  group 6: SCB5 SPI

   Ownership boundary for the console UART
     clk_hf2 source rate (clock management)   80000000 Hz
     leaf divider /86 (UART driver)             930232 Hz
     configured baud rate                       115200

   Applying the clk_hf2 default state
     declared rate (apply_state)   80000000 Hz
     computed rate (get_rate)      80000000 Hz
     declared == computed

   clock-management demo complete

The ``clk_hf1``, ``clk_hf3``, and ``clk_hf4`` roots are brought up at boot by
the transitional legacy bring-up
(``CONFIG_CLOCK_MANAGEMENT_INFINEON_CAT1_LEGACY_BRINGUP``), because the drivers
they feed are not yet clock-management consumers. ``clk_hf0`` is applied by the
SoC on behalf of the CPU, and ``clk_hf2`` by the console UART.

The kernel timer frequency is **not** the CPU clock: this board selects the
Infineon low-power timer (``CONFIG_USE_INFINEON_LPTIMER=y``), so
``CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC`` is 32768 and the tick is derived from the
WCO, not from ``clk_hf0``. That is why the sample prints the two values
separately rather than assuming they agree.
