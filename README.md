# finder-kde

**Finder for KDE Plasma**: KDE's [Dolphin](https://apps.kde.org/dolphin/)
file manager rebuilt to look and work like the macOS (Golden Gate) Finder.
It installs as a separate app called Finder, next to stock Dolphin, with its
own settings.

Based on Dolphin 26.08.1. Every Finder change is a commit on top of the
`v26.08.1` tag, and the upstream code is marked `Finder:` where it was changed.

## What's different from Dolphin

- **Window**: frameless, rounded, with its own stoplights (close, minimize,
  zoom), a native shadow, and a translucent glass sidebar that runs to the
  top of the window. Drag empty toolbar space to move; double-click to zoom.
- **Toolbar**: glass capsules for back/forward, the view modes (icons, list,
  columns), Group By, a "…" actions menu and search. The folder name is
  shown as a bold title; click it (or Ctrl+L) to type a path.
- **Sidebar**: Recents, Favorites and Locations, with accent-tinted icons,
  rounded selection, mount-on-click for drives, and AirDrop.
- **Icon view**: Finder's selection (a gray tile behind the icon and a blue
  pill behind the name), no hover or focus boxes.
- **List view**: Name / Date Modified / Size / Kind columns, flat header
  with the sort column in bold, zebra stripes that fill the view, full-row
  blue selection, disclosure chevrons, "Today at 4:49 PM" dates and decimal
  sizes ("245 KB", "--" for folders).
- **Column view** (new): Miller columns with a preview column, its own
  context menu, and a title and footer that follow the active column.
- **Footer**: "16 items, 727 GB available" / "1 of 16 selected", with the
  zoom slider.
- **AirDrop** with iPhones and Macs: AirDrop in the sidebar (Ctrl+Shift+R),
  right-click › Share › AirDrop, progress cards for sending and receiving,
  and notifications. See below.

## Install

```sh
finder-extras/install.sh --deps   # first time: build dependencies (Fedora)
finder-extras/install.sh          # build and install into ~/.local/opt/finder
```

Finder then shows up in your app menu. To make it the default file manager:

```sh
xdg-mime default org.brhett.finder.desktop inode/directory
```

## AirDrop

AirDrop runs on [omdrop-owl](https://github.com/t4t5/omdrop-owl), the open
AirDrop radio for non-Apple Wi-Fi cards, and
[omdrop](https://github.com/netmojo-systems/omdrop-plugin). On Fedora,
`finder-extras/install-airdrop` builds and installs both and opens the
AirDrop port in firewalld.

It depends on your Wi-Fi card (see omdrop-owl's hardware table). On most
Intel cards AirDrop takes the card over, so **Wi-Fi disconnects while
AirDrop is on** and reconnects afterwards. Finder warns before turning it on.

## Upstream

Dolphin is © the KDE community, and Finder keeps its license
(GPL-2.0-or-later; see [COPYING](COPYING) and [LICENSES](LICENSES)). The
original README is in [README.dolphin.md](README.dolphin.md). Finder is not
affiliated with KDE or Apple.
