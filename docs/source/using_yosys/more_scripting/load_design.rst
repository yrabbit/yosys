Loading a design
~~~~~~~~~~~~~~~~

.. TODO:: fill out this page better

keyword: Frontends

- :doc:`/cmd/index_frontends`

.. todo:: include ``read_verilog <<EOF``, also other methods of loading designs

.. code-block:: yoscrypt

    read_verilog file1.v
    read_verilog -I include_dir -D enable_foo -D WIDTH=12 file2.v
    read_verilog -lib cell_library.v

    verilog_defaults -add -I include_dir
    read_verilog file3.v
    read_verilog file4.v
    verilog_defaults -clear

    verilog_defaults -push
    verilog_defaults -add -I include_dir
    read_verilog file5.v
    read_verilog file6.v
    verilog_defaults -pop

.. todo:: more info on other ``read_*`` commands, also is this the first time we
   mention verific?

.. note::

   The Verific frontend for Yosys, which provides the `verific` command,
   requires Yosys to be built with Verific.  For full functionality, custom
   modifications to the Verific source code from YosysHQ are required, but
   limited useability can be achieved with some stock Verific builds.  Check
   :doc:`/yosys_internals/extending_yosys/build_verific` for more.

Others:

- `GHDL plugin`_ for VHDL

.. _GHDL plugin: https://github.com/ghdl/ghdl-yosys-plugin
