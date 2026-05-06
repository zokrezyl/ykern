# ykern

A unified, browsable, self-describing view of every Linux kernel interface.

## The problem

Linux exposes its state and configuration through a sprawl of unrelated
interfaces — `/proc`, `/sys`, classic netlink, generic netlink, ioctl,
configfs, debugfs, sysctl. Each has its own protocol, conventions,
discovery mechanism, and learning curve. Documentation lives scattered
across kernel source trees, mailing-list archives, and a handful of
single-purpose tools (`ip`, `ethtool`, `iw`, `sysctl`) that each cover
one narrow slice.

The consequence: anyone who wants to understand or manipulate what the
kernel is doing must learn a different idiom for every subsystem. There
is no single place to ask *what can I see, what can I change, what does
this mean?* Building a GUI on top of even a useful subset means
hand-writing a wrapper per family — huge effort, brittle output,
obsolete the moment the kernel grows a new interface.

## What ykern is

ykern is a C library that presents the entire kernel surface as one
tree of polymorphic objects. Every node knows its name, its path, what
kind of thing it is, and what it means in plain language. Every node
can be browsed lazily and described uniformly, regardless of which
transport carries it underneath.

Where the kernel can already describe itself — generic netlink does,
partially — ykern uses that. Where it can't, a hand-curated YAML
overlay fills in the gap with help text and intent. The two sources
merge into one consistent view.

The library is designed to be linked directly by both CLIs and GUIs.
Paths are stable strings, so a GUI can map them to layout and styling
the way a stylesheet maps selectors to rules. Read access lands first;
controlled mutation is reserved for v2, but the API shape is already
prepared for it.

## Status

Early. Generic netlink is the first transport under construction.
Everything else — rtnetlink, `/proc`, `/sys`, and beyond — follows the
same shape.
