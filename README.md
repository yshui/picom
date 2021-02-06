[**Click here for the official picom README**](https://github.com/yshui/picom)

[**Click here for the most recent branch of this fork**](https://github.com/ibhagwan/picom/tree/next-rebase)

![picom.png](https://github.com/ibhagwan/picom/raw/next/picom.png)

## Why another picom fork?

**TL;DR:** rounded corners and dual_kawase blur on all backends.

### This fork contains:

- Dual kawase blur method from [tryone144](https://github.com/tryone144/compton) as well as his new [feature/dual_kawase branch](https://github.com/tryone144/compton/tree/feature/dual_kawase) which implements the dual kawase blur method on the experimental glx backend.

- Rounded corners code from [sdhand](https://github.com/sdhand/picom) which is also ported to the experimetnal XRender backend.

- New code for rounded corners (+borders) on the glx backend using GLSL frangment shader for both legacy and experimental backends

For more information read [my reddit post](https://www.reddit.com/r/unixporn/comments/fs8trg/oc_comptonpicom_fork_with_both_tryone144s_dual/)

## How to install

### Arch Linux

Install [picom-ibhagwan-git](https://aur.archlinux.org/packages/picom-ibhagwan-git/) from the AUR using your favorite AUR helper such as `yay`
```sh
❯ yay -S picom-ibhagwan-git
```

### Void Linux

Follow the instructions found in [picom-ibhagwan-template](https://github.com/ibhagwan/picom-ibhagwan-template)

### Build from source

Clone this repo and follow the [build instructions of the official picom README](https://github.com/yshui/picom/blob/next/README.md#build)


## 2021-02-05 Update

It's been a while since this fork had some work and the good people at [the main picom branch](https://github.com/yshui/picom) merged some of this code into the main branch.

However, not all code / features have been merged, ATM the status is as per the below:


### Included in main branch

- Rounded corners on legacy backends (both "glx" and "xrender")
- Dual-kawase blur on experimental "glx" backend only

### Not-included in main branch

- Rounded corners with "--experimental-backends"
- Rounded borders on the legacy "glx" backend
- Rounded border rules on the legacy "glx" backend
- Dual-kawase blur on the legacy "glx" backend

### Updated fork

Since this fork was released a few issues were opened for bugs that were perhaps fixed in the main branch but not on this one, while I always recommend using the main branch as a better strategy than using older forked code, since not all features were yet implemented in the main branch I thought it might still be useful to rebase this fork on the most current work of the main branch.

However, this fork has also been forked quite a few times (over 15), not being certain what work was done based on this fork or what new bugs the rebase will introduce I created a new branch ([**next-rebase**](https://github.com/ibhagwan/picom/tree/next-rebase)) for the rebased code.

To pull the latest code residing in the `next-rebase` branch:

```sh
❯ git clone --single-branch --branch next-rebase --depth=1 https://github.com/ibhagwan/picom
```
