# ykern Metadata Schema (v1)

ykern reads hand-curated YAML overlays alongside what the kernel reports via
runtime introspection. The overlay attaches names, summaries, and longer help
to numeric ids the kernel returns but doesn't label (operation cmd ids,
attribute ids, etc.). This document defines the overlay format.

## Layout

Each generic-netlink family has up to two files. The split keeps mechanical
content (cmd_id ↔ name bindings extracted from kernel headers) separate
from human prose (descriptions written by hand):

```
metadata/
└── netlink/
    └── genl/
        ├── _generated/         ← machine-generated; do not edit
        │   ├── ethtool.yaml    ← cmd_id → name, regenerated from headers
        │   └── nl80211.yaml
        ├── ethtool.yaml        ← hand-curated short / long descriptions
        └── nl80211.yaml
```

The generated files are produced by
`tools/libclang/extract-netlink/extract-netlink.py` (driven by `make
regen-metadata-<family>`), which walks the kernel UAPI header with
libclang and emits each cmd_id and its enumerator name. Re-running the
tool after a kernel update is the supported way to track new operations.

The curated files are written by hand and contain only descriptions —
keyed by **op name**, not by cmd_id, so authors don't need to look ids
up. The loader merges the two by name at runtime.

The directory root is resolved at runtime in this order:

1. `$YKERN_METADATA_DIR` if set
2. Compile-time `YKERN_METADATA_DIR` (cmake injects `${CMAKE_SOURCE_DIR}/metadata`)

A family with no overlay files is fine — ykern just falls back to the
synthetic names (`cmd-<id>`, `group-<id>`). Either file alone also works:
generated alone gives names without descriptions; curated alone (without
generated) is parsed but operation entries can't bind to cmd_ids and are
dropped with a warning.

## Schemas

### Generated file — `_generated/<family>.yaml`

Mechanical, regenerated from kernel headers. Keys are integer cmd ids;
each entry has a single `name:` field, no descriptions.

```yaml
# GENERATED — DO NOT EDIT BY HAND.
family: ethtool

operations:
  1: { name: STRSET_GET }
  2: { name: LINKINFO_GET }
  ...
```

### Curated file — `<family>.yaml`

Hand-written. Keys for the `operations` map are op **names**; entries
contain only `short` and `long`. Mcast groups are keyed by their
kernel-assigned name. Family-level `short`/`long` go here too.

```yaml
family: ethtool

short: Ethtool — modern NIC configuration via netlink
long: |
  Multi-paragraph help text. Shown by `ykern -l /netlink/genl/ethtool`.

operations:
  STRSET_GET:
    short: Fetch a labeled string set
    long: |
      Long help. Optional.
  LINKINFO_GET:
    short: Read link information

mcast_groups:
  monitor:
    short: Configuration change events
    long: |
      Long help. Optional.
```

### Field semantics

| Path                            | Required | Type   | Notes                                          |
|---------------------------------|----------|--------|------------------------------------------------|
| `family`                        | yes      | string | Must match the kernel-reported family name     |
| `short`                         | no       | string | Overrides the auto family short description    |
| `long`                          | no       | string | Overrides the auto family long description     |
| `operations.<NAME>.short`       | no       | string | One-line per-op description                    |
| `operations.<NAME>.long`        | no       | string | Multi-line per-op help                         |
| `mcast_groups.<NAME>.short`     | no       | string | One-line per-group description                 |
| `mcast_groups.<NAME>.long`      | no       | string | Multi-line per-group help                      |

In the generated file, `operations.<id>.name` is the only field — and
nothing else belongs there.

### Multicast-group key — kernel-assigned name

The kernel returns mcast group names in `CTRL_ATTR_MCAST_GRP_NAME`. The
overlay matches against that name verbatim. Groups with no kernel-assigned
name are matched by `group-<id>` instead.

## Path resolution

When an operation has an overlay name, that name becomes its canonical path
segment. Both forms below address the same operation:

- `/netlink/genl/ethtool/STRSET_GET`   ← post-overlay canonical
- `/netlink/genl/ethtool/cmd-1`        ← pre-overlay synthetic (still resolves)

ykern accepts either at the CLI; the canonical form is what `get_name()`
returns and what `get_path()` builds.

## Style guidelines

- Keep `short` to one line, ~80 chars max.
- Use `|` block scalars for `long`; preserve newlines deliberately.
- Don't repeat the cmd id or family name inside the description text — the
  surrounding listing already shows it.
- Prefer kernel header constant names verbatim for `name` (e.g.
  `STRSET_GET`, not `strset_get` or `Get string set`). Match the constants
  in `<linux/ethtool_netlink.h>`, `<linux/nl80211.h>`, etc., minus the
  `<FAMILY>_CMD_` prefix.
