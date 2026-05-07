# ykern tutorial — read your NIC's link info via ethtool

A 10-minute walk through ykern from the root to a real kernel reply.
Every command is copy-paste; nothing assumes prior netlink knowledge.

The build directory is `build-desktop-ytrace-release` after `make build-desktop-ytrace-release`.
Replace it with whichever one you actually built.

---

## 1. Start at the top

```
$ ykern browse /
```

You see one transport child (`netlink`). Read the **What you can do here**
section that the tool prints — every node prints one. From the root the
move is to drill into a transport.

## 2. Drill into netlink

```
$ ykern browse /netlink
```

Netlink groups its protocols under namespaces. Right now only `genl`
(generic netlink) is wired. Drill in.

## 3. Drill into genl

```
$ ykern browse /netlink/genl
```

24 families show up. These are the kernel-side feature areas (ethtool,
nl80211, devlink, tcp_metrics, …). Pick `ethtool`.

## 4. Look at ethtool

```
$ ykern browse /netlink/genl/ethtool
```

You get a description from the curated YAML overlay, plus a list of
children — operations like `STRSET_GET`, `LINKINFO_GET`, `LINKMODES_GET`,
and the `monitor` multicast group.

Each `[operation]` child is **something you can call**. We'll use
`LINKINFO_GET` because every NIC supports it.

## 5. Inspect the operation

```
$ ykern browse /netlink/genl/ethtool/LINKINFO_GET
```

Three things matter in the output:

1. The `desc` line tells you *what the operation does* in one sentence.
2. The **Invoke** block shows the exact command line, plus example
   arguments (`ifname` / `ifindex` / no-arg dump).
3. The **children** list shows every attribute name in the LINKINFO
   attribute set. *That is what the kernel will return in its reply.*

> **Why are attributes children, and what does it mean to "drill into" one?**
>
> Each operation works with a fixed *attribute set* (the kernel groups
> related attributes into named bundles). ykern shows that set as the
> children of the operation so you can look up names in one place.
>
> Drilling into an attribute (`ykern browse .../HEADER`) prints what we
> know about that one attribute: its kernel id, the set it belongs to,
> and the exact `--arg ... =<VALUE>` line that uses it.

## 6. Invoke for a real NIC

Pick any interface name from `ip -br link` (or `ls /sys/class/net`).
Below uses `eth1g` — substitute yours.

```
$ ykern invoke /netlink/genl/ethtool/LINKINFO_GET --arg ifname=eth1g
LINKINFO_GET reply (cmd=2, family=ethtool id=22, set=LINKINFO):
  HEADER:
    DEV_INDEX = 2  (u32, 0x00000002)
    DEV_NAME = "eth1g"  (string, 6 bytes)
  PORT = 0  (u8)
  PHYADDR = 0  (u8)
  TP_MDIX = 0  (u8)
  TP_MDIX_CTRL = 0  (u8)
  TRANSCEIVER = 1  (u8)
```

Each line is one attribute the kernel returned:

- `HEADER` is a nested wrapper that echoes which device the reply is
  for. It contains `DEV_INDEX` and `DEV_NAME` from the *HEADER* set
  (a different set, used to identify NICs across all ethtool ops).
- `PORT`, `PHYADDR`, … are the *LINKINFO*-set attributes — actual
  link-level state.

## 7. Read everything in one shot — DUMP

Omit the `--arg` and the kernel walks every NIC for you:

```
$ ykern invoke /netlink/genl/ethtool/LINKINFO_GET

LINKINFO_GET reply (cmd=2, family=ethtool id=22, set=LINKINFO) [DUMP]:

--- entry 1 ---
  HEADER: …    PORT = 0  (u8)  …
--- entry 2 ---
  HEADER: …    PORT = 0  (u8)  …
…
```

Same shape, one entry per interface. Most ethtool `*_GET` ops support this.

## 8. The two ways to identify a NIC

ethtool always wants its `HEADER` to point at a specific NIC. The
dispatcher accepts either form:

```
ykern invoke /netlink/genl/ethtool/LINKINFO_GET --arg ifname=eth1g
ykern invoke /netlink/genl/ethtool/LINKINFO_GET --arg ifindex=2
```

These are not arbitrary names — they map to `ETHTOOL_A_HEADER_DEV_NAME`
(string) and `ETHTOOL_A_HEADER_DEV_INDEX` (u32) in the kernel header.
The dispatcher knows ethtool wraps both inside a nested `HEADER`
attribute, so you never have to construct that manually.

## 9. Try a few more reads

Same recipe for every ethtool `*_GET`:

```
ykern invoke /netlink/genl/ethtool/LINKMODES_GET  --arg ifname=eth1g
ykern invoke /netlink/genl/ethtool/LINKSTATE_GET  --arg ifname=eth1g
ykern invoke /netlink/genl/ethtool/RINGS_GET      --arg ifname=eth1g
ykern invoke /netlink/genl/ethtool/COALESCE_GET   --arg ifname=eth1g
ykern invoke /netlink/genl/ethtool/PAUSE_GET      --arg ifname=eth1g
ykern invoke /netlink/genl/ethtool/FEATURES_GET   --arg ifname=eth1g
```

`FEATURES_GET` and `LINKMODES_GET` use *bitsets* under the hood; ykern
decodes them automatically. Each NIC feature comes back as one line,
`name = on` / `name = off`.

## 10. When something goes wrong

If the kernel rejects, ykern surfaces the errno verbatim:

```
LINKINFO_GET: kernel returned errno 19 (No such device)
```

Common ones:

| errno | Likely meaning                                                  |
|-------|-----------------------------------------------------------------|
|  19   | Bad ifname / ifindex                                            |
|   1   | Need root (`sudo ykern invoke …`)                               |
|  95   | The driver doesn't implement this op for this NIC               |
|  22   | Bad arguments — the kernel expected a different attribute shape |

These are *kernel decisions*, not ykern bugs.

---

## Beyond ethtool

The same flow works for every family that has overlay metadata. Try:

```
ykern browse /netlink/genl/tcp_metrics                  # what it exposes
ykern browse /netlink/genl/tcp_metrics/GET              # accepted args
ykern invoke /netlink/genl/tcp_metrics/GET              # dump every metric
ykern invoke /netlink/genl/nl80211/GET_INTERFACE        # dump every wifi iface
ykern invoke /netlink/genl/devlink/GET                  # dump devlink devices
```

The browse output of every operation tells you which `--arg KEYs` it
accepts. The kernel does the rest.

## Cheat sheet

| Goal                          | Command                                          |
|-------------------------------|--------------------------------------------------|
| See what's reachable          | `ykern browse /`                                 |
| Drill in                      | `ykern browse <PATH>`                            |
| More detail                   | `ykern browse -l <PATH>` (repeat: `-ll`, `-lll`) |
| Dump the whole subtree        | `ykern browse -r <PATH>`                         |
| List children only            | `ykern browse -L <PATH>`                         |
| Call an operation             | `ykern invoke <PATH> --arg KEY=VALUE [...]`      |
| Dump every object of an op    | `ykern invoke <PATH>` (no `--arg`)               |

Mnemonic: every command starts with **a verb** — `browse` (read ykern's
own metadata, no kernel I/O) or `invoke` (send a real netlink request).
