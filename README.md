dmenu - dynamic menu
====================
dmenu is an efficient dynamic menu for X.


Requirements
------------
In order to build dmenu you need the Xlib header files.


Installation
------------
Edit config.mk to match your local setup (dmenu is installed into
the /usr/local namespace by default).

Afterwards enter the following command to build and install dmenu
(if necessary as root):

    make clean install


Running dmenu
-------------
See the man page for details.

# dmenu (personal fork)
-----------------------
My personal fork of [dmenu](https://tools.suckless.org/dmenu/), the dynamic menu
for X from [suckless.org](https://suckless.org).

This is a **personal-use fork**. It is tuned for my machine, my fonts, and my
keybindings. It is not maintained as a general-purpose distribution — no issues
or pull requests are accepted. If you want dmenu, use upstream:

    git clone https://git.suckless.org/dmenu

Upstream license (MIT/X Consortium) is preserved; see [LICENSE](LICENSE).
