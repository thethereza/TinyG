TinyG Introduction
========

![TinyG v7 Board](http://farm9.staticflickr.com/8186/8436183471_6b3708cd0d_c.jpg)

TinyG is a 6 axis motion control system designed for high-performance on small to mid-sized machines. Some features:

* 6 axis motion (XYXABC axes)
* jerk controlled motion for acceleration planning (3rd order motion planning)
* status displays ('?' character)
* XON/XOFF and RTS/CTS protocol over USB serial
* RESTful interface using JSON

See these links for more details.

* [Synthetos](https://www.synthetos.com/)
* [TinyG Wiki](https://github.com/synthetos/TinyG/wiki)
* [TinyG Support Forum](https://www.synthetos.com/forum/tinyg/)
* [TinyG Github](https://github.com/synthetos/TinyG)
* [Synthetos Web Store](https://www.synthetos.com/webstore/)

**See the new TinyG G2 [Arduino Due ARM Port](https://github.com/synthetos/g2) and the [G2 wiki](https://github.com/synthetos/g2/wiki)**


CURRENT MASTER VERSION
========
The current master version is 0.96, BUILD 380.08
Please refer to the [TinyG wiki](https://github.com/synthetos/TinyG/wiki) for documentation and user manuals

**IMPORTANT: If you are running a TinyG version 6 or earlier be sure to set the $hv value to 6 after you load version 0.95. 
Run $hv=6 and verify that it took. 
This addresses HW changes between the versions.**

Tinyg versions 7 and 8 are electrically the same, and can be set to HW version 8.

The previous master version is 0.95, BUILD 370.01

If you have feature requests or find any bugs please log them in the Issues tab on the github


========
BRANCHES
========

* MASTER is the current production code. This is not updated very frequently.
* EDGE branch is thought to be relatively stable, but has not received enough testing to be promoted to master.
* DEV is work in process and generally not a good idea to use. Use code from dev only if specifically directed there.
