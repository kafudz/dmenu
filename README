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

# dmenu (with app launcher)

A simple, keyboard-driven menu for Linux. Press a key, type a few letters,
hit Enter — done.

This version can also **launch your installed applications** by name, so you
don't need to remember commands or click through menus.

> This is a personal project, shared in case it's useful to others. It's based
> on [dmenu](https://tools.suckless.org/dmenu/) by suckless.org and on a fork by
> [kafudz](https://github.com/kafudz/dmenu). It is not an official dmenu release.

---

## What it does

- **Type to search.** Everything is keyboard-driven: no mouse needed.
- **Launch your apps.** It reads the application shortcuts installed on your
  system and lets you pick one by name.
- **Works with any list.** You can also feed it any list of items (files,
  commands, anything) and pick one.

Think of it as a "start menu" you open with a keyboard shortcut.

---

## What's different about this version

Two things:

### 1. It's safer

The version this is based on ran each app's launch command through a system
shell. That works, but it means a malicious or badly-written shortcut file could
sneak in extra commands. This version doesn't use a shell at all — it runs the
program directly, the way your system intended.

In plain terms: **the app you pick is the app that runs. Nothing else.**

### 2. It includes important bug fixes

The original dmenu project released a set of fixes in late 2026 for crashes and
memory problems. Those fixes weren't included in the fork this is based on, so
I've added them here.

---

## Installing

You'll need a few common libraries and a C compiler. If you've installed
software from source before, this will look familiar.

**Debian / Ubuntu / Linux Mint:**

```sh
sudo apt install build-essential libx11-dev libxft-dev libxinerama-dev \
                 libfontconfig1-dev libfreetype6-dev pkg-config
